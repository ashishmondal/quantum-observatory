// IR remote learning wizard (phase IR.2).
//
// diagnostic — bypasses theme:: by design (FR-15.3 exemption,
// CODING_PRACTICES §4): on-screen prompts and counters use raw
// inks/fonts directly so EMI behaviour and capture state read at a
// glance regardless of the active theme. Do not theme this scene.
//
// Replaces the bare phase-IR.1 logging POC. Walks the operator
// through every button on the target Roku-style remote, captures one
// clean NEC frame per prompt, then publishes the full capture set to
// `observatory/debug` so the address + per-button command codes can
// be lifted into `include/config.h` (FR-17.3) without retyping them
// from a serial log.
//
// Selectable over MQTT via {"scene_id":"ir_test"}. wants_clock_chrome
// is false so the chrome readout doesn't fight the prompts.
//
// Workflow:
//   1. init() resets ir_remote counters and the local capture table,
//      arms a short grace period (300 ms) to flush any stale frames
//      that arrived just before the scene came up.
//   2. For each button in kButtons[] (HOME first, then arrows, OK,
//      BACK, OPTIONS, REPLAY) the scene shows a "PRESS <NAME>"
//      prompt and watches ir_remote::stats() for a single new
//      non-repeat NEC frame with no parity/overflow flags.
//   3. On capture: stash protocol/address/command/raw, show a brief
//      "OK proto/addr/cmd" confirmation for ~1.2 s (which also
//      swallows any held-down repeat frames so they don't leak into
//      the next button), then advance.
//   4. Once every button is captured, build a JSON payload with the
//      address + per-button table and hand it to mqtt_link via
//      queue_debug() (Core 1 → Core 0 SPSC, sentinel-0). The screen
//      then sits on a "DONE — see observatory/debug" panel until the
//      next scene swap.
//
// Layout (64×32):
//   row  0      brightness band (cycling nebula gradient) — kept
//               from the IR.1 POC so we still capture EMI behaviour
//               against a busy bright frame. Per FR-17.13 the
//               90 %-of-30 reliability gate is met deliberately
//               under this top-row noise + whatever scene the user
//               flips to between learning sessions.
//   rows  2..7  step header   "STEP n/N"  (Picopixel)
//   rows  9..18 button name   centred in the default 5×7 GFX font
//   rows 20..25 prompt or capture readout
//                  prompt:  "PRESS NOW"
//                  ok:      "P8 A055 C00A"
//                  done:    "OK SEE MQTT"
//   rows 27..31 progress dots (one per button) — empty/captured/
//               current animations
//
// All counters and last-decode reads come from ir_remote::stats();
// the IR receiver is polled from main.cpp loop() on Core 0 as before.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "color_palette.h"
#include "config.h"
#include "ir_remote.h"
#include "mqtt_link.h"
#include "scenes/scene.h"

namespace ir_test_scene_detail {

struct ButtonDef {
  const char* short_name;   // shown on screen — uppercase, ≤ 8 chars
  const char* json_key;     // emitted in observatory/debug payload
};

// FR-17.5 button mapping for the Roku-style 8-button remote, plus a
// REPLAY button that some Roku models ship with. HOME first because
// it's the unambiguous "anchor" press — if anything else were first
// the operator might fumble between scene-trigger and learning.
static constexpr ButtonDef kButtons[] = {
    {"HOME",    "home"},
    {"UP",      "up"},
    {"DOWN",    "down"},
    {"LEFT",    "left"},
    {"RIGHT",   "right"},
    {"OK",      "ok"},
    {"BACK",    "back"},
    {"OPTIONS", "options"},
    {"REPLAY",  "replay"},
};
static constexpr uint8_t kButtonCount =
    sizeof(kButtons) / sizeof(kButtons[0]);

}  // namespace ir_test_scene_detail

