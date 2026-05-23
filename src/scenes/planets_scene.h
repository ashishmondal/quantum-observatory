// planets scene — generic procedural-planet showcase that rotates
// through planet_catalog::kBodies[] on every scene re-entry.
//
// Replaces the body-specific `jupiter_visibility` scene with a single
// scene that can render any of the ~17 catalog entries (planets,
// moons, the Sun, the headline exoplanet). The active body index is
// persisted in prefs::current().planet_index so the cycle resumes
// across reboots, and is advanced once per init() so each entry to
// the scene during the standard rotation moves the cursor forward
// by one body. This keeps the catalog visible to the audience over
// time without manual interaction.
//
// ── Layout (64×32) ────────────────────────────────────────────────
//
//   Settled frame, left half (x = 0..31), Picopixel baselines:
//     y= 8   line 1 — display name + 3-letter type ("MERCURY PLA")
//     y=17   line 2 — distance ("0.4 AU" / "384 K KM" / "634 LY")
//     y=26   line 3 — fact     ("DAY=176D")
//   Right half (x ≥ 32) hosts the parked procedural planet at
//   the orbit's right-side intersection point (cx≈50, cy≈16).
//
//   FR-9.3: the parked planet overlaps the top-right HH:MM corner,
//   so this scene OPTS OUT of the shared chrome (wants_clock_chrome
//   = false). Same trade-off the exoplanet_count + giant clock
//   scenes already make.
//
// ── Orbital-camera animation ──────────────────────────────────────
//
// The panel is treated as a near-edge-on window onto a true elliptical
// orbit whose centre sits inside the panel (cx0=20, cy0=16). The
// orbit itself is NEVER drawn — only the planet body — but its path
// defines every position the planet can occupy during entry,
// parking, and exit. From the viewer's perspective it reads as a
// camera locked just above the orbit plane, watching bodies do a
// front-pass (close, BOTTOM of the panel, LARGE) and a back-pass
// (far, TOP of the panel, SMALL).
//
//   Angle convention: uint8_t units, 0..255 = one full turn, standard
//   math direction (CCW from east). Math via fp::sin_q8/cos_q8 so
//   the per-frame trig stays integer-only (NFR-1.3).
//     θ =   0 → east  → park, mid-screen-right
//     θ =  64 → north → far  (back of orbit) → TOP of panel, smallest
//     θ = 128 → west  → mid-height, offscreen left
//     θ = 192 → south → near (front of orbit) → BOTTOM of panel, largest
//   Projection (Rx=30, Ry=14):
//     screen_x = cx + (Rx * cos_q8(θ)) >> 8
//     screen_y = cy - (Ry * sin_q8(θ)) >> 8       (y flipped)
//   Body radius modulated by (1 - sin) / 2 so the planet swells on
//   the front pass and shrinks on the back pass — depth cue without
//   needing a real 3D pipeline.
//
// Per-activation lifecycle (single PlanetsScene instance, re-used
// across activations via the scene_registry file-scope singleton):
//
//   1. EXIT   (only when this is NOT the first activation since
//              boot — i.e. there IS a previous body to wave off).
//              The PREVIOUS body sweeps CCW from the park position
//              (θ = 0) up over the top of the orbit to off-frame
//              past the LEFT edge of the back-pass (θ = 96, ≈
//              top-left, smallest size). Duration kExitMs = 1200 ms.
//
//   2. ENTER  The NEW body sweeps CCW from off-frame past the LEFT
//              edge of the front-pass (θ = 149, ≈ bottom-left,
//              already near full size) DOWN across the bottom
//              (passing through θ = 192 where it is LARGEST), up
//              the right side, and into the park position (θ = 0).
//              Duration kEnterMs = 800 ms. Both endpoints of the
//              animation are offscreen-LEFT (one at the top for the
//              outgoing body, one at the bottom for the incoming
//              one) per the user's edge-on-ellipse spec.
//
//   3. SETTLED Planet sits at park; typewriter kernel types out
//              the distance + fact lines and the planet spins
//              slowly in place (longitude phase via the renderer's
//              existing time_phase byte).
//
// The procedural renderer (`planet::ProceduralPlanet`) keeps ONE
// instance; we re-seed it once at the EXIT→ENTER phase boundary
// so the EXIT phase paints the outgoing body's seed and ENTER
// paints the incoming body's seed. Seed cost is one FNV-1a +
// palette pass — cheap enough to do on a phase transition.
//
// ── Advance-on-entry ──────────────────────────────────────────────
//
//   init() advances prefs::set_planet_index() by 1 (mod kBodyCount)
//   each time the scene activates AFTER the first. The first init()
//   after boot honours the persisted index unchanged (so the device
//   resumes on the same body the operator last saw), and skips the
//   EXIT phase because there is nothing to wave off.
//
// ── Live overlay (observatory/planet) ─────────────────────────────
//
//   When a fresh planet_state snapshot exists AND its `name` field
//   matches the active body (case-insensitive), the fact line (line
//   3) is replaced with the look-angles in the same vocabulary the
//   old jupiter scene used:
//     above horizon + dark    → "VIS BBBxEE"
//     above horizon + day     → "IN <IAU>"
//     below horizon           → "BELOW"
//   This keeps the scene useful for the small set of bodies HA can
//   actually ephemeris, without forcing every catalog row to have
//   a live data path.
//
// (added when jupiter_visibility was generalised — see plan in
//  /memories/session/plan.md. Entry/exit reworked to the orbital-
//  camera model after the initial multi-stage entry read as cartoon-y.)

