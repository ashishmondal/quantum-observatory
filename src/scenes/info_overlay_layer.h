// InfoOverlayLayer — IR.4 diagnostic info overlay (FR-17.8, FR-16.1).
//
// Triggered by the IR remote `OK` button (FR-17.5 local-fast). When
// armed, renders a 5-line text panel of operator-facing diagnostics
// over whatever scene is active for ~5 seconds:
//
//   line 1: IP address           (e.g. "192.168.1.42")
//   line 2: RSSI + MQTT state    (e.g. "RSS-55 MQ OK")
//   line 3: uptime + FPS         (e.g. "UP 12:34 F24")
//   line 4: scene id             (e.g. "SCN clock")
//   line 5: theme + free heap    (e.g. "THM apo H180k")
//
// Highest debug-payoff feature in Phase IR — when something breaks at
// the in-laws' place, the operator presses one button and gets every
// piece of state needed to diagnose it from across the room.
//
// Cross-core IPC: a single naturally-aligned `volatile uint32_t`
// (`g_info_overlay_event_ms`) carries the press timestamp from Core 0
// (the IR dispatch action) to Core 1 (this layer). Single-uint32
// atomicity is sufficient on RP2040 — no mutex / seqlock needed
// (CODING_PRACTICES §3, same pattern as `g_render_fps`,
// `g_render_alive_ms`, `g_first_frame_render_ms`). The layer
// remembers the last value it observed and treats any change as a
// toggle event.
//
// Toggle semantics (FR-17.8 "second OK press while visible
// dismisses immediately"):
//   - press in OFF / FADE_OUT → start FADE_IN (re-arm if mid-out)
//   - press in FADE_IN / HOLD → start FADE_OUT (early dismiss)
//
// Visual envelope (5 s total):
//   FADE_IN  300 ms  alpha 0   → 255   bayer-black overlay 255 → 0
//   HOLD     4100 ms alpha 255         no overlay (full opacity)
//   FADE_OUT 600 ms  alpha 255 → 0     bayer-black overlay 0   → 255
//
// Same Bayer dither as fade_black_layer + safety_overlay_layer — no
// framebuffer readback (Adafruit_Protomatter exposes none) and the
// stippling matches the device's retro pixel-art aesthetic better
// than a true alpha blend would.
//
// Slot ordering: lives in LAYER_OVERLAY_INFO (between SAFETY and
// TRANSITION). Above SAFETY so the operator's explicit OK press
// is honoured even when NIGHT / OFFLINE / THERMAL_SAFE has dimmed
// the underlying scene — the whole point is to expose the device's
// state, including which override is engaged. Below TRANSITION so
// a scene swap fade still composites cleanly on top.
//
// (added in phase IR.4)

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "scene_state.h"
#include "scenes/bayer_dither.h"
#include "scenes/layer.h"
#include "theme.h"

// Defined in main.cpp. Core 0 (IR dispatch action) writes the press
// timestamp; Core 1 (this layer) reads. Each new value is treated as
// a toggle event. Sentinel 0 = "no event ever fired" (matches the
// natural value at boot, so the layer stays OFF until the first OK).
extern volatile uint32_t g_info_overlay_event_ms;

// Defined in main.cpp — same per-frame value mqtt_link surfaces in the
// §5.4 status heartbeat. Read here so the operator's diagnostic view
// cannot disagree with HA's view of the device.
extern volatile uint32_t g_render_fps;

// Defined in main.cpp and republished once per second by Core 0 from
// the 1 Hz log tick. The WiFi.* / rp2040.getFreeHeap() entry points
// are not safe to call from Core 1 (radio SPI contention; malloc
// subsystem cross-core), so the layer reads the published snapshot
// instead. All four are naturally-aligned 32-bit volatiles — atomic
// on RP2040 (CODING_PRACTICES §3, same pattern as g_render_fps).
extern volatile uint32_t g_info_ip;          // IPv4 packed: byte[i] in (i*8)
extern volatile int32_t  g_info_rssi_dbm;
extern volatile uint32_t g_info_link_flags;  // bit 0 wifi, bit 1 mqtt
extern volatile uint32_t g_info_free_heap_b;