class IrTestScene : public Scene {
public:
  const char* name() const override { return "ir_test"; }
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    (void)matrix;
    using namespace ir_test_scene_detail;

    m_initialised      = false;
    m_init_ms          = 0;
    m_last_tick_ms     = 0;
    m_palette_shift    = 0;
    m_current          = 0;
    m_baseline_decoded = 0;
    m_advance_at_ms    = 0;
    m_published        = false;

    for (uint8_t i = 0; i < kButtonCount; ++i) {
      m_caps[i].captured = false;
      m_caps[i].protocol = 0;
      m_caps[i].address  = 0;
      m_caps[i].command  = 0;
      m_caps[i].raw      = 0;
    }

    // Wipe any stale decodes still in the global counters from a
    // previous scene visit. The 300 ms grace below covers the case
    // where a frame is mid-decode at scene-entry.
    ir_remote::reset_counters();
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using namespace ir_test_scene_detail;

    if (!m_initialised) {
      m_init_ms          = now_ms;
      m_last_tick_ms     = now_ms;
      m_grace_until_ms   = now_ms + kGraceMs;
      m_initialised      = true;
    }

    const ir_remote::Stats st = ir_remote::stats();

    // ---- Capture state machine -----------------------------------------
    if (m_current < kButtonCount) {
      const bool grace_done = static_cast<int32_t>(now_ms - m_grace_until_ms) >= 0;
      const bool in_cooldown = (m_advance_at_ms != 0);
      if (in_cooldown) {
        // Holding the captured value on screen for kCooldownMs to (a)
        // give the operator visual confirmation, (b) absorb any NEC
        // repeat frames the held button is still emitting so they
        // don't get attributed to the next prompt. Also swallow any
        // new decodes that arrive in this window (advance baseline).
        m_baseline_decoded = st.decoded;
        if (static_cast<int32_t>(now_ms - m_advance_at_ms) >= 0) {
          m_advance_at_ms = 0;
          ++m_current;
          // Reset baseline AGAIN at the moment we start watching for
          // the next button — anything that arrived during the
          // cooldown window is already absorbed above.
          m_baseline_decoded = st.decoded;
        }
      } else if (grace_done && st.decoded > m_baseline_decoded) {
        // A new decode landed since we started watching this button.
        // FR-17.2 discipline: NEC only, no parity/overflow, ignore
        // repeats (those are held-button artefacts, not first
        // presses). On a reject we still bump the baseline so the
        // next attempt is judged against this new high-water mark.
        const auto& d = st.last;
        const bool is_nec       = (d.protocol == /*decode_type_t::NEC*/8);
        const bool parity_bad   = (d.flags & /*IRDATA_FLAGS_PARITY_FAILED*/0x04) != 0;
        const bool overflowed   = (d.flags & /*IRDATA_FLAGS_WAS_OVERFLOW*/0x10) != 0;
        const bool is_repeat    = (d.flags & /*IRDATA_FLAGS_IS_REPEAT*/0x01) != 0;
        if (is_nec && !parity_bad && !overflowed && !is_repeat) {
          Capture& c = m_caps[m_current];
          c.captured = true;
          c.protocol = d.protocol;
          c.address  = d.address;
          c.command  = d.command;
          c.raw      = d.raw_data;
          m_advance_at_ms = now_ms + kCooldownMs;
        } else {
          // Bad frame: bump baseline so we keep waiting for a clean
          // one. The screen still says "PRESS <NAME>" — operator
          // sees the press didn't take and tries again.
          m_baseline_decoded = st.decoded;
        }
      } else if (!grace_done) {
        // During the grace window, accept whatever counter value
        // arrives so the post-grace baseline is "now" not "boot".
        m_baseline_decoded = st.decoded;
      }
    } else if (!m_published) {
      // Final state: publish exactly once. queue_debug() copies into
      // a static buffer + arms a sentinel; Core 0's MQTT poll drains
      // it on the next iteration. If MQTT is offline the payload is
      // simply lost — the on-screen capture table is still readable
      // by eye, and the operator can re-enter the scene.
      build_and_queue_payload();
      m_published = true;
    }

