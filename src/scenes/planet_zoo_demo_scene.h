// planet_zoo_demo — visually inspect every planet_renderer preset.
//
// Diagnostic scene that walks through the kSolarPresets[] table and
// (optionally) a few hash-derived seeds, parking each on the right
// half of the panel at three radii in sequence — TINY → SMALL →
// LARGE — so a single dwell exercises all three LOD code paths in
// the renderer for that body. The body name + LOD tag types out on
// the left half so the operator can see which planet is on screen
// at a glance.
//
// Layout (64×32):
//   y= 0..6   "[ZOO]" header (theme HEADER ink + brackets/halo)
//   y= 9..14  Body name (BODY font; typewriter-revealed) — left half
//   y=17..22  "LOD tier" + r=NN tag (BODY font)             — left half
//   y=25..30  Hash hex (BODY font) — present for hash-seeded entries,
//             blank for preset entries (no hash on a preset hit)
//   right half (x≈32..63): the planet, parked at cx=48, cy=16. Radius
//             cycles 3 → 7 → 13 over each body's dwell so the eye
//             sees the algorithm step-up live.
//
// Dwell schedule (per body): TINY 1.2 s → SMALL 1.8 s → LARGE 4.0 s.
// Total = 7.0 s × N bodies. The list cycles forever.
//
// Per FR-9.3 this scene OPTS OUT of the FR-9.2 top-right HH:MM clock
// chrome — the planet parks across the chrome region.

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "gfx_text.h"
#include "planet_renderer.h"
#include "scene.h"
#include "theme.h"
#include "typewriter.h"

class PlanetZooDemoScene : public Scene {
 public:
  const char* name() const override { return "planet_zoo_demo"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_enter_ms   = 0;
    m_have_enter = false;
    m_last_idx   = 0xFF;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_have_enter) {
      m_enter_ms   = now_ms;
      m_have_enter = true;
    }
    const uint32_t t      = now_ms - m_enter_ms;     // wrap-safe (§2)
    const uint32_t cycle  = t % kDwellTotalMs;
    const uint8_t  idx    = static_cast<uint8_t>(
        (t / kDwellTotalMs) % kEntryCount);

    matrix.fillScreen(0x0000);

    // Re-seed only on entry change (cheap strcmp gate against cached idx).
    if (idx != m_last_idx) {
      m_planet.seed(kEntries[idx].name);
      m_last_idx = idx;
    }

    // ── Radius from the per-body schedule ───────────────────────────
    uint8_t r;
    const char* lod_tag;
    if (cycle < kTinyEndMs) {
      r       = 3;
      lod_tag = "TINY";
    } else if (cycle < kSmallEndMs) {
      r       = 7;
      lod_tag = "SMAL";
    } else {
      r       = 13;
      lod_tag = "LARG";
    }

    // ── Planet, parked on the right half ────────────────────────────
    const uint8_t time_phase = static_cast<uint8_t>((now_ms >> 6) & 0xFFu);
    m_planet.render(matrix, /*cx=*/48, /*cy=*/16, r, time_phase);

    // ── Header [ZOO] ────────────────────────────────────────────────
    gfx::draw_scene_header(matrix, "ZOO", /*x_unused=*/0, /*y=*/0,
                           theme::ink(theme::Ink::HEADER));

    // ── Typewriter lines on the left half ───────────────────────────
    // line 1: body name (upper-cased into a small local buffer; theme
    //         BODY font is variable-width Picopixel, room for ~8 chars).
    // line 2: "LOD r=NN"
    // line 3: hash hex (8 chars) — visible for both preset + hash hits
    //         since the renderer hashes the name regardless.
    char line1[12], line2[12], line3[12];
    const char* src = kEntries[idx].name;
    size_t n = 0;
    while (src[n] != '\0' && n < sizeof(line1) - 1) {
      char c = src[n];
      if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
      line1[n] = c;
      ++n;
    }
    line1[n] = '\0';
    snprintf(line2, sizeof(line2), "%s r%u", lod_tag, unsigned(r));
    snprintf(line3, sizeof(line3), "%08lX",
             static_cast<unsigned long>(m_planet.hash()));

