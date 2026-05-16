// SettingsOverlayLayer — FR-19 settings menu (FR-16.1 layer).
//
// Visual half of the FR-19 settings overlay. Cross-core IPC into the
// settings_ui model is one mutex-guarded snapshot per frame on Core 1
// (CODING_PRACTICES §3 — multi-field state, mutex; the lock-free
// `settings_ui::is_open()` byte is for the IR carve-out fast path on
// Core 0, not for the renderer).
//
// Slot ordering (compositor.cpp):
//   LAYER_FG → LAYER_OVERLAY_SAFETY → LAYER_OVERLAY_SETTINGS →
//   LAYER_OVERLAY_INFO → LAYER_OVERLAY_TRANSITION → LAYER_CHROME
//
// Above SAFETY: the operator can adjust BG TINT or mute sounds
// while NIGHT / OFFLINE / THERMAL_SAFE are engaged — same rationale
// as InfoOverlayLayer (the menu is an explicit operator action and
// therefore wins over implicit firmware overrides).
//
// Below TRANSITION: a scene swap fade still composites cleanly
// over the menu so an ISS pass blacking the panel does not reveal
// a half-rendered overlay underneath.
//
// Above CHROME: the corner HH:MM readout is suppressed while the
// menu is up — we want the operator's full attention on the menu,
// and we need every pixel for the segmented tint bar / row labels.
// (Implemented by the matching wants_clock_chrome() override stamp
// in compositor.cpp's chrome adapter.)
//
// Animation envelopes (settings_ui.cpp drives the timestamps; this
// layer just consumes them):
//   FADE_IN  300 ms — bayer-black fade-out from 100 % → 0 %
//   FADE_OUT 300 ms — symmetric, then layer self-skips next frame
//   FLASH    80  ms — inverse-flash on commit (XOR per row)
//   TOAST    1500 ms— "SAVED" pill bottom-right, fired on prefs
//                     dirty→clean falling edge

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "gfx_text.h"
#include "settings_ui.h"
#include "scenes/bayer_dither.h"
#include "scenes/layer.h"
#include "theme.h"

class SettingsOverlayLayer final : public Layer {
public:
  const char* name() const override { return "settings_overlay"; }

  // FR-19 — suppress the corner clock chrome while the menu is up
  // so the segmented tint bar and SOUND rows have the full panel.
  // The chrome adapter consults this hint via the active scene OR
  // the topmost covering override; we plumb it through as the menu
  // state so it applies regardless of what the underlying scene is.
  bool covering_now() const { return m_visible_last_frame; }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    const settings_ui::Snapshot snap = settings_ui::snapshot();

    // Fast-path: nothing to draw, skip even the fade envelope work.
    const bool open_now = snap.mode != settings_ui::Mode::CLOSED;
    if (!open_now && snap.fade_dir != settings_ui::FadeDir::OUT) {
      m_visible_last_frame = false;
      return;
    }

    // ── Compute fade envelope alpha [0..255] (255 = fully visible) ─
    uint16_t alpha = 255;
    if (snap.fade_dir == settings_ui::FadeDir::IN) {
      const uint32_t e = now_ms - snap.fade_started_ms;
      alpha = e >= kFadeMs
                  ? 255u
                  : static_cast<uint16_t>((e * 255u) / kFadeMs);
    } else if (snap.fade_dir == settings_ui::FadeDir::OUT) {
      const uint32_t e = now_ms - snap.fade_started_ms;
      if (e >= kFadeMs) {
        // Fade complete -- nothing to draw this frame.
        m_visible_last_frame = false;
        return;
      }
      alpha = static_cast<uint16_t>(255u - (e * 255u) / kFadeMs);
    }