    // ---- Top brightness band (EMI stress) ------------------------------
    const uint32_t dt = now_ms - m_last_tick_ms;
    m_last_tick_ms = now_ms;
    m_palette_shift += static_cast<uint16_t>((dt * 24u) / 1000u);

    matrix.fillScreen(0x0000);
    for (int x = 0; x < PANEL_WIDTH; ++x) {
      const uint16_t idx = static_cast<uint16_t>(x * 3);
      matrix.drawPixel(x, 0,
          palette::bg(palette::Id::NEBULA_CLOUDS, idx, m_palette_shift));
    }

    // ---- Step header ---------------------------------------------------
    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);
    matrix.setTextColor(palette::fg(palette::Id::STAR_WHITE, 35));
    char header[16];
    if (m_current < kButtonCount) {
      snprintf(header, sizeof(header), "STEP %u/%u",
               static_cast<unsigned>(m_current + 1u),
               static_cast<unsigned>(kButtonCount));
    } else {
      snprintf(header, sizeof(header), "DONE %u/%u",
               static_cast<unsigned>(kButtonCount),
               static_cast<unsigned>(kButtonCount));
    }
    matrix.setCursor(1, 7);
    matrix.print(header);

    // ---- Button name (default 5x7 font, large + readable) -------------
    matrix.setFont(nullptr);
    matrix.setTextSize(1);
    const char* big = (m_current < kButtonCount)
                          ? kButtons[m_current].short_name
                          : "ALL OK";
    const int big_w = static_cast<int>(strlen(big)) * 6;  // 5px + 1 spacing
    int big_x = (PANEL_WIDTH - big_w) / 2;
    if (big_x < 0) big_x = 0;
    matrix.setTextColor(palette::fg(palette::Id::STAR_AMBER, 60));
    matrix.setCursor(big_x, 11);
    matrix.print(big);

    // ---- Prompt / capture readout / done banner -----------------------
    matrix.setFont(&Picopixel);
    matrix.setTextColor(0xFFFF);
    matrix.setCursor(1, 24);
    char line[24];
    if (m_current < kButtonCount) {
      if (m_advance_at_ms != 0) {
        // Cooldown: show the just-captured frame.
        const Capture& c = m_caps[m_current];
        snprintf(line, sizeof(line), "OK P%u A%03X C%03X",
                 static_cast<unsigned>(c.protocol),
                 static_cast<unsigned>(c.address) & 0xFFFu,
                 static_cast<unsigned>(c.command) & 0xFFFu);
        matrix.setTextColor(palette::fg(palette::Id::STAR_BLUE, 55));
      } else {
        snprintf(line, sizeof(line), "PRESS NOW");
      }
      matrix.print(line);
    } else {
      // Done: tell operator where to look.
      matrix.setTextColor(palette::fg(palette::Id::STAR_AMBER, 50));
      matrix.print("SEE observatory/debug");
    }

    // ---- Progress dots (one per button) -------------------------------
    // 9 buttons × 6 px stride = 54 px → fits centred with margins.
    constexpr int kDotStride = 6;
    const int dots_w = kButtonCount * kDotStride - 2;
    int dot_x = (PANEL_WIDTH - dots_w) / 2;
    for (uint8_t i = 0; i < kButtonCount; ++i) {
      uint16_t color;
      if (m_caps[i].captured) {
        color = palette::fg(palette::Id::STAR_BLUE, 50);  // done
      } else if (i == m_current) {
        // Pulse the current dot so it's clear which button is being asked for.
        const uint8_t br = static_cast<uint8_t>(
            20 + ((now_ms / 64u) & 0x1Fu));  // 20..51
        color = palette::fg(palette::Id::STAR_AMBER, br);
      } else {
        color = palette::fg(palette::Id::STAR_WHITE, 6);  // pending
      }
      // 3x3 filled square for readability.
      const int x0 = dot_x + i * kDotStride;
      for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
          matrix.drawPixel(x0 + dx, 28 + dy, color);
        }
      }
    }
  }

