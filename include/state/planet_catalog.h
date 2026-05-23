// Static facts table for the `planets` scene.
//
// One row per body the scene can show, in lockstep with the
// procedural-renderer presets in include/render/planet_renderer.h
// (kSolarPresets[]). `render_name` is the exact string we hand to
// `ProceduralPlanet::seed()` so the renderer picks the hand-tuned
// preset rather than falling through to the hash-derived archetype.
//
// All strings live in flash. The struct is POD; the array is
// `constexpr` so the linker emits the whole catalog into .rodata
// with zero RAM cost.
//
// Distance encoding — we want a generic readout that fits both
// "5.2 AU" (planets) and "384 K KM" (moon) and "634 LY" (kepler-22b)
// without per-body branching at render time. Encoded as a
// `(value_x10, unit)` pair: value is fixed-point ×10 to preserve
// a single decimal, unit is a discriminator. The scene formats one
// of three short tokens based on the unit; see PlanetsScene::
// format_distance().
//
// Fact strings are deliberately short (≤ 10 chars after upper-case
// folding) so they fit on the Picopixel body row at baseline y=30
// alongside the 2-px cursor.
//
// (added with the `planets` scene that replaced `jupiter_visibility`)

#pragma once

#include <stdint.h>

namespace planet_catalog {

enum class BodyType : uint8_t {
  STAR      = 0,
  PLANET    = 1,
  DWARF     = 2,
  MOON      = 3,
  EXOPLANET = 4,
};

enum class DistanceUnit : uint8_t {
  AU         = 0,  // astronomical units, x10
  KKM        = 1,  // thousand km (used for the Moon's 384 000 km)
  LY         = 2,  // light years, x10 (Kepler-22b sits at ~634 LY)
};

struct Body {
  const char*  render_name;     // exact key for ProceduralPlanet::seed()
  const char*  display_name;    // upper-cased label, ≤ 8 chars
  const char*  short_type;      // 3-letter type tag for line 2 (STAR/PLA/DWF/MOO/EXO)
  BodyType     type;
  uint16_t     distance_x10;    // see DistanceUnit
  DistanceUnit distance_unit;
  const char*  fact;            // ≤ 10 chars after upper-case folding
};

// Order is deliberate — the IR ▲/▼ scene cycle re-enters the scene
// to step through this list. Inner planets first, outer planets,
// dwarf, sun + moon (the bodies a kid recognises from a backyard),
// Jovian moons, Titan, and finally the headline exoplanet so the
// "this one is REALLY far" entry punctuates the loop.
inline constexpr Body kBodies[] = {
  // ── Inner rocky planets ──────────────────────────────────────────
  { "mercury",    "MERCURY",  "PLA", BodyType::PLANET,    4,  DistanceUnit::AU,  "DAY=176D" },
  { "venus",      "VENUS",    "PLA", BodyType::PLANET,    7,  DistanceUnit::AU,  "ROT BKWD" },
  { "earth",      "EARTH",    "PLA", BodyType::PLANET,   10,  DistanceUnit::AU,  "HOME"     },
  { "mars",       "MARS",     "PLA", BodyType::PLANET,   15,  DistanceUnit::AU,  "RED SAND" },
  // ── Gas + ice giants ─────────────────────────────────────────────
  { "jupiter",    "JUPITER",  "GAS", BodyType::PLANET,   52,  DistanceUnit::AU,  "95 MOONS" },
  { "saturn",     "SATURN",   "GAS", BodyType::PLANET,   95,  DistanceUnit::AU,  "RINGS"    },
  { "uranus",     "URANUS",   "ICE", BodyType::PLANET,  192,  DistanceUnit::AU,  "TIPPED"   },
  { "neptune",    "NEPTUNE",  "ICE", BodyType::PLANET,  301,  DistanceUnit::AU,  "WINDIEST" },
  // ── Dwarf ────────────────────────────────────────────────────────
  { "pluto",      "PLUTO",    "DWF", BodyType::DWARF,   394,  DistanceUnit::AU,  "5 MOONS"  },
  // ── Star + Moon ──────────────────────────────────────────────────
  { "sun",        "SUN",      "STR", BodyType::STAR,     10,  DistanceUnit::AU,  "G2V STAR" },
  { "moon",       "MOON",     "MOO", BodyType::MOON,   3844,  DistanceUnit::KKM, "OUR MOON" },
  // ── Jovian moons ─────────────────────────────────────────────────
  { "io",         "IO",       "MOO", BodyType::MOON,     52,  DistanceUnit::AU,  "VOLCANIC" },
  { "europa",     "EUROPA",   "MOO", BodyType::MOON,     52,  DistanceUnit::AU,  "ICE OCEAN"},
  { "ganymede",   "GANYMEDE", "MOO", BodyType::MOON,     52,  DistanceUnit::AU,  "BIGGEST"  },
  { "callisto",   "CALLISTO", "MOO", BodyType::MOON,     52,  DistanceUnit::AU,  "CRATERED" },
  // ── Saturnian moon ───────────────────────────────────────────────
  { "titan",      "TITAN",    "MOO", BodyType::MOON,     95,  DistanceUnit::AU,  "LIQUID N2"},
  // ── Headline exoplanet ───────────────────────────────────────────
  { "kepler-22b", "KEP-22B",  "EXO", BodyType::EXOPLANET, 6340, DistanceUnit::LY, "OCEAN?"   },
};

inline constexpr uint8_t kBodyCount =
    sizeof(kBodies) / sizeof(kBodies[0]);

}  // namespace planet_catalog
