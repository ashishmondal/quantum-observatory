// spectrum_demo scene — visible-spectrum bar + sweeping absorption-line
// marker (phase 7.6, FUTURE_SCENES Tier 2 "astrophysics flavor").
//
// Educational scene that turns the panel into a tiny Astronomy-101
// classroom: a 64-pixel-wide 380→700 nm gradient bar sits mid-panel,
// and a vertical marker sweeps left→right, pausing on each of eight
// famous spectral lines (Ca-K / Hδ / Hγ / Hβ / Mg-b / Na-D / Hα /
// O₂-B) while the element name + wavelength type out below.
//
// Fully offline (FR-14 spirit — the dashboard stays alive when HA is
// down): no MQTT topic, no asset pipeline, just RTC-driven sweep
// math. The gradient itself is a compile-time `constexpr` PROGMEM
// table (~128 B flash, zero RAM) built from a piecewise-linear
// wavelength→sRGB approximation (Dan Bruton's classic; integer
// math, NFR-1.3). At 64 px width the piecewise version is visually
// indistinguishable from the full CIE conversion and avoids any
// per-frame floating point.
//
// CODING_PRACTICES §4 exemption: the gradient palette is intentionally
// scene-local (it IS the data being shown), not theme-owned. Header,
// tick marks, marker, and labels all route through `theme::ink()` so
// theme switches retone the chrome while the spectrum keeps its
// physically-meaningful colors.
//
// Layout (64×32):
//   y= 0..6   "[STRM]" header (theme HEADER font + brackets); top-right
//             FR-9.2 HH:MM chrome painted by the compositor sits in the
//             same band, separated from the left-anchored header.
//   y= 9..16  64-wide gradient strip (drawFastVLine per column from
//             kSpectrumBar[])
//   y=17      4× tick marks at 400/500/600/700 nm (LABEL ink) +
//             sweeping vertical marker (VALUE ink, 2 px above + below)
//   y=19..24  Element name (BODY font, centred)
//   y=25..30  Wavelength "NNN NM" (BODY font, centred)
//
// Sweep state machine: SWEEPING (1.5 s linear interp between the
// previous and next line) → SETTLED (5 s pause on the new line) →
// advance idx → SWEEPING. All `uint32_t` millis-delta arithmetic,
// wrap-safe (CODING_PRACTICES §2).
//
// Opts in to the FR-9.2 top-right HH:MM clock chrome (default) — the
// [STRM] header is left-anchored at x=2 so the two share the y=0..6
// band without overlapping.

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "gfx_text.h"
#include "scene.h"
#include "theme.h"

