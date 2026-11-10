#include "scene_registry.h"

#include "scenes/scene.h"
#include "scenes/background_scene.h"
#include "scenes/boot_scene.h"
#include "scenes/clock_scene.h"
#include "scenes/color_cycle_scene.h"
#include "scenes/constellation_now_scene.h"
#include "scenes/exoplanet_count_scene.h"
#include "scenes/font_demo_scene.h"
#include "scenes/gfx_test_scene.h"
#include "scenes/giant_clock_scene.h"
#include "scenes/ir_test_scene.h"
#include "scenes/iss_pass_scene.h"
#include "scenes/launch_countdown_scene.h"
#include "scenes/moon_phase_scene.h"
#include "scenes/night_scene.h"
#include "scenes/offline_scene.h"
#include "scenes/planet_zoo_demo_scene.h"
#include "scenes/planets_scene.h"
#include "scenes/spectrum_demo_scene.h"
#include "scenes/splash_scene.h"
#include "scenes/text_demo_scene.h"
#include "scenes/thermal_safe_scene.h"
#include "scenes/alert_scene.h"
#include "scenes/artemis_scene.h"
#include "scenes/aurora_scene.h"
#include "scenes/black_holes_scene.h"
#include "scenes/deep_sky_scene.h"
#include "scenes/hail_mary_scene.h"
#include "scenes/light_travel_scene.h"
#include "scenes/relativity_scene.h"
#include "scenes/sky_now_scene.h"
#include "scenes/weather_scene.h"

// Static scene instances — never heap-allocated (NFR-2.2). Add new
// scenes here as plain file-scope objects, then add a SceneId case
// in scene_for() below. Instances marked `[[maybe_unused]]` are not
// reached from any SceneId case yet but are kept linker-alive for
// the next phase that wires them in.
namespace {

[[maybe_unused]] BootScene             s_boot_scene;        // phase 1.5 demo
[[maybe_unused]] ClockScene            s_clock_scene;       // phase 1.4 demo
[[maybe_unused]] ColorCycleScene       s_color_cycle_scene; // smoke-test fallback

// One BackgroundScene per BgType — each is a thin wrapper that
// dispatches to g_backgrounds. Adding a fourth bg type means
// implementing *Bg.h, adding the enum value + dispatch case in
// backgrounds.h, then declaring another instance here.
BackgroundScene s_bg_starfield(BgType::STARFIELD);
BackgroundScene s_bg_parallax (BgType::PARALLAX);
BackgroundScene s_bg_nebula   (BgType::NEBULA);
BackgroundScene s_bg_bitmap   (BgType::BITMAP);
BackgroundScene s_bg_image    (BgType::IMAGE);

[[maybe_unused]] TextDemoScene s_text_demo_scene;
GiantClockScene  s_giant_clock_scene;          // phase 3.5.3 — default room-clock view
NightScene       s_night_scene;                // phase 5.5.1 — LDR-triggered override
OfflineScene     s_offline_scene;              // phase 6.4 — MQTT-disconnect override
SplashScene      s_splash_scene;               // phase 6.5+ — boot splash override
ThermalSafeScene s_thermal_safe_scene;         // phase 5.5.2 — DS3231-triggered override
GfxTestScene     s_gfx_test_scene;             // graphics smoke-test (FPS, palette cycle)
IssPassScene     s_iss_pass_scene;             // phase 7.1 — "ISS NOW" callout
MoonPhaseScene   s_moon_phase_scene;           // phase 7.2 — sticky moon disc + phase
// JupiterVisibilityScene retired — generalised into PlanetsScene below.
ConstellationNowScene  s_constellation_now_scene;  // phase 7.4 — dynamic constellation art
IrTestScene            s_ir_test_scene;            // phase IR.1 — IR receiver POC readout
FontDemoScene          s_font_demo_scene;          // diagnostic: cycle Adafruit_GFX builtin fonts
LaunchCountdownScene   s_launch_countdown_scene;   // phase L     — next-scheduled rocket launch T-minus
SpectrumDemoScene      s_spectrum_demo_scene;      // phase 7.6   — visible-spectrum bar + sweeping absorption-line marker (offline)
ExoplanetCountScene    s_exoplanet_count_scene;    // phase 7.7   — animated procedural planet + NASA Exoplanet Archive stats
PlanetZooDemoScene     s_planet_zoo_demo_scene;    // phase 7.7b  — diagnostic: walk every planet_renderer preset across LOD tiers
PlanetsScene           s_planets_scene;            // generic procedural-planet scene (replaces JUPITER_VISIBILITY)
WeatherScene           s_weather_scene;            // FR-21.1 weather + what to wear / stargazing verdict
SkyNowScene            s_sky_now_scene;            // FR-21.2 bright objects up now
AlertScene             s_alert_scene;              // FR-21.3 go-look-now callout
LightTravelScene       s_light_travel_scene;       // FR-21.4 light-travel time
RelativityScene        s_relativity_scene;         // FR-21.4 time dilation
BlackHolesScene        s_black_holes_scene;        // FR-21.6 gravitational waves
DeepSkyScene           s_deep_sky_scene;           // FR-21.8 pixel-art nebulae/galaxies
AuroraScene            s_aurora_scene;             // FR-21.5 Kp + aurora curtain
ArtemisScene           s_artemis_scene;            // FR-21.7 Artemis tracker
HailMaryScene          s_hail_mary_scene;          // FR-21.8 Project Hail Mary easter eggs

}  // namespace

