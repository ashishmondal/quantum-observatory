// planet_zoo_demo — manually browse every planet_renderer preset.
//
// Diagnostic scene that lets the operator browse the kSolarPresets[]
// table plus a few hash-derived seeds from the IR remote. There is
// NO automatic / timer-based cycling — every change is an explicit
// key press, so a body stays on screen as long as the operator is
// looking at it.
//
// Controls (require OK first to enter focus mode — see ir_actions):
//   LEFT  — previous body (wraps)
//   RIGHT — next body     (wraps)
//   UP    — next larger   render radius  (TINY → SMALL → LARGE)
//   DOWN  — next smaller  render radius
//   OK    — (entered focus mode; ignored thereafter)
//   BACK / HOME — exit focus + return to CLOCK (handled globally)
//
// Layout (64×32):
//   y= 0..6   "[ZOO]" header (theme HEADER ink + brackets/halo)
//   y= 9..14  Body name (BODY font; typewriter-revealed) — left half
//   y=17..22  LOD tier + r=NN tag                         — left half
//   y=25..30  8-char hex hash of the seeded name          — left half
//   right half (x≈32..63): the planet, parked at cx=48, cy=16. The
//             planet still rotates over time (longitude spin driven
//             by now_ms) so motion stays visible while the operator
//             dwells on a body.
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
    // First-render gate; updated lazily in render() below.
    m_have_typewriter_anchor = false;
    m_typewriter_anchor_ms   = 0;
    m_idx        = 0;
    m_radius_lvl = kDefaultRadiusLvl;
    m_last_seeded_idx = 0xFF;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    // Anchor the typewriter clock on first render so the name
    // initially types in instead of jumping straight to fully-typed.
    if (!m_have_typewriter_anchor) {
      m_typewriter_anchor_ms   = now_ms;
      m_have_typewriter_anchor = true;
    }

    matrix.fillScreen(0x0000);

    // Re-seed only on body change (cheap idx-equality gate).
    if (m_idx != m_last_seeded_idx) {
      m_planet.seed(kEntries[m_idx].name);
      m_last_seeded_idx = m_idx;
    }

    // ── Radius / LOD tag from the manual radius level ───────────────
    const uint8_t r = kRadiusLadder[m_radius_lvl];
    const char* lod_tag =
        (r <= 3)  ? "TINY" :
        (r <= 9)  ? "SMAL" :
                    "LARG";

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
    const char* src = kEntries[m_idx].name;
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

    // Typewriter ms is relative to the most recent body change so
    // each new selection retypes fresh. Wrap-safe (§2).
    const uint32_t tw_ms = now_ms - m_typewriter_anchor_ms;
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

  // IR navigation. Delivered by the compositor only while this scene
  // is in focus mode (operator pressed OK). BACK / HOME never arrive
  // here — the dispatch layer handles those as exit-focus + CLOCK.
  void on_key(SceneKey key, uint32_t now_ms) override {
    switch (key) {
      case SceneKey::LEFT:
        m_idx = static_cast<uint8_t>(
            (m_idx == 0) ? (kEntryCount - 1) : (m_idx - 1));
        // Retype the name + LOD line from the start of the new body.
        m_typewriter_anchor_ms   = now_ms;
        m_have_typewriter_anchor = true;
        break;
      case SceneKey::RIGHT:
        m_idx = static_cast<uint8_t>((m_idx + 1) % kEntryCount);
        m_typewriter_anchor_ms   = now_ms;
        m_have_typewriter_anchor = true;
        break;
      case SceneKey::UP:
        if (m_radius_lvl + 1 < kRadiusLevels) {
          m_radius_lvl = static_cast<uint8_t>(m_radius_lvl + 1);
          // Retype the LOD line so the new tier is obvious.
          m_typewriter_anchor_ms   = now_ms;
          m_have_typewriter_anchor = true;
        }
        break;
      case SceneKey::DOWN:
        if (m_radius_lvl > 0) {
          m_radius_lvl = static_cast<uint8_t>(m_radius_lvl - 1);
          m_typewriter_anchor_ms   = now_ms;
          m_have_typewriter_anchor = true;
        }
        break;
      case SceneKey::OK:
        // Already focused — nothing meaningful to commit. Ignored.
        break;
    }
  }

 private:
  // Radius ladder — one entry per LOD step the renderer can show.
  // TINY (3), SMALL low/high (5, 8), LARGE low/mid/high (10, 13, 16).
  // UP/DOWN walk this array; index is clamped at the ends (no wrap,
  // so the operator gets a tactile "that's as big/small as it gets"
  // rather than a surprise jump).
  static constexpr uint8_t kRadiusLadder[]   = { 3, 5, 8, 10, 13, 16 };
  static constexpr uint8_t kRadiusLevels     =
      sizeof(kRadiusLadder) / sizeof(kRadiusLadder[0]);
  static constexpr uint8_t kDefaultRadiusLvl = 4;  // r=13 — a comfortable LARGE.

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

  uint8_t                  m_idx              = 0;            // current entry
  uint8_t                  m_radius_lvl       = kDefaultRadiusLvl;
  uint8_t                  m_last_seeded_idx  = 0xFF;          // cached planet seed
  uint32_t                 m_typewriter_anchor_ms = 0;         // resets on key press
  bool                     m_have_typewriter_anchor = false;
  planet::ProceduralPlanet m_planet;
};