#pragma once

#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <Adafruit_Protomatter.h>

#include "astro/fixed_point.h"
#include "config.h"
#include "gfx_text.h"
#include "planet_catalog.h"
#include "planet_renderer.h"
#include "planet_state.h"
#include "prefs.h"
#include "scene.h"
#include "stars.h"
#include "sun_position.h"
#include "theme.h"
#include "time_of_day.h"
#include "typewriter.h"

class PlanetsScene : public Scene {
 public:
  const char* name() const override { return "planets"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);

    // Capture the body we were just parked on (if any) — it becomes
    // the OUTGOING body during the EXIT phase below. m_has_outgoing
    // is false only on the very first activation since boot, when
    // there's nothing to wave off.
    uint8_t persisted = prefs::current().planet_index;
    if (persisted >= planet_catalog::kBodyCount) persisted = 0;
    if (m_had_first_init) {
      m_outgoing_idx = m_active_idx;
      m_has_outgoing = true;
      m_active_idx   = static_cast<uint8_t>(
                           (persisted + 1u) % planet_catalog::kBodyCount);
      prefs::set_planet_index(m_active_idx);
    } else {
      m_outgoing_idx   = 0;
      m_has_outgoing   = false;
      m_active_idx     = persisted;
      m_had_first_init = true;
    }

    // Seed the renderer for whichever body paints FIRST. With an
    // outgoing body we paint EXIT first → seed = outgoing. Without,
    // we go straight to ENTER → seed = incoming. The phase-boundary
    // re-seed in render() picks up the swap when EXIT finishes.
    seed_for(m_has_outgoing ? m_outgoing_idx : m_active_idx);
    m_phase_seed_is_incoming = !m_has_outgoing;

