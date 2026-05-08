// IR receiver POC scene.
//
// Diagnostic scene for phase IR.1 — turns the panel into a live readout
// of the on-board IR receiver (GP28) so we can sit in front of the
// device with a remote and characterise:
//
//   - Does the receiver decode our remote at all? (proto / addr / cmd)
//   - How well does it work against a bright HUB75 frame? (ghost-decode
//     rate while no button is being pressed)
//   - Does the AGC recover quickly between presses? (counters climb
//     monotonically; missed presses show as a mismatch between what
//     you press and what `decoded` increments by)
//
// Selectable over MQTT via {"scene_id": "ir_test"}. wants_clock_chrome
// is false so the chrome readout doesn't fight the diagnostic text.
//
// Layout (64x32):
//
//     row  0       brightness band: full-width nebula gradient cycling
//                  left→right. Lets us watch ghost-decode rate against
//                  a deliberately busy bright frame at the top of the
//                  panel. (HUB75 EMI is brightness-correlated.)
//     rows  2..6   "decoded=NNNN" big-ish (Picopixel ×1) running total
//                  + a small 16-cell flash strip on the right edge that
//                  blinks white on every new decode (visual "did it
//                  fire?" for noisy environments).
//     rows  8..12  "u=NN p=NN o=NN r=NN" — unknown / parity_failed /
//                  overflow / repeats counters. Watch these climb on
//                  their own with no remote pressed → that's EMI.
//     rows 14..18  "P=NN A=XXXX C=XXXX" — most recent decode's
//                  protocol (decode_type_t int), address, command.
//                  P=8 = NEC (Roku family), P=9 = SONY, P=2 = RC5,
//                  P=0 = UNKNOWN (likely noise).
//     rows 20..24  "BITS=NN F=XX age=NNs" — bit-count, flag bitmask,
//                  age of last decode in seconds. Age is useful for
//                  the "is this remote even in the same room?" check.
//     rows 26..31  health bar: green segment width = recent decode
//                  rate, red segment width = recent unknown rate,
//                  both relative to a 5 s rolling window. Eyeball
//                  metric for "signal vs. noise" without reading
//                  digits from across the room.
//
// All counter math reuses ir_remote::stats() — this scene is a pure
// reader; ir_remote::poll() is still driven from main.cpp loop()
// every iteration. The scene also calls ir_remote::reset_counters()
// in init() so each visit starts from zero (FR-12.6 spirit: a
// diagnostic scene's first frame should be a clean baseline).

#pragma once

#include <stdint.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>
#include <Fonts/Picopixel.h>

#include "color_palette.h"
#include "config.h"
#include "ir_remote.h"
#include "scenes/scene.h"