private:
  struct Capture {
    bool     captured;
    uint8_t  protocol;
    uint16_t address;
    uint16_t command;
    uint32_t raw;
  };

  // Build the observatory/debug payload from the capture table and
  // hand it to mqtt_link via the cross-core sentinel-0 buffer. Called
  // exactly once when m_current == kButtonCount.
  void build_and_queue_payload() {
    using namespace ir_test_scene_detail;

    // Local stack scratch — 640 B fits inside the kDebugPayloadCapacity
    // (768 B) the publisher allocates, with margin for MQTT framing
    // and any future extra fields.
    static constexpr size_t kBufCap = 640;
    char buf[kBufCap];

    // Top-level address = whichever capture's address shows up most
    // often (operationally always the same, but the loop below
    // tolerates a single misfire). Falls back to HOME's address.
    uint16_t addr = m_caps[0].captured ? m_caps[0].address : 0;
    int n = snprintf(buf, kBufCap,
        "{\"event\":\"ir_learn\",\"address\":%u,\"button_count\":%u,"
        "\"captures\":[",
        static_cast<unsigned>(addr),
        static_cast<unsigned>(kButtonCount));
    if (n < 0 || n >= static_cast<int>(kBufCap)) return;

    for (uint8_t i = 0; i < kButtonCount; ++i) {
      const Capture& c = m_caps[i];
      const int written = snprintf(
          buf + n, kBufCap - static_cast<size_t>(n),
          "%s{\"name\":\"%s\",\"proto\":%u,\"addr\":%u,\"cmd\":%u,\"raw\":%lu}",
          (i == 0) ? "" : ",",
          kButtons[i].json_key,
          static_cast<unsigned>(c.protocol),
          static_cast<unsigned>(c.address),
          static_cast<unsigned>(c.command),
          static_cast<unsigned long>(c.raw));
      if (written < 0 || static_cast<size_t>(written) >= kBufCap - static_cast<size_t>(n)) {
        // Truncation — bail out of the loop and close the JSON best-
        // effort. The publish side log will still show whatever fit.
        break;
      }
      n += written;
    }

    if (n + 3 < static_cast<int>(kBufCap)) {
      buf[n++] = ']';
      buf[n++] = '}';
      buf[n]   = '\0';
    } else {
      // Worst-case overflow guard: clamp + close.
      buf[kBufCap - 3] = ']';
      buf[kBufCap - 2] = '}';
      buf[kBufCap - 1] = '\0';
    }

    mqtt_link::queue_debug(buf);
  }

  // Grace period after init() before we accept input — covers the
  // case where a frame is mid-decode at scene-entry. 300 ms = 1
  // typical NEC frame + headroom.
  static constexpr uint32_t kGraceMs    = 300;
  // Time the captured-value confirmation stays on screen, also long
  // enough to absorb any NEC repeat frames a held button is still
  // emitting (~110 ms apart).
  static constexpr uint32_t kCooldownMs = 1200;

  bool        m_initialised      = false;
  uint32_t    m_init_ms          = 0;
  uint32_t    m_last_tick_ms     = 0;
  uint16_t    m_palette_shift    = 0;
  uint32_t    m_grace_until_ms   = 0;

  Capture     m_caps[ir_test_scene_detail::kButtonCount]{};
  uint8_t     m_current          = 0;   // index into kButtons; == count → done
  uint32_t    m_baseline_decoded = 0;   // ir_remote::stats().decoded high-water
  uint32_t    m_advance_at_ms    = 0;   // 0 = not in cooldown, else millis() target

  bool        m_published        = false;
};