    m_have_enter = false;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_have_enter) {
      m_enter_ms   = now_ms;
      m_have_enter = true;
    }
    const uint32_t t = now_ms - m_enter_ms;  // wrap-safe per CODING_PRACTICES §2

    matrix.fillScreen(0x0000);

    // ── Phase resolution ─────────────────────────────────────────
    // Timeline:
    //   [0,           exit_ms)             → EXIT  (outgoing)
    //   [exit_ms,     settle_at)           → ENTER (incoming)
    //   [settle_at,   ∞)                   → SETTLED
    // With m_has_outgoing == false we collapse exit_ms to 0 so
    // ENTER starts at t=0.
    const uint32_t exit_ms   = m_has_outgoing ? kExitMs : 0;
    const uint32_t enter_at  = exit_ms;
    const uint32_t settle_at = exit_ms + kEnterMs;

    enum class Phase : uint8_t { EXIT, ENTER, SETTLED };
    Phase phase;
    if      (t < exit_ms)   phase = Phase::EXIT;
    else if (t < settle_at) phase = Phase::ENTER;
    else                    phase = Phase::SETTLED;

    // Lazy re-seed at the EXIT→ENTER boundary: pay the seed cost
    // once when the outgoing body has finished sweeping out, not
    // every frame.
    if (phase != Phase::EXIT && !m_phase_seed_is_incoming) {
      seed_for(m_active_idx);
      m_phase_seed_is_incoming = true;
    }

    // ── Orbital position ─────────────────────────────────────────
    // Linear interpolation in ANGLE (not in screen-space) — what
    // gives the motion its "following the orbit" feel. Both ENTER
    // and EXIT travel CCW (angle INCREASING), modulo wrap, so the
    // outgoing body continues past park along the back-pass while
    // the incoming body climbs the front-pass up into park.
    //
    //   EXIT  : kAngleParked     (0)   → kAngleExitEnd  (96)
    //   ENTER : kAngleEnterStart (149) → kAngleParked + 256 (256)
    //           — extending past 255 then casting to uint8_t wraps
    //             cleanly back to 0 at the endpoint.
    uint8_t angle;
    if (phase == Phase::EXIT) {
      const uint32_t span = exit_ms > 0 ? exit_ms : 1;
      const int32_t  da   = int32_t(kAngleExitEnd) - int32_t(kAngleParked);
      angle = static_cast<uint8_t>(
                int32_t(kAngleParked) + (da * int32_t(t)) / int32_t(span));
    } else if (phase == Phase::ENTER) {
      const uint32_t lp = t - enter_at;
      const int32_t  da = 256 - int32_t(kAngleEnterStart);  // CCW to park
      angle = static_cast<uint8_t>(
                int32_t(kAngleEnterStart) + (da * int32_t(lp)) / int32_t(kEnterMs));
    } else {
      angle = kAngleParked;
    }

    // Project angle → screen-space via the shared Q8.8 sin/cos LUT.
    // mul: Q8.8 amplitude (sin/cos in [-256,+256]) × integer R →
    // shift back by 8 to land in integer pixels. Off-frame positions
    // are passed to the renderer as-is — render_small / render_large
    // clip cleanly past the panel edges. screen_y SUBTRACTS the sin
    // term so north (sin>0) ends up at the TOP of the panel.
    const int16_t cx = static_cast<int16_t>(
        kOrbitCx + ((fp::cos_q8(angle) * kOrbitRx) >> 8));
    const int16_t cy = static_cast<int16_t>(
        kOrbitCy - ((fp::sin_q8(angle) * kOrbitRy) >> 8));

    // Body radius: full size at park, gently tapered away from
    // park to sell "approaching" (ENTER) and "receding" (EXIT).
    const uint8_t r = body_radius_for(angle);

    // >> 6 matches the planet_zoo_demo cadence (2× the previous
    // >> 7 rate) so the surface rotation reads at the same speed
    // the operator saw in the zoo preview.
    const uint8_t time_phase =
        static_cast<uint8_t>((now_ms >> 6) & 0xFFu);
    m_planet.render(matrix, cx, cy, r, time_phase);

    // ── Settled-only chrome (banner + typewriter) ────────────────
    if (phase != Phase::SETTLED) return;

    const planet_catalog::Body& body =
        planet_catalog::kBodies[m_active_idx];

    paint_line1_banner(matrix, body, /*x=*/1, accent_for(body.type));

    char line2[16];
    char line3[16];
    format_distance(body, line2, sizeof(line2));
    format_fact_or_overlay(body, now_ms, line3, sizeof(line3));

    const uint8_t lens[3] = {
      0,
      static_cast<uint8_t>(strlen(line2)),
      static_cast<uint8_t>(strlen(line3)),
    };
    const uint32_t tw_ms = t - settle_at;
    const typewriter::Schedule tw = typewriter::compute(tw_ms, lens);

    constexpr uint8_t kBaselineY[3] = { 8, 17, 26 };
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);

    const char* lines[3] = { "", line2, line3 };
    const uint16_t inks[3] = {
      0x0000,                                  // unused (banner slot)
      theme::ink(theme::Ink::STATUS_INFO),     // distance — cool info
      theme::ink(theme::Ink::STATUS_WARN),     // fact / overlay — warm
    };

    for (int i = 1; i < 3; ++i) {
      const uint8_t n = tw.typed[i];
      if (n == 0 && tw.active != i) continue;

      char prefix[16];
      memcpy(prefix, lines[i], n);
      prefix[n] = '\0';
      const int16_t by = static_cast<int16_t>(kBaselineY[i]);

      int16_t  bx, by_u; uint16_t bw, bh;
      uint16_t prefix_px = 0;
      if (n > 0) {
        matrix.getTextBounds(prefix, 1, by, &bx, &by_u, &bw, &bh);
        prefix_px = bw;
      }

      // Black backdrop strip behind typed glyphs only — the planet
      // stays visible behind un-typed portions of each row.
      const int16_t bg_y = static_cast<int16_t>(by - 6);
      int16_t bg_w_total =
          (n > 0) ? static_cast<int16_t>(prefix_px + 2) : 0;
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

  // Planet parks across the top-right HH:MM region and sweeps
  // through it during ENTER / EXIT, so we OPT OUT of the shared
  // chrome (FR-9.3). Same trade-off the exoplanet_count + giant
  // clock scenes already make.
  bool wants_clock_chrome() const override { return false; }

 private:
  // ── Animation timing ─────────────────────────────────────────────
  // Exit is the longer sweep (planet crosses most of the panel);
  // enter is shorter (planet travels from just past the right edge
  // to park only). Tuned by eye to read as "deliberate" rather than
  // "hurried" without dragging out the rotation cycle.
  static constexpr uint32_t kExitMs  = 1200;
  static constexpr uint32_t kEnterMs =  800;

  // ── Orbit geometry ───────────────────────────────────────────────
  // Edge-on elliptical orbit. Centre offset LEFT of panel centre so
  // the eastern park spot (θ=0) lands at (cx+Rx, cy) = (50, 16) —
  // same muscle-memory parked position the previous designs used
  // (text left, planet right). Ry is roughly Rx/2 so the orbit
  // reads as significantly tilted (near edge-on) without being
  // perfectly flat.
  //
  // Standard math angle convention (CCW from east, uint8_t units
  // 0..255 = full turn).
  // Rx widened 1.5× (30 → 45) so the front-pass sweep crosses more
  // of the panel before parking; cx pulled left to keep the eastern
  // park spot at ≈ (50, 16) (cx + Rx → 5 + 45).
  static constexpr int16_t kOrbitCx =  5;
  static constexpr int16_t kOrbitCy = 16;
  static constexpr int16_t kOrbitRx = 45;
  static constexpr int16_t kOrbitRy = 14;

  // ENTER starts offscreen-LEFT on the FRONT (near) side of the
  // orbit and sweeps CCW through south (bottom, largest) up the
  // east side into the park spot.
  //   θ = 149  ≈ 210°  → screen ≈ (-6, 23)   far left, below mid
  //   θ =   0           → screen  = (50, 16)  PARK
  // EXIT continues CCW from park up the east side, across the back
  // (top of panel, smallest), and offscreen-LEFT on the back side.
  //   θ =  96  ≈ 135°  → screen ≈ (-1,  6)   far left, above mid
  static constexpr uint8_t kAngleEnterStart = 149;
  static constexpr uint8_t kAngleParked     =   0;
  static constexpr uint8_t kAngleExitEnd    =  96;

  // At the front-pass apex (θ=192, sin=-256) the planet is largest;
  // at the back-pass apex (θ=64, sin=+256) it is smallest; at the
  // east/west crossings (sin=0) it is mid-sized. The park spot is
  // a sin=0 crossing so kParkedRadius effectively == the mid size.
  static constexpr uint8_t kMaxRadius    = 19;  // front pass, big and bossy
  static constexpr uint8_t kParkedRadius = 15;  // east crossing, settled
  static constexpr uint8_t kMinRadius    =  8;  // back pass, distant

  // Body radius driven by depth proxy sin(θ): south (sin=-256) is
  // closest to the camera (largest), north (sin=+256) is furthest
  // (smallest), east/west crossings (sin=0) are at the parked /
  // mid size. Piecewise-linear between kMaxRadius and kParkedRadius
  // on the near half, kParkedRadius and kMinRadius on the far half,
  // so the parked east crossing exactly equals kParkedRadius.
  static uint8_t body_radius_for(uint8_t angle) {
    const int16_t s = fp::sin_q8(angle);  // [-256, +256]
    if (s <= 0) {
      // Near half: s in [-256, 0]. depth in [0, 256], 0 = parked,
      // 256 = front apex / largest.
      const int depth = -int(s);
      const int delta = int(kMaxRadius) - int(kParkedRadius);
      return static_cast<uint8_t>(
          int(kParkedRadius) + (delta * depth) / 256);
    } else {
      // Far half: s in [0, +256]. recede in [0, 256], 256 = back
      // apex / smallest.
      const int recede = int(s);
      const int delta  = int(kParkedRadius) - int(kMinRadius);
      return static_cast<uint8_t>(
          int(kParkedRadius) - (delta * recede) / 256);
    }
  }

  // Map BodyType → a fixed theme ink slot for the type-tag accent.
  // Planets warm, moons cool, dwarves dim, the Sun WARN (its real
  // visual signature), exoplanets OK-green (the "discovery" hue).
  static uint16_t accent_for(planet_catalog::BodyType t) {
    using BT = planet_catalog::BodyType;
    switch (t) {
      case BT::STAR:      return theme::ink(theme::Ink::STATUS_WARN);
      case BT::PLANET:    return theme::ink(theme::Ink::STATUS_WARN);
      case BT::DWARF:     return theme::ink(theme::Ink::STATUS_WARN_DIM);
      case BT::MOON:      return theme::ink(theme::Ink::STATUS_INFO);
      case BT::EXOPLANET: return theme::ink(theme::Ink::STATUS_OK);
    }
    return theme::ink(theme::Ink::STATUS_INFO);
  }

  // Paint line 1: display name + 3-letter type tag, with a static
  // underline rule. No slide-in — the entry beat belongs to the
  // planet sweeping along its orbit; the text just appears when
  // SETTLED begins.
  static void paint_line1_banner(Adafruit_Protomatter& matrix,
                                 const planet_catalog::Body& body,
                                 int16_t x, uint16_t accent_ink) {
    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);
    constexpr int16_t kBy = 8;
    // Measure name + type so we can paint a tight black backdrop
    // (the planet is slowly spinning behind the banner during
    // SETTLED and we don't want pixel bleed-through).
    int16_t  bx, by_u; uint16_t bw, bh;
    matrix.getTextBounds(body.display_name, x, kBy, &bx, &by_u, &bw, &bh);
    int16_t bg_w = static_cast<int16_t>(bw + 2);
    int16_t  bx2, by_u2; uint16_t bw2, bh2;
    matrix.getTextBounds(body.short_type, 0, kBy, &bx2, &by_u2, &bw2, &bh2);
    const int16_t tag_x = static_cast<int16_t>(x + bw + 2);
    const bool tag_fits = (tag_x + bw2 + 1) < 32;
    if (tag_fits) bg_w = static_cast<int16_t>(bg_w + bw2 + 2);
    matrix.fillRect(0, kBy - 6 + 2, bg_w, 5, 0x0000);

    matrix.setTextColor(theme::ink(theme::Ink::BODY));
    matrix.setCursor(x, kBy);
    matrix.print(body.display_name);
    if (tag_fits) {
      matrix.setTextColor(accent_ink);
      matrix.setCursor(tag_x, kBy);
      matrix.print(body.short_type);
    }
    matrix.drawFastHLine(1, kBy + 2,
                         static_cast<int16_t>(bg_w - 2), accent_ink);
  }

  // Build the distance string. Single decimal for AU + LY, no
  // decimal for K KM. Branchy but cheap — runs once per render.
  static void format_distance(const planet_catalog::Body& body,
                              char* out, size_t cap) {
    using DU = planet_catalog::DistanceUnit;
    const uint16_t d10 = body.distance_x10;
    switch (body.distance_unit) {
      case DU::AU:
        if (body.type == planet_catalog::BodyType::STAR) {
          snprintf(out, cap, "AT CORE");
        } else {
          snprintf(out, cap, "%u.%u AU",
                   static_cast<unsigned>(d10 / 10u),
                   static_cast<unsigned>(d10 % 10u));
        }
        break;
      case DU::KKM:
        snprintf(out, cap, "%u K KM",
                 static_cast<unsigned>(d10));
        break;
      case DU::LY:
        snprintf(out, cap, "%u.%u LY",
                 static_cast<unsigned>(d10 / 10u),
                 static_cast<unsigned>(d10 % 10u));
        break;
    }
  }

  // Line 3: catalog fact OR — if a fresh planet_state snapshot
  // exists for the active body — the live look-angle overlay. Same
  // VIS/IN/BELOW vocabulary the old jupiter scene used so muscle
  // memory carries.
  void format_fact_or_overlay(const planet_catalog::Body& body,
                              uint32_t now_ms,
                              char* out, size_t cap) const {
    planet_state::Snapshot snap;
    if (planet_state::get(now_ms, &snap) && snap.valid &&
        name_matches_ci(snap.name, body.render_name)) {
      bool dark = false;
      const tod::Reading r = tod::now(now_ms);
      if (r.valid) {
        const int32_t utc_epoch = r.local_epoch
            - static_cast<int32_t>(r.tz_offset_min) * 60;
        const sun::Position sp =
            sun::compute(utc_epoch, LATITUDE_DEG, LONGITUDE_DEG);
        if (sp.altitude_deg <= -6.0f) dark = true;
      }
      if (snap.elevation_deg < 0) {
        snprintf(out, cap, "BELOW");
      } else if (!dark) {
        const char* iau =
            constellations_iau::kCatalog[snap.constellation_index].iau;
        char up[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 3 && iau[i] != '\0'; ++i) {
          char c = iau[i];
          if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
          up[i] = c;
        }
        snprintf(out, cap, "IN %s", up);
      } else {
        int b = snap.bearing_deg;   if (b < 0) b = 0; if (b > 359) b = 359;
        int e = snap.elevation_deg; if (e < 0) e = 0; if (e >  90) e =  90;
        snprintf(out, cap, "VIS %03dx%d", b, e);
      }
    } else {
      snprintf(out, cap, "%s", body.fact);
    }
  }

  static bool name_matches_ci(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
      char ca = *a++;
      char cb = *b++;
      if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
      if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
      if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
  }

  // Re-seed the procedural renderer for the body at catalog index
  // `idx`. Guarded so a repeat call with the same body is a no-op
  // (avoids the FNV-1a + palette pass on every render frame during
  // EXIT where the angle changes but the body doesn't).
  void seed_for(uint8_t idx) {
    const char* nm = planet_catalog::kBodies[idx].render_name;
    if (m_seeded_name[0] != '\0' &&
        strncmp(m_seeded_name, nm, sizeof(m_seeded_name)) == 0) {
      return;
    }
    m_planet.seed(nm);
    const size_t cap = sizeof(m_seeded_name);
    size_t i = 0;
    while (i + 1 < cap && nm[i] != '\0') { m_seeded_name[i] = nm[i]; ++i; }
    m_seeded_name[i] = '\0';
  }

  // Per-instance state — persists across activations because the
  // scene lives in a file-scope singleton in scene_registry.cpp.
  uint32_t                 m_enter_ms        = 0;
  bool                     m_have_enter      = false;
  bool                     m_had_first_init  = false;
  bool                     m_has_outgoing    = false;
  // Tracks whether the renderer's seed currently matches the
  // INCOMING body (true) or the OUTGOING body (false). Set in
  // init(), flipped at the EXIT→ENTER phase boundary.
  bool                     m_phase_seed_is_incoming = false;
  uint8_t                  m_active_idx      = 0;
  uint8_t                  m_outgoing_idx    = 0;
  planet::ProceduralPlanet m_planet;
  char                     m_seeded_name[16] = {0};
};
