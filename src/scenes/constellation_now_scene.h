// constellation_now scene — renders one of the 88 IAU constellations
// from the auto-generated `include/stars.h` catalog (Phase 7.4).
//
// Data source (NEW in this revision):
//   * `stars::kStars[]`             — Hipparcos stars filtered to
//     mag < 6.0 (~5000 entries), produced by
//     `tools/skyculture_to_header.py` from the Stellarium "western"
//     sky culture + the HYG database. Each entry has real RA/Dec
//     in milliarcseconds and a V magnitude × 100.
//   * `constellations_iau::kCatalog[]` — 88 IAU entries, each a list
//     of `LineSegment{a,b}` pairs that index into kStars[].
//
// Why per-frame projection (was: hand-tuned 32×32 layouts):
//   The previous revision shipped 4 hand-laid constellations
//   (Orion / Big Dipper / Cassiopeia / Lyra) with art-directed pixel
//   coordinates. Scaling that to 88 entries by hand is a non-starter.
//   Real RA/Dec at 32×32 *does* read recognisably as long as we:
//     1. project per-constellation (not the whole sky), so the
//        bounding box fills the panel half;
//     2. apply cos(dec_center) RA scaling so circumpolar shapes
//        don't squash;
//     3. preserve aspect ratio (the smaller of the two axis scales
//        wins) so Orion's tall-narrow box doesn't stretch into a
//        rectangle;
//     4. flip x so east is on the LEFT (matches naked-eye view
//        when facing south — the dashboard is a sky simulator, not
//        a map).
//
// Layout (64×32) — unchanged from the 4-entry MVP:
//   left half  (x=0..31):
//     y= 0..7   "[CON]" header in built-in 5×7 mono, brackets pulse
//     y= 9..14  Typewriter line 1 — English name (e.g. "EAGLE")
//     y=17..22  Typewriter line 2 — Latin/native name (e.g. "AQUILA")
//                                   or featured-star name when one
//                                   is highlighted (e.g. "*BETELGEUSE")
//     y=25..30  Typewriter line 3 — "IAU xxx" (3-letter code)
//   right half (x=32..63): dynamic constellation render, projected
//     into the 30×26 sub-region (x=33..62, y=5..30) so chrome's
//     y=0..6 corner band sits over empty space.
//
// Star render bands (from `mag_x100`):
//   < 100  (mag < 1.0) — full + cross with tips, twinkle  (Sirius / Vega / Rigel)
//   < 200  (mag < 2.0) — + cross, no tips, no twinkle
//   < 300  (mag < 3.0) — single bright pixel
//   < 400  (mag < 4.0) — single dim pixel
//   else               — single very faint pixel
//
// Highlight contract (observatory/constellation MQTT):
//   `highlight_star` is the brightness rank within the rendered
//   constellation: 0 = brightest visible star, 1 = second brightest,
//   etc. This is independent of HIP numbers so HA can issue a
//   stable "highlight the alpha star" command without knowing the
//   catalog details.
//
// Source-of-truth precedence (mirrors moon/iss/jupiter):
//   1. observatory/constellation MQTT push (preferred).
//   2. Local fallback — rotates through the catalog every 30 s so
//      the scene works standalone.
//
// Per FR-9.3 this scene KEEPS the corner clock chrome.
//
// (revised in phase 7.4 — Stellarium catalog import)

#pragma once

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "constellation_state.h"
#include "gfx_text.h"
#include "scene.h"
#include "stars.h"
#include "theme.h"

class ConstellationNowScene : public Scene {
public:
  const char* name() const override { return "constellation_now"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    m_cache.entry_idx = kCacheInvalid;
  }