namespace scene_registry {

Scene* scene_for(scene_state::SceneId id) {
  using SI = scene_state::SceneId;
  switch (id) {
    case SI::BOOT:               return &s_boot_scene;
    case SI::CLOCK:              return &s_giant_clock_scene;
    case SI::COLOR_CYCLE:        return &s_color_cycle_scene;
    case SI::TEXT_DEMO:          return &s_text_demo_scene;
    case SI::BG_STARFIELD:       return &s_bg_starfield;
    case SI::BG_PARALLAX:        return &s_bg_parallax;
    case SI::BG_NEBULA:          return &s_bg_nebula;
    case SI::BG_BITMAP:          return &s_bg_bitmap;
    case SI::BG_IMAGE:           return &s_bg_image;
    case SI::NIGHT:              return &s_night_scene;
    case SI::OFFLINE:            return &s_offline_scene;
    case SI::SPLASH:             return &s_splash_scene;
    case SI::THERMAL_SAFE:       return &s_thermal_safe_scene;
    case SI::GFX_TEST:           return &s_gfx_test_scene;
    case SI::ISS_PASS:           return &s_iss_pass_scene;
    case SI::MOON_PHASE:         return &s_moon_phase_scene;
    case SI::CONSTELLATION_NOW:  return &s_constellation_now_scene;
    case SI::IR_TEST:            return &s_ir_test_scene;
    case SI::LAUNCH_COUNTDOWN:   return &s_launch_countdown_scene;
    case SI::SPECTRUM_DEMO:      return &s_spectrum_demo_scene;
    case SI::EXOPLANET_COUNT:    return &s_exoplanet_count_scene;
    case SI::PLANET_ZOO_DEMO:    return &s_planet_zoo_demo_scene;
    case SI::PLANETS:            return &s_planets_scene;
    case SI::FONT_DEMO:          return &s_font_demo_scene;
    case SI::WEATHER:            return &s_weather_scene;
    case SI::SKY_NOW:            return &s_sky_now_scene;
    case SI::ALERT:              return &s_alert_scene;
    case SI::LIGHT_TRAVEL:       return &s_light_travel_scene;
    case SI::RELATIVITY:         return &s_relativity_scene;
    case SI::BLACK_HOLES:        return &s_black_holes_scene;
    case SI::DEEP_SKY:           return &s_deep_sky_scene;
    case SI::AURORA:             return &s_aurora_scene;
    case SI::ARTEMIS:            return &s_artemis_scene;
    case SI::HAIL_MARY:          return &s_hail_mary_scene;
  }
  return nullptr;
}

Scene* default_scene() { return &s_giant_clock_scene; }

FontDemoScene* font_demo() { return &s_font_demo_scene; }

}  // namespace scene_registry
