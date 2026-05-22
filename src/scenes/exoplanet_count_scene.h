// exoplanet_count scene — animated procedural planet + NASA Exoplanet
// Archive stats (phase 7.7, FUTURE_SCENES Tier 2 "astrophysics 20%").
//
// Educational scene that turns a daily NASA Exoplanet Archive snapshot
// into something kid-shaped. Three lines of typewriter telemetry sit
// over a procedurally rendered planet whose look is seeded from the
// nearest-known exoplanet's name (FNV-1a → archetype + hue + bands).
// Same name → same planet across reboots and theme switches; a wire
// update to a different `nearest_name` re-seeds visibly.
//
// Intro animation (driven by now_ms − scene_enter_ms):
//
//   t = 0..600 ms — DOT
//     One lit pixel at (32, 16) — the planet "ignites" in the centre
//     of the panel.
//
//   t = 600..1400 ms — ZOOM
//     Center stays (32, 16); radius lerps 1 → 16 with an ease-out
//     cubic so the planet inflates fast and settles slowly.
//
//   t = 1400..1900 ms — SLIDE
//     Center lerps (32, 16) → (50, 16); radius lerps 16 → 12.
//     The planet parks at ~75 % of panel height in the right half,
//     leaving x = 0..31 for the typewriter readout.
//
//   t ≥ 1900 ms — SETTLED
//     Planet rotates slowly in place (longitude phase advances ~one
//     turn per 30 s) and the three Picopixel data lines type out in
//     the left half via the shared typewriter::compute() kernel.
//
// Lines (left half, baselines y = 14 / 22 / 30 — same as jupiter):
//   line 1: TOT %u            e.g. "TOT 5847"
//   line 2: %+d WK            e.g. "+12 WK"    (signed; "+0 WK" when absent)
//   line 3: NR %u.%u LY       e.g. "NR 4.2 LY" ("NR ? LY" when absent)
//
// Per FR-9.3 this scene OPTS OUT of the FR-9.2 top-right HH:MM clock
// chrome — the planet parks across the chrome region from t ≥ 1400 ms
// and the corner would be defaced. Matches GiantClockScene's same
// trade-off (composed-art scenes own their corner real estate).
//
// On scene re-entry the animation restarts cleanly: scene_enter_ms is
// stamped in init() so a request → revert → request cycle re-runs the
// dot/zoom/slide rhythm rather than parachuting straight into SETTLED.
//
// The procedural planet is re-seeded only when nearest_name changes —
// hash is cached so steady-state frames cost one strcmp instead of
// the full seed() pass.

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "exoplanet_state.h"
#include "gfx_text.h"
#include "planet_renderer.h"
#include "scene.h"
#include "theme.h"
#include "typewriter.h"

class ExoplanetCountScene : public Scene {
 public:
  const char* name() const override { return "exoplanet_count"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_enter_ms      = 0;          // reset on first render() call
    m_have_enter    = false;
    m_seeded_name[0] = '\0';
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_have_enter) {
      m_enter_ms   = now_ms;
      m_have_enter = true;
    }
    const uint32_t t = now_ms - m_enter_ms;  // wrap-safe per CODING_PRACTICES §2

    // Universal black background — scene composites over it.
    matrix.fillScreen(0x0000);

    // ── Snapshot ─────────────────────────────────────────────────────
    exoplanet_state::Snapshot snap;
    const bool fresh = exoplanet_state::get(now_ms, &snap);

    // Re-seed planet only when nearest_name actually changes (cheap
    // strcmp gate against the cached name). When stale we keep the
    // previously-seeded planet so the scene doesn't visually thrash
    // on a brief publish gap.
    if (fresh && snap.nearest_name[0] != '\0' &&
        strncmp(snap.nearest_name, m_seeded_name,
                exoplanet_state::kNameCap) != 0) {
      m_planet.seed(snap.nearest_name);
      strncpy(m_seeded_name, snap.nearest_name,
              exoplanet_state::kNameCap);
      m_seeded_name[exoplanet_state::kNameCap - 1] = '\0';
    } else if (m_seeded_name[0] == '\0') {
      // First-ever frame with no fresh data: seed a deterministic
      // placeholder so the intro still has SOMETHING to inflate.
      m_planet.seed("?");
      m_seeded_name[0] = '?';
      m_seeded_name[1] = '\0';
    }

    // ── Animation phase resolution ───────────────────────────────────
    int16_t cx; int16_t cy = 16;
    uint8_t r;
    bool    settled = false;
    if (t < kDotEnd) {
      cx = 32; r = 1;
    } else if (t < kZoomEnd) {
      // Ease-out cubic on radius. progress p in [0,1] → 1-(1-p)^3.
      const uint32_t span = kZoomEnd - kDotEnd;
      const uint32_t lp   = t - kDotEnd;          // 0..span
      const uint32_t inv  = span - lp;            // span..0
      // r_lerp ≈ 1 + 15 * (1 - (inv/span)^3)
      const uint32_t inv3 = (inv * inv / span) * inv / span;  // inv³/span² ∈ [0,span]
      const uint32_t eased = span > 0
          ? (1000u - (1000u * inv3) / span)        // 0..1000
          : 1000u;
      r  = static_cast<uint8_t>(1 + (15u * eased) / 1000u);
      cx = 32;
    } else if (t < kSlideEnd) {
      // Linear slide from (32,16,r=16) → (50,16,r=12).
      const uint32_t span = kSlideEnd - kZoomEnd;
      const uint32_t lp   = t - kZoomEnd;          // 0..span
      cx = static_cast<int16_t>(32 + (18 * int32_t(lp)) / int32_t(span));
      r  = static_cast<uint8_t>(16 - (4u * lp) / span);
    } else {
      cx = 50; r = 12; settled = true;
    }