    // ── Opaque backdrop (FR-19.5) — paint a fully black 64×32 fill
    // so the menu reads as its own scene rather than a translucent
    // veil over whatever scene + safety override is underneath.
    // The DISPLAY → BG TINT row paints its own preview swatch
    // on top of this surface (per FR-19.3); no hole is punched in
    // the backdrop.
    matrix.fillRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, 0x0000);

    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    const uint16_t ink   = theme::ink(theme::Ink::CHROME);
    const uint16_t halo  = theme::ink(theme::Ink::CHROME_HALO);
    const uint16_t accent= theme::ink(theme::Ink::ACCENT);

    // ── Title row (no typewriter; render the full string immediately).
    const char* title = (snap.mode == settings_ui::Mode::ROOT)
                          ? "SETTINGS"
                          : (snap.cat == settings_ui::Cat::DISPLAY
                              ? "DISPLAY"
                              : "SOUND");
    gfx::draw_text_halo(matrix, /*x=*/2, /*y=*/5, title, accent, halo);

    if (snap.mode == settings_ui::Mode::ROOT) {
      draw_root(matrix, snap, now_ms, ink, halo, accent);
    } else {
      draw_category(matrix, snap, now_ms, ink, halo, accent);
    }

    // ── Idle countdown (last 5 s of the 30 s window) bottom-right.
    // Reads as a small "5...4...3..." hint so the operator knows the
    // menu is about to dismiss itself.
    const uint32_t idle_ms = (now_ms >= snap.last_input_ms)
                                 ? (now_ms - snap.last_input_ms)
                                 : 0u;
    if (open_now && idle_ms >= 25'000 && idle_ms < 30'000) {
      const uint8_t s_left = static_cast<uint8_t>(
          (30'000 - idle_ms) / 1000 + 1);
      char cd[3];
      snprintf(cd, sizeof(cd), "%u", static_cast<unsigned>(s_left));
      gfx::draw_text_halo(matrix, /*x=*/PANEL_WIDTH - 5, /*y=*/PANEL_HEIGHT - 2,
                          cd, accent, halo);
    }

    // ── Apply the entry/exit fade as a destructive bayer overlay.
    if (alpha < 255) {
      bayer::apply_black_overlay(matrix, static_cast<uint16_t>(255u - alpha));
    }

    m_visible_last_frame = true;
  }

private:
  static constexpr uint32_t kFadeMs    = 300;
  static constexpr uint32_t kFlashMs   = 80;
  static constexpr uint32_t kToastMs   = 1500;

  bool m_visible_last_frame = false;

  static bool flash_active(const settings_ui::Snapshot& snap, uint32_t now_ms) {
    if (snap.flash_started_ms == 0) return false;
    return (now_ms - snap.flash_started_ms) < kFlashMs;
  }

  // ROOT view: two stacked rows -- DISPLAY / SOUND. Selected row has
  // accent ink + boxed border; unselected is dim ink only.
  void draw_root(Adafruit_Protomatter& matrix,
                 const settings_ui::Snapshot& snap,
                 uint32_t now_ms,
                 uint16_t ink, uint16_t halo, uint16_t accent) {
    (void)now_ms;
    struct Row { const char* label; int16_t y_baseline; int16_t y_box; };
    const Row rows[2] = {
        {"DISPLAY", 18, 12},
        {"SOUND",   27, 21},
    };
    for (uint8_t i = 0; i < 2; ++i) {
      const bool sel = (i == snap.sel_index);
      const uint16_t fg = sel ? accent : ink;
      if (sel) {
        matrix.drawRect(1, rows[i].y_box, PANEL_WIDTH - 2, 9, accent);
      }
      gfx::draw_text_halo(matrix, /*x=*/3, rows[i].y_baseline,
                          rows[i].label, fg, halo);
    }
  }

  // CATEGORY view: dispatch to per-category drawing.
  void draw_category(Adafruit_Protomatter& matrix,
                     const settings_ui::Snapshot& snap,
                     uint32_t now_ms,
                     uint16_t ink, uint16_t halo, uint16_t accent) {
    const bool flash = flash_active(snap, now_ms);
    if (snap.cat == settings_ui::Cat::DISPLAY) {
      draw_display(matrix, snap, ink, halo, accent, flash);
    } else {
      draw_sound(matrix, snap, ink, halo, accent, flash);
    }
  }

  void draw_display(Adafruit_Protomatter& matrix,
                    const settings_ui::Snapshot& snap,
                    uint16_t ink, uint16_t halo, uint16_t accent,
                    bool flash) {
    // Row label.
    gfx::draw_text_halo(matrix, /*x=*/2, /*y=*/13, "BG TINT",
                        ink, halo);
    // 10-cell segmented bar: cells 0..9 representing 10..100 %.
    // Cell width = 5 px, gap = 1 px -> 6*10 = 60 px wide; centered.
    constexpr int16_t kCellW = 5;
    constexpr int16_t kCellH = 6;
    constexpr int16_t kGap   = 1;
    constexpr int16_t kBarY  = 17;
    const int16_t bar_x = (PANEL_WIDTH - (10 * (kCellW + kGap) - kGap)) / 2;
    const uint8_t filled = snap.tint_pct / 10;  // 0..10
    for (uint8_t i = 0; i < 10; ++i) {
      const int16_t x = bar_x + i * (kCellW + kGap);
      const bool on = i < filled;
      const uint16_t cell = on ? accent : ink;
      if (on || flash) {
        matrix.fillRect(x, kBarY, kCellW, kCellH, cell);
      } else {
        matrix.drawRect(x, kBarY, kCellW, kCellH, cell);
      }
    }
    // Numeric readout to the right.
    char val[5];
    snprintf(val, sizeof(val), "%u%%", static_cast<unsigned>(snap.tint_pct));
    gfx::draw_text_halo(matrix, /*x=*/2, /*y=*/29, val, accent, halo);
  }

  void draw_sound(Adafruit_Protomatter& matrix,
                  const settings_ui::Snapshot& snap,
                  uint16_t ink, uint16_t halo, uint16_t accent,
                  bool flash) {
    struct Row { const char* label; const char* val; };
    char tick_val[5];
    switch (snap.tick_sound_mode) {
      case 0: snprintf(tick_val, sizeof(tick_val), "OFF"); break;
      case 1: snprintf(tick_val, sizeof(tick_val), "MIN"); break;
      case 2: snprintf(tick_val, sizeof(tick_val), "10M"); break;
      case 3: snprintf(tick_val, sizeof(tick_val), "HR");  break;
      default: snprintf(tick_val, sizeof(tick_val), "?");  break;
    }
    const Row rows[3] = {
        {"THEME ",  snap.theme_sound  ? "ON"  : "OFF"},
        {"BUTTON",  snap.button_sound ? "ON"  : "OFF"},
        {"TICK  ",  tick_val},
    };
    constexpr int16_t kRowH = 6;
    constexpr int16_t kY0   = 13;
    for (uint8_t i = 0; i < 3; ++i) {
      const int16_t y   = kY0 + i * kRowH;
      const bool    sel = (i == snap.sel_index);
      const uint16_t fg = sel ? accent : ink;
      // Selection arrow.
      if (sel) {
        gfx::draw_text_halo(matrix, /*x=*/0, y, ">", accent, halo);
      }
      gfx::draw_text_halo(matrix, /*x=*/5, y, rows[i].label, fg, halo);
      // Value text right-aligned, 5 px from the right edge of the panel;
      // inverse-flash on commit paints a filled pill behind the value.
      int16_t  vbx, vby;
      uint16_t vbw, vbh;
      matrix.getTextBounds(rows[i].val, 0, 0, &vbx, &vby, &vbw, &vbh);
      const int16_t vx = static_cast<int16_t>(
          PANEL_WIDTH - 5 - static_cast<int16_t>(vbw) - vbx);
      if (sel && flash) {
        // Inverse flash: filled pill with ink-color text.
        const int16_t pill_x = vx + vbx - 1;
        const int16_t pill_w = static_cast<int16_t>(vbw) + 2;
        matrix.fillRect(pill_x, y - 5, pill_w, kRowH, accent);
        gfx::draw_text_halo(matrix, vx, y, rows[i].val, 0x0000, 0x0000);
      } else {
        gfx::draw_text_halo(matrix, vx, y, rows[i].val, fg, halo);
      }
    }
  }
};