namespace spectrum_demo_detail {

// Piecewise-linear wavelength→sRGB approximation (Dan Bruton), all
// integer math so the table can be `constexpr`. Output channels are
// 0..255 pre-packing. Intensity falloff at the violet edge (380→420)
// mimics scotopic dropoff; the red edge stays at full intensity
// since the bar stops at 700 nm.
constexpr uint16_t nm_to_rgb565(int nm) {
  int r = 0, g = 0, b = 0;
  if (nm < 440) {
    // 380..440: violet — R fades in from 0 backwards, B = full.
    r = ((440 - nm) * 255) / 60;
    b = 255;
  } else if (nm < 490) {
    // 440..490: blue → cyan.
    g = ((nm - 440) * 255) / 50;
    b = 255;
  } else if (nm < 510) {
    // 490..510: cyan → green.
    g = 255;
    b = ((510 - nm) * 255) / 20;
  } else if (nm < 580) {
    // 510..580: green → yellow.
    r = ((nm - 510) * 255) / 70;
    g = 255;
  } else if (nm < 645) {
    // 580..645: yellow → red.
    r = 255;
    g = ((645 - nm) * 255) / 65;
  } else {
    // 645..700: pure red.
    r = 255;
  }

  // Violet-edge intensity falloff (rises 0.3 → 1.0 over 380..420).
  // Outside that window the factor stays at 255 (full intensity).
  int factor = 255;
  if (nm < 420) {
    // 76 ≈ 0.30 × 255; 179 ≈ 0.70 × 255.
    factor = 76 + ((nm - 380) * 179) / 40;
    if (factor < 0)   factor = 0;
    if (factor > 255) factor = 255;
  }
  r = (r * factor) / 255;
  g = (g * factor) / 255;
  b = (b * factor) / 255;

  // Pack into RGB565 (5/6/5).
  return static_cast<uint16_t>(((r & 0xF8) << 8) |
                               ((g & 0xFC) << 3) |
                               (b >> 3));
}

// Map a wavelength to a column index on the 64-wide bar covering
// 380..700 nm (320 nm span). Clamped to [0, PANEL_WIDTH-1].
constexpr uint8_t nm_to_x(int nm) {
  int x = (nm - 380) * (PANEL_WIDTH - 1) / 320;
  if (x < 0) x = 0;
  if (x > PANEL_WIDTH - 1) x = PANEL_WIDTH - 1;
  return static_cast<uint8_t>(x);
}

// Compile-time gradient table. Wrapped in a struct so the initializer
// loop can run in a `constexpr` ctor (C++14+) — gives us a single
// `.rodata` table without per-call computation. ~128 B flash, 0 RAM.
struct SpectrumBar {
  uint16_t v[PANEL_WIDTH];
  constexpr SpectrumBar() : v{} {
    for (int i = 0; i < PANEL_WIDTH; ++i) {
      // Map column i ∈ [0, 63] back to nm ∈ [380, 700].
      const int nm = 380 + (i * 320) / (PANEL_WIDTH - 1);
      v[i] = nm_to_rgb565(nm);
    }
  }
};
inline constexpr SpectrumBar kSpectrumBar{};
static_assert(sizeof(kSpectrumBar) == PANEL_WIDTH * 2,
              "spectrum bar table must be 128 bytes (64 × uint16_t)");

// Famous absorption / emission lines visible in the solar spectrum.
// Names kept ≤10 chars to fit the BODY font comfortably (FR-4.4
// 14-char ceiling). nm precomputed-to-x at build time.
struct Line {
  const char* name;
  uint16_t    nm;
};
inline constexpr Line kLines[] = {
  { "CA K",    393 },
  { "H DELTA", 410 },
  { "H GAMMA", 434 },
  { "H BETA",  486 },
  { "MG B",    518 },
  { "NA D",    589 },
  { "H ALPHA", 656 },
  { "O2 B",    687 },
};
inline constexpr uint8_t kLineCount =
    sizeof(kLines) / sizeof(kLines[0]);

// Tick-mark wavelengths drawn under the bar (every 100 nm in the
// visible range). Kept short so the per-frame draw is 4 vlines.
inline constexpr uint16_t kTickNm[] = { 400, 500, 600, 700 };
inline constexpr uint8_t  kTickCount =
    sizeof(kTickNm) / sizeof(kTickNm[0]);

// Animation timing (CODING_PRACTICES §2 — millis deltas only).
constexpr uint32_t kSweepMs  = 1500;  // glide between lines
constexpr uint32_t kSettleMs = 5000;  // pause on a line, type out label

// Y bands.
constexpr int16_t kBarY      = 9;     // top of gradient strip (1 px below header band)
constexpr int16_t kBarH      = 8;     // strip height
constexpr int16_t kTickY     = kBarY + kBarH;          // row immediately below the bar
constexpr int16_t kMarkerTop = kBarY - 2;              // top tick of the sweep marker
constexpr int16_t kMarkerBot = kBarY + kBarH + 1;      // bottom tick of the sweep marker
constexpr int16_t kNameBaseY = 24;    // BODY baseline for element name
constexpr int16_t kWaveBaseY = 30;    // BODY baseline for wavelength

}  // namespace spectrum_demo_detail

class SpectrumDemoScene : public Scene {
public:
  const char* name() const override { return "spectrum_demo"; }

  // The standard top-right Picopixel HH:MM chrome sits at y=0..6 on
  // the right edge — the [STRM] header is left-anchored so the two
  // don't overlap. Keep default `wants_clock_chrome() == true` so the
  // scene gets the FR-9.2 corner readout for free.

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_idx              = 0;
    m_phase            = Phase::SWEEPING;
    m_segment_start_ms = 0;
    // Sweep begins by gliding from the last line in the table back to
    // the first one, so the marker has a visible motion at startup
    // rather than snapping into place mid-panel.
    using namespace spectrum_demo_detail;
    m_from_x = nm_to_x(kLines[kLineCount - 1].nm);
    m_to_x   = nm_to_x(kLines[0].nm);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using namespace spectrum_demo_detail;

    // Lazy timer seed: init() runs before the first millis() tick is
    // forwarded to scenes (Splash sequencing), so anchor on the first
    // real render() rather than risk a zero-duration first segment.
    if (m_segment_start_ms == 0) m_segment_start_ms = now_ms;

    matrix.fillScreen(0x0000);

    // ── 1. Gradient strip ──────────────────────────────────────────
    // 64 drawFastVLine calls — well under the per-frame budget
    // (bayer_dither.h notes 2048 drawPixel calls fit in < 0.5 ms).
    for (int16_t x = 0; x < PANEL_WIDTH; ++x) {
      matrix.drawFastVLine(x, kBarY, kBarH, kSpectrumBar.v[x]);
    }

    // ── 2. Tick marks at 400/500/600/700 nm ────────────────────────
    const uint16_t label_ink = theme::ink(theme::Ink::LABEL);
    for (uint8_t i = 0; i < kTickCount; ++i) {
      const uint8_t tx = nm_to_x(kTickNm[i]);
      matrix.drawPixel(tx, kTickY, label_ink);
    }