class IrTestScene : public Scene {
public:
  const char* name() const override { return "ir_test"; }
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    (void)matrix;
    m_init_ms          = 0;
    m_initialised      = false;
    m_last_decoded     = 0;
    m_flash_until_ms   = 0;
    m_palette_shift    = 0;
    m_last_tick_ms     = 0;
    // Snapshot the live counters so the on-screen "since you opened
    // this scene" view starts at zero without losing any global
    // history other consumers (none yet) might care about.
    ir_remote::reset_counters();
    // Five-second rolling window history (one slot per second).
    for (auto& s : m_history) {
      s.decoded_at_close = 0;
      s.unknown_at_close = 0;
    }
    m_history_head        = 0;
    m_window_open_at_ms   = 0;
    m_window_decoded_base = 0;
    m_window_unknown_base = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_initialised) {
      m_init_ms             = now_ms;
      m_last_tick_ms        = now_ms;
      m_window_open_at_ms   = now_ms;
      m_initialised         = true;
    }

    const ir_remote::Stats st = ir_remote::stats();

    // --- Edge-detect new decodes for the flash strip ---------------------
    // st.decoded is monotonic; on any climb, light the flash strip for
    // a short visible window (~120 ms — long enough to see at 60 fps,
    // short enough that a held-down repeat looks like a fast strobe).
    if (st.decoded != m_last_decoded) {
      m_last_decoded   = st.decoded;
      m_flash_until_ms = now_ms + 120u;
    }

    // --- Palette-cycle phase advance for the top brightness band --------
    const uint32_t dt = now_ms - m_last_tick_ms;
    m_last_tick_ms = now_ms;
    m_palette_shift += static_cast<uint16_t>((dt * 24u) / 1000u);

    // --- 1 s window bookkeeping for the health bar ----------------------
    while (now_ms - m_window_open_at_ms >= 1000u) {
      m_history[m_history_head].decoded_at_close =
          st.decoded - m_window_decoded_base;
      m_history[m_history_head].unknown_at_close =
          st.unknown - m_window_unknown_base;
      m_history_head = static_cast<uint8_t>((m_history_head + 1u) % kHistoryLen);
      m_window_open_at_ms   += 1000u;
      m_window_decoded_base  = st.decoded;
      m_window_unknown_base  = st.unknown;
    }

    // --- Draw -----------------------------------------------------------
    matrix.fillScreen(0x0000);

    // Top brightness band — same EMI-stress trick as gfx_test, scoped
    // to row 0 so it doesn't crowd the digits below.
    for (int x = 0; x < PANEL_WIDTH; ++x) {
      const uint16_t idx = static_cast<uint16_t>(x * 3);
      matrix.drawPixel(x, 0,
          palette::bg(palette::Id::NEBULA_CLOUDS, idx, m_palette_shift));
    }

    matrix.setFont(&Picopixel);
    matrix.setTextSize(1);

    char line[24];

    // Row of decode total + flash witness.
    matrix.setTextColor(0xFFFF);
    snprintf(line, sizeof(line), "dec %lu",
             static_cast<unsigned long>(st.decoded));
    matrix.setCursor(1, 6);
    matrix.print(line);
    if (now_ms < m_flash_until_ms) {
      // Flash strip on the right — bright cyan so it pops against any
      // residual band noise. Width scales with intensity (every
      // PARITY_FAILED also counts as a decode, so this fires for
      // those too — that's deliberate, the scene cares about
      // "receiver activity" generally).
      for (int x = PANEL_WIDTH - 8; x < PANEL_WIDTH; ++x) {
        matrix.drawPixel(x, 5, 0x07FF);
        matrix.drawPixel(x, 6, 0x07FF);
      }
    }

    // Counter row — colour-coded to nudge the eye:
    //   white  decoded summary (above)
    //   amber  unknown / parity / overflow (warnings)
    //   gray   repeats (informational, every held key spams these)
    matrix.setTextColor(palette::fg(palette::Id::STAR_AMBER, 50));
    snprintf(line, sizeof(line), "u%lu p%lu o%lu",
             static_cast<unsigned long>(st.unknown),
             static_cast<unsigned long>(st.parity_failed),
             static_cast<unsigned long>(st.overflows));
    matrix.setCursor(1, 12);
    matrix.print(line);

    matrix.setTextColor(palette::fg(palette::Id::STAR_WHITE, 30));  // dim
    snprintf(line, sizeof(line), "rep %lu",
             static_cast<unsigned long>(st.repeats));
    matrix.setCursor(36, 12);
    matrix.print(line);

    // Last-decode protocol / addr / cmd. Only meaningful once we've
    // had at least one decode this session.
    matrix.setTextColor(0xFFFF);
    if (st.decoded > 0u) {
      snprintf(line, sizeof(line), "P%u A%04X C%04X",
               static_cast<unsigned>(st.last.protocol),
               static_cast<unsigned>(st.last.address),
               static_cast<unsigned>(st.last.command));
      matrix.setCursor(1, 18);
      matrix.print(line);

      const uint32_t age_s = (now_ms - st.last.at_ms) / 1000u;
      snprintf(line, sizeof(line), "b%u f%02X %lus",
               static_cast<unsigned>(st.last.num_bits),
               static_cast<unsigned>(st.last.flags),
               static_cast<unsigned long>(age_s));
      matrix.setCursor(1, 24);
      matrix.print(line);
    } else {
      matrix.setTextColor(palette::fg(palette::Id::STAR_WHITE, 20));
      matrix.setCursor(1, 18);
      matrix.print("press a key");
      matrix.setCursor(1, 24);
      matrix.print("on remote...");
    }

    // Health bar — sum the rolling 5 s window. Green = good signal,
    // red = noise. Caps at PANEL_WIDTH so a long-press repeat storm
    // doesn't run off the panel.
    uint32_t recent_decoded = 0;
    uint32_t recent_unknown = 0;
    for (const auto& s : m_history) {
      recent_decoded += s.decoded_at_close;
      recent_unknown += s.unknown_at_close;
    }
    const int green_w = clamp_to_panel(recent_decoded);
    const int red_w   = clamp_to_panel(recent_unknown);
    for (int y = 28; y < 31; ++y) {
      for (int x = 0; x < green_w; ++x)               matrix.drawPixel(x, y, 0x07E0);
      for (int x = 0; x < red_w; ++x)                 matrix.drawPixel(PANEL_WIDTH - 1 - x, y, 0xF800);
    }
    // Faint baseline so an empty bar is still visible.
    for (int x = 0; x < PANEL_WIDTH; x += 4) {
      matrix.drawPixel(x, 31, palette::fg(palette::Id::STAR_WHITE, 5));
    }
  }

private:
  static int clamp_to_panel(uint32_t v) {
    return v >= static_cast<uint32_t>(PANEL_WIDTH)
             ? PANEL_WIDTH
             : static_cast<int>(v);
  }

  static constexpr uint8_t kHistoryLen = 5;  // 5 × 1 s windows
  struct WindowSlot {
    uint32_t decoded_at_close;
    uint32_t unknown_at_close;
  };

  bool        m_initialised        = false;
  uint32_t    m_init_ms            = 0;
  uint32_t    m_last_tick_ms       = 0;
  uint16_t    m_palette_shift      = 0;

  uint32_t    m_last_decoded       = 0;
  uint32_t    m_flash_until_ms     = 0;

  WindowSlot  m_history[kHistoryLen]{};
  uint8_t     m_history_head       = 0;
  uint32_t    m_window_open_at_ms  = 0;
  uint32_t    m_window_decoded_base = 0;
  uint32_t    m_window_unknown_base = 0;
};