    const char*   lines[3] = { line1, line2, line3 };
    const uint8_t lens[3]  = {
      static_cast<uint8_t>(strlen(line1)),
      static_cast<uint8_t>(strlen(line2)),
      static_cast<uint8_t>(strlen(line3)),
    };
    constexpr uint8_t kBaselineY[3] = { 14, 22, 30 };

    // Typewriter ms reset on every body change so each preset gets a
    // fresh type-in pass.
    const uint32_t tw_ms = cycle;
    const typewriter::Schedule tw = typewriter::compute(tw_ms, lens);

    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    const uint16_t inks[3] = {
      theme::ink(theme::Ink::VALUE),         // name — bright
      theme::ink(theme::Ink::LABEL),         // LOD tag — dim
      theme::ink(theme::Ink::STATUS_INFO),   // hash — cool tint
    };

    for (int i = 0; i < 3; ++i) {
      const uint8_t typed = tw.typed[i];
      if (typed == 0 && tw.active != i) continue;

      char prefix[12];
      memcpy(prefix, lines[i], typed);
      prefix[typed] = '\0';
      const int16_t by = static_cast<int16_t>(kBaselineY[i]);

      // Measure typed prefix so the backdrop matches exactly.
      int16_t  bx, by_u;
      uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (typed > 0) {
        matrix.getTextBounds(prefix, 1, by, &bx, &by_u, &bw, &bh);
        prefix_px = bw;
      }

      // Black backdrop strip behind typed glyphs only — keeps the
      // planet visible on the right while readability stays clean
      // on the left.
      const int16_t bg_y = static_cast<int16_t>(by - 6);
      int16_t bg_w_total = (typed > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
      if (tw.active == i && tw.cursor_on) bg_w_total += 4;
      if (bg_w_total > 0) {
        matrix.fillRect(0, bg_y + 2, bg_w_total - 1, 5, 0x0000);
      }

      if (typed > 0) {
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

  // Planet parks across the chrome region — same trade-off as
  // ExoplanetCountScene / GiantClockScene.
  bool wants_clock_chrome() const override { return false; }

 private:
  // Dwell schedule (per body). Times are cumulative within one body's
  // dwell — TINY runs 0..kTinyEndMs, SMALL kTinyEndMs..kSmallEndMs,
  // LARGE kSmallEndMs..kDwellTotalMs.
  static constexpr uint32_t kTinyEndMs    = 1200;
  static constexpr uint32_t kSmallEndMs   = 3000;
  static constexpr uint32_t kDwellTotalMs = 7000;

  // The set walked by the demo. First N entries match kSolarPresets[]
  // by name (renderer will hit the preset table); the trailing entries
  // exercise the hash-derived path so the operator can confirm both
  // code paths are visually distinct. Names kept short for the 1-line
  // header buffer (8-char cap after upper-casing).
  struct Entry { const char* name; };
  static constexpr Entry kEntries[] = {
    {"mercury"},  {"venus"},   {"earth"},    {"mars"},
    {"jupiter"},  {"saturn"},  {"uranus"},   {"neptune"},
    {"pluto"},    {"sun"},     {"moon"},
    {"io"},       {"europa"},  {"ganymede"}, {"callisto"},
    {"titan"},
    // Hash-derived seeds — exercise the random-archetype path.
    {"kepler22b"}, {"trapp1e"}, {"prox-b"}, {"hd189733"},
  };
  static constexpr uint8_t kEntryCount =
      sizeof(kEntries) / sizeof(kEntries[0]);

  uint32_t                 m_enter_ms   = 0;
  bool                     m_have_enter = false;
  uint8_t                  m_last_idx   = 0xFF;
  planet::ProceduralPlanet m_planet;
};