  // FR-16.4 / phase D.7: pre-pack the projection of the most-likely
  // entry so the first post-swap render() finds the cache primed.
  // Idempotent — same-entry calls short-circuit on the cache hit.
  // Touches no framebuffer state (only m_cache); safe to call while
  // the previous scene is still rendering.
  void prepare(uint32_t now_ms) override {
    const uint8_t idx = pick_entry_idx(now_ms);
    if (m_cache.entry_idx != idx) compute_projection(idx);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    using constellations_iau::kCatalog;
    using constellations_iau::kCatalogCount;

    // ── Pick which constellation to render ─────────────────────────
    constellation_state::Snapshot snap;
    const bool have_mqtt = constellation_state::get(now_ms, &snap);

    uint8_t entry_idx;
    bool    have_highlight = false;
    uint8_t highlight_rank = 0;
    if (have_mqtt && snap.index < kCatalogCount) {
      entry_idx = snap.index;
      if (snap.have_highlight) {
        have_highlight = true;
        highlight_rank = snap.highlight_star;
      }
    } else {
      // Local rotation. Wall clock so the cycle survives reboots.
      entry_idx = static_cast<uint8_t>(
          (now_ms / 30000u) % kCatalogCount);
    }
    const constellations_iau::Entry& entry = kCatalog[entry_idx];

    matrix.fillScreen(0x0000);  // universal background

    // Projection is cached across frames keyed by entry_idx — the
    // dedup + insertion sort + cos(dec) projection pass costs ~150 µs
    // on RP2040 soft-float and used to run every frame; with the
    // cache it only runs on entry change (every 30 s in fallback,
    // or on MQTT push). Phase D.7 prepare() primes this cache during
    // the fade-out window of a swap so the first post-swap frame
    // hits the same fast path as steady-state.
    if (m_cache.entry_idx != entry_idx) compute_projection(entry_idx);

    const uint8_t   unique_count = m_cache.unique_count;
    const uint16_t* const uniques = m_cache.uniques;
    const int16_t*  const spx     = m_cache.spx;
    const int16_t*  const spy     = m_cache.spy;

    if (unique_count == 0) {
      // No drawable lines — render a WAIT placeholder.
      matrix.setFont(theme::font(theme::FontRole::BODY));
      matrix.setTextColor(theme::ink(theme::Ink::STATUS_STALE));
      matrix.setCursor(2, 14);
      matrix.print("CONST");
      matrix.setCursor(2, 22);
      matrix.print("WAIT");
      return;
    }

    // ── Lines first (so star marks draw on top) ────────────────────
    // Very dim warm grey — the asterism is a "guide", not data; the
    // eye should land on the stars first and only then trace the
    // connection. Dropped from 0x18C3 to 0x0841 (one bit per channel)
    // after on-panel review showed the lines competing with mag<3
    // single-pixel stars.
    constexpr uint16_t kLineInk = 0x0841;
    for (uint16_t i = 0; i < entry.line_count; ++i) {
      const int8_t ia = find_local(uniques, unique_count, entry.lines[i].a);
      const int8_t ib = find_local(uniques, unique_count, entry.lines[i].b);
      if (ia < 0 || ib < 0) continue;
      matrix.drawLine(spx[ia], spy[ia], spx[ib], spy[ib], kLineInk);
    }

    // ── Stars on top of lines ──────────────────────────────────────
    for (uint8_t i = 0; i < unique_count; ++i) {
      const stars::Star& s = stars::kStars[uniques[i]];
      const int16_t px = spx[i];
      const int16_t py = spy[i];

      // Highlight wins over magnitude.
      const bool is_highlight = (have_highlight && i == highlight_rank);
      if (is_highlight) {
        // Region clip bounds — must mirror the kRegionX/kRegionY in
        // compute_projection() and in_region(). Local rather than
        // class-level so the projection geometry stays a single
        // self-contained block.
        constexpr int16_t kRegionX = 33;
        constexpr int16_t kRegionY = 5;
        const bool blink_on = ((now_ms / 400u) & 1u) == 0u;
        const uint16_t hl_center = blink_on ? 0xF800 : 0x8000;
        const uint16_t hl_arm    = blink_on ? 0x8000 : 0x4000;
        const uint16_t hl_tip    = blink_on ? 0x4000 : 0x2000;
        if (in_region(px, py))      matrix.drawPixel(px, py, hl_center);
        if (px - 1 >= kRegionX)     matrix.drawPixel(px - 1, py, hl_arm);
        if (px + 1 < PANEL_WIDTH)   matrix.drawPixel(px + 1, py, hl_arm);
        if (py - 1 >= kRegionY)     matrix.drawPixel(px, py - 1, hl_arm);
        if (py + 1 < PANEL_HEIGHT)  matrix.drawPixel(px, py + 1, hl_arm);
        if (px - 2 >= kRegionX)     matrix.drawPixel(px - 2, py, hl_tip);
        if (px + 2 < PANEL_WIDTH)   matrix.drawPixel(px + 2, py, hl_tip);
        if (py - 2 >= kRegionY)     matrix.drawPixel(px, py - 2, hl_tip);
        if (py + 2 < PANEL_HEIGHT)  matrix.drawPixel(px, py + 2, hl_tip);
        continue;
      }

      // Magnitude buckets. Without spectral class data in the parsed
      // catalog (HYG has it but the converter doesn't extract it
      // yet) every star renders neutral white — easy upgrade later.
      const uint16_t kInk = theme::ink(theme::Ink::ACCENT);  // white pop
      const int16_t mag = s.mag_x100;
      if (mag < 100) {
        // mag < 1.0 — full + cross with tips + twinkle.
        // Integer triangle wave 0..127 → modulates center
        // brightness in [75%, 100%]. No float (NFR-1.3).
        const uint32_t tw = (now_ms >> 2) & 0xFF;
        const uint8_t  tri = (tw < 128) ? tw : (255 - tw);
        const uint8_t  k_pct = static_cast<uint8_t>(75 + (tri * 25) / 127);
        const uint16_t center_ink = scale_ink(kInk, k_pct);
        const uint16_t arm_ink    = scale_ink(kInk, 25);
        const uint16_t tip_ink    = scale_ink(kInk, 10);
        draw_star_cross(matrix, px, py, center_ink, arm_ink, tip_ink);
      } else if (mag < 200) {
        const uint16_t arm_ink = scale_ink(kInk, 25);
        draw_star_cross(matrix, px, py, kInk, arm_ink, 0);
      } else if (mag < 300) {
        if (in_region(px, py)) matrix.drawPixel(px, py, kInk);
      } else if (mag < 400) {
        if (in_region(px, py)) {
          matrix.drawPixel(px, py,
              static_cast<uint16_t>((kInk >> 1) & 0x7BEF));
        }
      } else {
        if (in_region(px, py)) {
          matrix.drawPixel(px, py,
              static_cast<uint16_t>((kInk >> 2) & 0x39E7));
        }
      }
    }

    // ── Left half: header + typewriter readout ─────────────────────    // Cool blue-white header is scene-identity (constellation =
    // "sky" tonality, distinct from ISS green / JUP amber / MOON
    // grey). Stays inline per CODING_PRACTICES §4 — scene-internal
    // identity color, not a generic STATUS_* role.
    const bool header_dim = (now_ms % 1500u) < 200u;
    constexpr uint16_t kHeaderInk = 0xAFFF;  // cool blue-white
    constexpr uint16_t kHeaderDim = 0x4A1F;
    // Brackets / halo / BLOCK_BARS routing owned by theme via
    // gfx::draw_scene_header (T.7a). Identity hue stays inline
    // (CODING_PRACTICES §4 — scene-internal cool-blue, not STATUS_*).
    gfx::draw_scene_header(matrix, "CON", 1, 0,
                           header_dim ? kHeaderDim : kHeaderInk);

    // Build readout lines. Convert names to upper case for the
    // dashboard's shouty-typewriter aesthetic.
    char line1_buf[16];
    char line2_buf[16];
    char line3_buf[16];
    upper_copy(line1_buf, entry.name_en,     sizeof(line1_buf));
    upper_copy(line2_buf, entry.name_native, sizeof(line2_buf));
    snprintf(line3_buf, sizeof(line3_buf), "IAU %s", entry.iau);

    // When highlighted AND the highlighted star has a proper name in
    // the catalog, line 2 swaps to "*STARNAME" (red ink).
    bool line2_is_star = false;
    if (have_highlight && highlight_rank < unique_count) {
      const uint16_t name_idx = stars::kStars[uniques[highlight_rank]].name_idx;
      if (name_idx != 0 && name_idx < stars::kStarNamesCount) {
        const char* sn = stars::kStarNames[name_idx];
        if (sn != nullptr && sn[0] != '\0') {
          char tmp[16];
          tmp[0] = '*';
          // Leave room for '*' + NUL.
          const size_t cap = sizeof(tmp) - 2;
          size_t k = 0;
          while (k < cap && sn[k] != '\0') { tmp[k + 1] = sn[k]; ++k; }
          tmp[k + 1] = '\0';
          upper_copy(line2_buf, tmp, sizeof(line2_buf));
          line2_is_star = true;
        }
      }
    }

    const char*  lines[3] = { line1_buf, line2_buf, line3_buf };
    const uint8_t lens[3] = {
      static_cast<uint8_t>(strlen(lines[0])),
      static_cast<uint8_t>(strlen(lines[1])),
      static_cast<uint8_t>(strlen(lines[2])),
    };
    constexpr uint8_t kBaselineY[3] = { 14, 22, 30 };

    constexpr uint32_t kCharMs  = 90;
    constexpr uint32_t kPauseMs = 600;
    constexpr uint32_t kHoldMs  = 2500;

    const uint32_t span1 = lens[0] * kCharMs + kPauseMs;
    const uint32_t span2 = lens[1] * kCharMs + kPauseMs;
    const uint32_t span3 = lens[2] * kCharMs + kPauseMs;
    const uint32_t total = span1 + span2 + span3 + kHoldMs;
    const uint32_t t     = now_ms % total;
    const uint32_t starts[3] = { 0u, span1, span1 + span2 };

    uint8_t typed[3] = { 0, 0, 0 };
    int8_t  active   = -1;
    for (int i = 0; i < 3; ++i) {
      if (t < starts[i]) {
        typed[i] = 0;
      } else {
        const uint32_t local = t - starts[i];
        const uint32_t typing_dur = lens[i] * kCharMs;
        if (local < typing_dur) {
          typed[i] = static_cast<uint8_t>(local / kCharMs);
          active = static_cast<int8_t>(i);
        } else {
          typed[i] = lens[i];
        }
      }
    }
    const bool cursor_on = ((now_ms / 280u) & 1u) == 0u;

    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    const uint16_t kNameInk      = theme::ink(theme::Ink::ACCENT);          // white pop
    const uint16_t kLatinInk     = theme::ink(theme::Ink::VALUE);           // ~80% white sub-name
    const uint16_t kHighlightInk = theme::ink(theme::Ink::ALERT);           // red — named star highlight
    const uint16_t kIauInk       = theme::ink(theme::Ink::STATUS_INFO);     // cyan IAU code

    for (int i = 0; i < 3; ++i) {
      if (typed[i] == 0 && active != i) continue;

      char prefix[16];
      const uint8_t n = typed[i];
      memcpy(prefix, lines[i], n);
      prefix[n] = '\0';

      int16_t  bx, by;
      uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (n > 0) {
        matrix.getTextBounds(prefix, 1, kBaselineY[i], &bx, &by, &bw, &bh);
        prefix_px = bw;
      }

      const int16_t bg_x = 0;
      const int16_t bg_y = static_cast<int16_t>(kBaselineY[i] - 6);
      const int16_t bg_h = 5;
      const int16_t bg_w = (n > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
      int16_t bg_w_total = bg_w;
      if (active == i && cursor_on) bg_w_total += 4;
      if (bg_w_total > 0) {
        matrix.fillRect(bg_x, bg_y + 2, bg_w_total - 1, bg_h, 0x0000);  // universal background
      }

      uint16_t ink = kNameInk;
      if (i == 1)      ink = line2_is_star ? kHighlightInk : kLatinInk;
      else if (i == 2) ink = kIauInk;

      if (n > 0) {
        matrix.setTextColor(ink);
        matrix.setCursor(1, kBaselineY[i]);
        matrix.print(prefix);
      }

      if (active == i && cursor_on) {
        const int16_t cx = static_cast<int16_t>(1 + prefix_px + 1);
        matrix.fillRect(cx, bg_y + 1, 3, 7, kNameInk);
      }
    }

    // matrix.show() is called by loop1() (phase 3.5.2).
  }

  // FR-9.3: keep the corner clock chrome.
  bool wants_clock_chrome() const override { return true; }

private:
  // Max unique stars per constellation entry. The IAU set tops out
  // at ~28 (Hercules); 64 is generous head-room and keeps the cache
  // a single ~512-byte instance member.
  static constexpr uint8_t kMaxUnique = 64;
  static constexpr uint8_t kCacheInvalid = 0xFF;

  // FR-16.4 / D.7 projection cache. Populated by compute_projection()
  // (idempotent — same entry_idx → same output). render() and
  // prepare() both consult this; on a hit, render() skips the dedup +
  // sort + cos(dec) trig and goes straight to drawing.
  struct ProjectionCache {
    uint8_t  entry_idx;             // kCacheInvalid until first compute
    uint8_t  unique_count;
    uint16_t uniques[kMaxUnique];   // brightness-sorted (uniques[0] = brightest)
    int16_t  spx[kMaxUnique];       // projected screen x
    int16_t  spy[kMaxUnique];       // projected screen y
  };
  ProjectionCache m_cache{kCacheInvalid, 0, {0}, {0}, {0}};

  // Reproduce render()'s entry pick exactly so prepare() and render()
  // agree on which constellation gets cached.
  static uint8_t pick_entry_idx(uint32_t now_ms) {
    using constellations_iau::kCatalogCount;
    constellation_state::Snapshot snap;
    if (constellation_state::get(now_ms, &snap)
        && snap.index < kCatalogCount) {
      return snap.index;
    }
    return static_cast<uint8_t>((now_ms / 30000u) % kCatalogCount);
  }

  // Dedup → brightness-sort → cos(dec) project the IAU entry into
  // m_cache. Cost: ~150 µs on RP2040 soft-float for a typical
  // ~12-star entry. NOT called from the per-pixel hot loop —
  // run-once-per-entry-change semantics, gated by the m_cache
  // entry_idx check at every call site.
  void compute_projection(uint8_t entry_idx) {
    using constellations_iau::kCatalog;
    using constellations_iau::kCatalogCount;
    if (entry_idx >= kCatalogCount) {
      m_cache.entry_idx    = entry_idx;
      m_cache.unique_count = 0;
      return;
    }
    const constellations_iau::Entry& entry = kCatalog[entry_idx];

    // ── Collect unique star indices ────────────────────────────────
    uint16_t* uniques = m_cache.uniques;
    uint8_t   unique_count = 0;
    for (uint16_t i = 0; i < entry.line_count; ++i) {
      const uint16_t pair[2] = { entry.lines[i].a, entry.lines[i].b };
      for (int k = 0; k < 2; ++k) {
        const uint16_t s = pair[k];
        bool found = false;
        for (uint8_t j = 0; j < unique_count; ++j) {
          if (uniques[j] == s) { found = true; break; }
        }
        if (!found && unique_count < kMaxUnique) {
          uniques[unique_count++] = s;
        }
      }
    }

    if (unique_count == 0) {
      m_cache.entry_idx    = entry_idx;
      m_cache.unique_count = 0;
      return;
    }

    // ── Brightness sort (insertion; uniques[0] = brightest) ────────
    for (uint8_t i = 1; i < unique_count; ++i) {
      for (uint8_t j = i; j > 0; --j) {
        if (stars::kStars[uniques[j]].mag_x100 <
            stars::kStars[uniques[j - 1]].mag_x100) {
          const uint16_t tmp = uniques[j];
          uniques[j] = uniques[j - 1];
          uniques[j - 1] = tmp;
        } else break;
      }
    }

    // ── Project (RA, Dec) → panel coords ───────────────────────────
    int64_t dec_sum_mas = 0;
    for (uint8_t i = 0; i < unique_count; ++i) {
      dec_sum_mas += stars::kStars[uniques[i]].dec_mas;
    }
    const int32_t dec_c_mas =
        static_cast<int32_t>(dec_sum_mas / unique_count);
    const float dec_c_rad =
        static_cast<float>(dec_c_mas) *
        (3.14159265358979f / (180.0f * 3600000.0f));
    int32_t cos_q15 =
        static_cast<int32_t>(cosf(dec_c_rad) * 32768.0f + 0.5f);
    if (cos_q15 < 1) cos_q15 = 1;  // poles → clamp to avoid div-zero

    int32_t flat_x[kMaxUnique], flat_y[kMaxUnique];
    int32_t x_min = INT32_MAX, x_max = INT32_MIN;
    int32_t y_min = INT32_MAX, y_max = INT32_MIN;
    for (uint8_t i = 0; i < unique_count; ++i) {
      const stars::Star& s = stars::kStars[uniques[i]];
      const int64_t fx =
          (static_cast<int64_t>(s.ra_mas) * cos_q15) >> 15;
      flat_x[i] = static_cast<int32_t>(fx);
      flat_y[i] = s.dec_mas;
      if (flat_x[i] < x_min) x_min = flat_x[i];
      if (flat_x[i] > x_max) x_max = flat_x[i];
      if (flat_y[i] < y_min) y_min = flat_y[i];
      if (flat_y[i] > y_max) y_max = flat_y[i];
    }
    int32_t x_span = x_max - x_min; if (x_span < 1) x_span = 1;
    int32_t y_span = y_max - y_min; if (y_span < 1) y_span = 1;

    // Render-region constants — must mirror render()'s draw region.
    constexpr int16_t kRegionX = 33;
    constexpr int16_t kRegionY = 5;
    constexpr int16_t kRegionW = 30;
    constexpr int16_t kRegionH = 26;

    // Isotropic scale: smaller axis budget wins. Same int64 multiply-
    // then-divide as the previous in-render path to avoid the Q16
    // underflow trap (see render-side comment archive).
    const int64_t lhs = static_cast<int64_t>(kRegionW) * y_span;
    const int64_t rhs = static_cast<int64_t>(kRegionH) * x_span;
    const bool    x_limits = (lhs < rhs);
    const int32_t scale_num = x_limits ? kRegionW : kRegionH;
    const int32_t scale_den = x_limits ? x_span   : y_span;

    const int32_t bbox_xc   = (x_min + x_max) / 2;
    const int32_t bbox_yc   = (y_min + y_max) / 2;
    const int16_t px_center = kRegionX + kRegionW / 2;
    const int16_t py_center = kRegionY + kRegionH / 2;

    for (uint8_t i = 0; i < unique_count; ++i) {
      const int32_t dx = static_cast<int32_t>(
          (static_cast<int64_t>(flat_x[i] - bbox_xc) * scale_num) /
          scale_den);
      const int32_t dy = static_cast<int32_t>(
          (static_cast<int64_t>(flat_y[i] - bbox_yc) * scale_num) /
          scale_den);
      // East-on-left + North-on-top — invert both axes.
      m_cache.spx[i] = static_cast<int16_t>(px_center - dx);
      m_cache.spy[i] = static_cast<int16_t>(py_center - dy);
    }

    m_cache.unique_count = unique_count;
    m_cache.entry_idx    = entry_idx;
  }

  // Render-region clip used by the single-pixel magnitude bands.
  // The cross-shape helper has its own bounds; this is for the bare
  // pixel cases where a stray projection could land in the chrome
  // band or the left-half readout panel.
  static inline bool in_region(int16_t x, int16_t y) {
    return x >= 33 && x < PANEL_WIDTH && y >= 5 && y < PANEL_HEIGHT;
  }

  // Find the position of a global star index inside our brightness-
  // sorted uniques[] table. Linear search; N ≤ 28.
  static inline int8_t find_local(const uint16_t* uniques,
                                  uint8_t unique_count,
                                  uint16_t global_idx) {
    for (uint8_t k = 0; k < unique_count; ++k) {
      if (uniques[k] == global_idx) return static_cast<int8_t>(k);
    }
    return -1;
  }

  // Scale an RGB565 colour by an integer percentage (0..100).
  // Pure-integer; no float (NFR-1.3).
  static inline uint16_t scale_ink(uint16_t ink, uint8_t pct) {
    const uint8_t r5 = static_cast<uint8_t>((ink >> 11) & 0x1F);
    const uint8_t g6 = static_cast<uint8_t>((ink >>  5) & 0x3F);
    const uint8_t b5 = static_cast<uint8_t>( ink        & 0x1F);
    const uint8_t r = static_cast<uint8_t>((r5 * pct) / 100);
    const uint8_t g = static_cast<uint8_t>((g6 * pct) / 100);
    const uint8_t b = static_cast<uint8_t>((b5 * pct) / 100);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
  }

  // Draw a + (cross): center pixel + 4 arm pixels + optional 4 tip
  // pixels. Bounds-clipped against the right-half render region so
  // an edge star can't bleed into the readout panel.
  static inline void draw_star_cross(Adafruit_Protomatter& matrix,
                                     int16_t cx, int16_t cy,
                                     uint16_t center_ink,
                                     uint16_t arm_ink,
                                     uint16_t tip_ink) {
    constexpr int16_t kMinX = 33;
    constexpr int16_t kMinY = 5;
    if (cx >= kMinX && cx < PANEL_WIDTH && cy >= kMinY && cy < PANEL_HEIGHT)
      matrix.drawPixel(cx, cy, center_ink);
    if (cx - 1 >= kMinX)         matrix.drawPixel(cx - 1, cy, arm_ink);
    if (cx + 1 < PANEL_WIDTH)    matrix.drawPixel(cx + 1, cy, arm_ink);
    if (cy - 1 >= kMinY)         matrix.drawPixel(cx, cy - 1, arm_ink);
    if (cy + 1 < PANEL_HEIGHT)   matrix.drawPixel(cx, cy + 1, arm_ink);
    if (tip_ink) {
      if (cx - 2 >= kMinX)       matrix.drawPixel(cx - 2, cy, tip_ink);
      if (cx + 2 < PANEL_WIDTH)  matrix.drawPixel(cx + 2, cy, tip_ink);
      if (cy - 2 >= kMinY)       matrix.drawPixel(cx, cy - 2, tip_ink);
      if (cy + 2 < PANEL_HEIGHT) matrix.drawPixel(cx, cy + 2, tip_ink);
    }
  }

  // Copy `src` into `dst`, uppercasing ASCII and NUL-terminating.
  // Truncates to dst_size-1 chars. Used to massage the catalog's
  // mixed-case names into the typewriter font's all-caps house style.
  static inline void upper_copy(char* dst, const char* src, size_t dst_size) {
    if (dst_size == 0) return;
    size_t k = 0;
    while (k + 1 < dst_size && src[k] != '\0') {
      const char c = src[k];
      dst[k] = (c >= 'a' && c <= 'z')
                   ? static_cast<char>(c - 'a' + 'A')
                   : c;
      ++k;
    }
    dst[k] = '\0';
  }
};