    // ── 3. Header (bracketed identity, theme-owned routing) ────────
    // Routes through gfx::draw_scene_header so brackets, halo, and
    // BLOCK_BARS hint (LCARS) all stay theme-driven (T.7a).
    gfx::draw_scene_header(matrix, "STRM",
                           /*x_unused=*/0, /*y=*/0,
                           theme::ink(theme::Ink::HEADER));

    // ── 4. Sweep state machine ─────────────────────────────────────
    const uint32_t elapsed = now_ms - m_segment_start_ms;
    const uint32_t deadline =
        (m_phase == Phase::SWEEPING) ? kSweepMs : kSettleMs;

    if (elapsed >= deadline) {
      if (m_phase == Phase::SWEEPING) {
        m_phase = Phase::SETTLED;
      } else {
        // Advance to the next line, swap from/to anchors.
        m_idx = static_cast<uint8_t>((m_idx + 1) % kLineCount);
        m_from_x = m_to_x;
        m_to_x   = nm_to_x(kLines[m_idx].nm);
        m_phase  = Phase::SWEEPING;
      }
      m_segment_start_ms = now_ms;
    }

    // Marker x: linear interp during SWEEPING, fixed during SETTLED.
    // t in [0, 256] avoids a divide-by-256 in the hot path.
    int16_t marker_x;
    if (m_phase == Phase::SWEEPING) {
      uint32_t t = (elapsed * 256u) / kSweepMs;
      if (t > 256u) t = 256u;
      const int16_t span = static_cast<int16_t>(m_to_x) -
                           static_cast<int16_t>(m_from_x);
      marker_x = static_cast<int16_t>(m_from_x) +
                 static_cast<int16_t>((span * static_cast<int16_t>(t)) >> 8);
    } else {
      marker_x = static_cast<int16_t>(m_to_x);
    }
    if (marker_x < 0)              marker_x = 0;
    if (marker_x > PANEL_WIDTH - 1) marker_x = PANEL_WIDTH - 1;

    // ── 5. Marker glyph: two 2-pixel tick stems above + below the
    //      bar, pointing at the current wavelength. Theme VALUE ink
    //      so it stays legible against the gradient (high V/luma).
    const uint16_t marker_ink = theme::ink(theme::Ink::VALUE);
    matrix.drawPixel(marker_x, kMarkerTop,     marker_ink);
    matrix.drawPixel(marker_x, kMarkerTop + 1, marker_ink);
    matrix.drawPixel(marker_x, kMarkerBot,     marker_ink);
    matrix.drawPixel(marker_x, kMarkerBot + 1, marker_ink);

    // ── 6. Labels — name + wavelength, BODY font, theme-toned ──────
    // During SETTLED: full LABEL / VALUE inks.
    // During SWEEPING: ghost the *target* label at LABEL_HALO-ish
    // dimness so the panel isn't blank for 1.5 s while the marker is
    // in flight. Matches the user-memory `color-perception.md` advice
    // — same hue, much lower V — by routing through theme::DIVIDER
    // (the canonical "1-px separator / dim hint" role).
    const Line&    line = kLines[m_idx];
    char wave_buf[8];
    snprintf(wave_buf, sizeof(wave_buf), "%u NM",
             static_cast<unsigned>(line.nm));

    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    uint16_t name_ink, wave_ink;
    if (m_phase == Phase::SETTLED) {
      name_ink = theme::ink(theme::Ink::VALUE);
      wave_ink = theme::ink(theme::Ink::LABEL);
    } else {
      // Ghost while sweeping — same hue, low V.
      name_ink = theme::ink(theme::Ink::DIVIDER);
      wave_ink = theme::ink(theme::Ink::DIVIDER);
    }
    // Element name + wavelength render flat (no halo) — the labels sit
    // on the black panel band beneath the bar so the 9× cost of a halo
    // pass would buy nothing legibility-wise.

    const int16_t name_x =
        gfx::centered_x(matrix, line.name);
    matrix.setTextColor(name_ink);
    matrix.setCursor(name_x, kNameBaseY);
    matrix.print(line.name);

    const int16_t wave_x =
        gfx::centered_x(matrix, wave_buf);
    matrix.setTextColor(wave_ink);
    matrix.setCursor(wave_x, kWaveBaseY);
    matrix.print(wave_buf);
  }

private:
  enum class Phase : uint8_t { SWEEPING, SETTLED };

  uint32_t m_segment_start_ms = 0;
  uint8_t  m_idx              = 0;
  uint8_t  m_from_x           = 0;
  uint8_t  m_to_x             = 0;
  Phase    m_phase            = Phase::SWEEPING;
};