    // ── Planet ───────────────────────────────────────────────────────
    // Rotation byte: only meaningful in SETTLED, but harmless during
    // intro (advances regardless; the body isn't visible as a sphere
    // yet during DOT/ZOOM-start anyway).
    const uint8_t time_phase = static_cast<uint8_t>((now_ms >> 7) & 0xFFu);
    m_planet.render(matrix, cx, cy, r, time_phase);

    // ── Header [EXO] — STATUS_INFO ink, theme-owned brackets/halo ───
    // Skip during DOT phase so the "ignition" pixel is alone for the
    // first 600 ms; fade in (full bright) from t ≥ 600 ms.
    if (t >= kDotEnd) {
      gfx::draw_scene_header(matrix, "EXO", /*x_unused=*/0, /*y=*/0,
                             theme::ink(theme::Ink::STATUS_INFO));
    }

    // ── Typewriter lines ─────────────────────────────────────────────
    // Only paint in SETTLED — during intro the screen belongs to the
    // planet. Reuses the shared 3-line typewriter kernel.
    if (!settled) return;

    char line1[16], line2[16], line3[16];
    if (fresh) {
      snprintf(line1, sizeof(line1), "TOT %lu",
               static_cast<unsigned long>(snap.total_count));
      if (snap.have_added_recent) {
        snprintf(line2, sizeof(line2), "%+d WK",
                 static_cast<int>(snap.added_recent));
      } else {
        snprintf(line2, sizeof(line2), "+0 WK");
      }
      if (snap.have_nearest_distance) {
        const uint32_t d = snap.nearest_distance_ly_x10;
        snprintf(line3, sizeof(line3), "NR %lu.%luLY",
                 static_cast<unsigned long>(d / 10u),
                 static_cast<unsigned long>(d % 10u));
      } else {
        snprintf(line3, sizeof(line3), "NR ?LY");
      }
    } else {
      // No fresh snapshot — render the WAIT triple on the data lines.
      snprintf(line1, sizeof(line1), "TOT ?");
      snprintf(line2, sizeof(line2), "WAIT");
      snprintf(line3, sizeof(line3), "NR ?");
    }

    const uint8_t lens[3] = {
      static_cast<uint8_t>(strlen(line1)),
      static_cast<uint8_t>(strlen(line2)),
      static_cast<uint8_t>(strlen(line3)),
    };
    constexpr uint8_t kBaselineY[3] = { 14, 22, 30 };
    const uint32_t tw_ms = t - kSlideEnd;   // typewriter time starts at SETTLED
    const typewriter::Schedule tw = typewriter::compute(tw_ms, lens);

    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    const char* lines[3] = { line1, line2, line3 };
    const uint16_t inks[3] = {
      theme::ink(theme::Ink::STATUS_INFO),    // TOT — cool info
      theme::ink(fresh && snap.have_added_recent && snap.added_recent >= 0
                     ? theme::Ink::STATUS_OK
                     : theme::Ink::STATUS_WARN_DIM),  // +WK growth tinted
      theme::ink(fresh ? theme::Ink::STATUS_WARN
                       : theme::Ink::STATUS_STALE),   // NR — highlight or stale
    };

    for (int i = 0; i < 3; ++i) {
      const uint8_t n = tw.typed[i];
      if (n == 0 && tw.active != i) continue;

      char prefix[16];
      memcpy(prefix, lines[i], n);
      prefix[n] = '\0';
      const int16_t by = static_cast<int16_t>(kBaselineY[i]);

      // Measure typed prefix so the backdrop is exactly its width.
      int16_t  bx, by_u; uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (n > 0) {
        matrix.getTextBounds(prefix, 1, by, &bx, &by_u, &bw, &bh);
        prefix_px = bw;
      }

      // Black backdrop strip behind the typed glyphs only — the
      // planet stays visible where text hasn't typed yet (it sits
      // mostly in x ≥ 38 anyway, but the backdrop also protects
      // readability from any halo bleed).
      const int16_t bg_y = static_cast<int16_t>(by - 6);
      int16_t bg_w_total = (n > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
      if (tw.active == i && tw.cursor_on) bg_w_total += 4;
      if (bg_w_total > 0) {
        matrix.fillRect(0, bg_y + 2, bg_w_total - 1, 5, 0x0000);
      }

      if (n > 0) {
        matrix.setTextColor(inks[i]);
        matrix.setCursor(1, by);
        matrix.print(prefix);
      }
      if (tw.active == i && tw.cursor_on) {
        const int16_t cur_x = static_cast<int16_t>(1 + prefix_px + 1);
        matrix.fillRect(cur_x, bg_y + 1, 3, 7, inks[i]);
      }
    }
  }

  // FR-9.3 carve-out: the planet's parked position (cx=50, r=12)
  // occupies the top-right HH:MM region from t ≥ 1400 ms. Letting
  // the compositor paint chrome on top would either fight the
  // planet's halo or hide it. Same trade-off GiantClockScene makes.
  bool wants_clock_chrome() const override { return false; }

 private:
  // Animation milestones — see header docstring for the rhythm.
  static constexpr uint32_t kDotEnd   = 600;
  static constexpr uint32_t kZoomEnd  = 1400;
  static constexpr uint32_t kSlideEnd = 1900;

  uint32_t                 m_enter_ms   = 0;
  bool                     m_have_enter = false;
  planet::ProceduralPlanet m_planet;
  char                     m_seeded_name[exoplanet_state::kNameCap] = {0};
};