class InfoOverlayLayer final : public Layer {
public:
  const char* name() const override { return "info_overlay"; }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // ── Edge-detect a press event ──────────────────────────────────
    // Single 32-bit volatile read is atomic on RP2040 (CODING_PRACTICES
    // §3 — same pattern as g_render_fps). Any change vs. last_seen is
    // a fresh OK press; the press timestamp is also the trigger time
    // (we use now_ms for envelope start, which is close enough — the
    // press-to-render lag is a single frame at most, ~42 ms).
    const uint32_t evt = g_info_overlay_event_ms;
    if (evt != m_last_event_seen) {
      m_last_event_seen = evt;
      // Toggle — visible (FADE_IN/HOLD) dismisses; hidden re-arms.
      if (m_state == State::FADE_IN || m_state == State::HOLD) {
        m_state            = State::FADE_OUT;
        m_phase_started_ms = now_ms;
      } else {
        m_state            = State::FADE_IN;
        m_phase_started_ms = now_ms;
      }
    }

    if (m_state == State::OFF) return;

    // ── Advance the envelope ───────────────────────────────────────
    const uint32_t elapsed = now_ms - m_phase_started_ms;
    uint16_t       alpha   = 0;  // visibility 0..255 (255 = fully on)
    switch (m_state) {
      case State::FADE_IN:
        if (elapsed >= kFadeInMs) {
          m_state            = State::HOLD;
          m_phase_started_ms = now_ms;
          alpha              = 255;
        } else {
          alpha = static_cast<uint16_t>((elapsed * 255u) / kFadeInMs);
        }
        break;
      case State::HOLD:
        if (elapsed >= kHoldMs) {
          m_state            = State::FADE_OUT;
          m_phase_started_ms = now_ms;
        }
        alpha = 255;
        break;
      case State::FADE_OUT:
        if (elapsed >= kFadeOutMs) {
          m_state = State::OFF;
          return;
        }
        alpha = 255u - static_cast<uint16_t>((elapsed * 255u) / kFadeOutMs);
        break;
      case State::OFF:
        return;
    }

    // ── Wipe the panel + draw the diagnostics text ─────────────────
    // Full wipe (not just bg colour) so the underlying scene's pixels
    // don't bleed through behind glyphs. Picopixel + theme inks; the
    // halo on each line keeps the readout legible against any future
    // partial-overlay variant. Five rows × 6 px line height = 30 px,
    // fits the 32-row panel with a 1 px top margin.
    matrix.fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, 0x0000);
    draw_content(matrix, now_ms);

    // ── Apply the fade as a destructive bayer overlay ──────────────
    // bayer::apply_black_overlay stamps 0x0000 wherever
    // kBayer8[y%8][x%8] < threshold. We want fade-IN to start fully
    // black (visibility 0) and end fully clear (visibility 255), so
    // the overlay threshold is the COMPLEMENT of visibility.
    bayer::apply_black_overlay(matrix, static_cast<uint16_t>(255u - alpha));
  }

private:
  enum class State : uint8_t { OFF, FADE_IN, HOLD, FADE_OUT };

  // 5 s total visible window per FR-17.8 / IR.4. Fade-in is short
  // because the operator is staring at the panel waiting for it;
  // fade-out is longer so the readout doesn't snap away while the
  // last digit is still being read.
  static constexpr uint32_t kFadeInMs  = 300;
  static constexpr uint32_t kHoldMs    = 4100;  // 5000 - 300 - 600
  static constexpr uint32_t kFadeOutMs = 600;

  State    m_state            = State::OFF;
  uint32_t m_phase_started_ms = 0;
  uint32_t m_last_event_seen  = 0;

  // Picopixel baselines for each line. Glyphs span ~5 px above the
  // baseline; rows 0..4 hold the y=5 line, etc. Halo extends ±1 row
  // so consecutive baselines are spaced 6 px apart to avoid cross-
  // line halo collisions.
  static constexpr int16_t kY1 = 5;
  static constexpr int16_t kY2 = 11;
  static constexpr int16_t kY3 = 17;
  static constexpr int16_t kY4 = 23;
  static constexpr int16_t kY5 = 29;

  void draw_content(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    // Cross-theme baseline alignment is the font's job
    // (fonts/tomthumb_shifted.h, THEME.md §2.3) — no per-call y bias
    // here.
    const int16_t y1 = kY1;
    const int16_t y2 = kY2;
    const int16_t y3 = kY3;
    const int16_t y4 = kY4;
    const int16_t y5 = kY5;

    // BODY is the active theme's data-line ink — same role the
    // typewriter scenes use for their value rows. Halo is plain
    // black so neighbouring lines don't bleed at the bottom of
    // each Picopixel glyph.
    const uint16_t fg   = theme::ink(theme::Ink::BODY);
    const uint16_t halo = theme::ink(theme::Ink::BODY_HALO);

    char buf[24];

    // Line 1 — IP. The packed snapshot is 0.0.0.0 before the link
    // comes up (Core 0 hasn't published a non-zero value yet);
    // print the literal then so the operator can tell "no IP yet"
    // from "no Wi-Fi configured".
    {
      const uint32_t ip = g_info_ip;
      snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
               static_cast<unsigned>((ip      ) & 0xFFu),
               static_cast<unsigned>((ip >>  8) & 0xFFu),
               static_cast<unsigned>((ip >> 16) & 0xFFu),
               static_cast<unsigned>((ip >> 24) & 0xFFu));
      gfx::draw_text_halo(matrix, 0, y1, buf, fg, halo);
    }

    // Line 2 — RSSI (dBm) + MQTT state. RSSI reads 0 before the
    // first publish (matches "not connected"). MQ tag is 2 chars
    // (OK / NO) so the whole line stays under ~14 chars wide at
    // Picopixel's ~3 px advance.
    {
      const uint32_t flags = g_info_link_flags;
      const long     rssi  = static_cast<long>(g_info_rssi_dbm);
      const char*    mq    = (flags & 0x2u) ? "OK" : "NO";
      snprintf(buf, sizeof(buf), "RSS%ld MQ %s", rssi, mq);
      gfx::draw_text_halo(matrix, 0, y2, buf, fg, halo);
    }

    // Line 3 — uptime + FPS. Uptime as HHHH:MM (caps at 9999 h ≈
    // 416 d, well past any realistic single-boot lifetime). FPS is
    // sourced from g_render_fps via the same accessor mqtt_link uses
    // for the §5.4 heartbeat; keeping the value path identical means
    // the overlay cannot disagree with HA's view of the device.
    {
      const uint32_t up_s   = now_ms / 1000u;
      const uint32_t up_min = up_s / 60u;
      const uint32_t hh     = up_min / 60u;
      const uint32_t mm     = up_min % 60u;
      const uint32_t fps    = g_render_fps;
      snprintf(buf, sizeof(buf), "UP %lu:%02lu F%lu",
               static_cast<unsigned long>(hh),
               static_cast<unsigned long>(mm),
               static_cast<unsigned long>(fps));
      gfx::draw_text_halo(matrix, 0, y3, buf, fg, halo);
    }

    // Line 4 — active scene wire-id. Same string mqtt_link emits in
    // the §5.4 heartbeat.
    {
      const char* scn = scene_state::string_from_id(scene_state::current());
      snprintf(buf, sizeof(buf), "SCN %s", scn);
      gfx::draw_text_halo(matrix, 0, y4, buf, fg, halo);
    }

    // Line 5 — active theme + free heap (KB). Theme.h doesn't yet
    // expose an id-to-string helper (T.4 will add `theme` to the
    // status heartbeat — same need); keep the mapping local until
    // then so this layer doesn't block on T.4. Free heap is
    // rounded to KB so the value fits the row even at full SRAM.
    {
      const char* thm = theme_short_name(theme::current());
      const uint32_t heap_kb = g_info_free_heap_b / 1024u;
      snprintf(buf, sizeof(buf), "THM %s H%luk", thm,
               static_cast<unsigned long>(heap_kb));
      gfx::draw_text_halo(matrix, 0, y5, buf, fg, halo);
    }
  }

  // Local short-name table. Mirrors theme::Id; collapses each name
  // to ≤ 4 chars so the line-5 readout fits Picopixel's ~3 px advance
  // alongside the heap reading. Folded out when T.4 adds a global
  // id-to-string helper.
  static const char* theme_short_name(theme::Id id) {
    switch (id) {
      case theme::Id::APOLLO_AMBER:   return "APO";
      // T.5+ themes — keep ahead of theme::Id growth so a new theme
      // showing as "?" is the diagnostic, not a build break.
      default:                         return "?";
    }
  }
};
