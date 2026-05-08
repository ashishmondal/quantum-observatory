// Boot splash scene — shown from boot until MQTT first connects.
//
// Renders the artist-supplied observatory.bmp full-screen with no
// foreground overlay. Uses its own ImagePaletteBg instance pointed at
// kImageRegistry's "observatory" entry (looked up by name, so we
// don't depend on registry order) — keeps the shared g_backgrounds
// IMAGE renderer pinned to the starfield used by clock/offline
// scenes.
//
// Override priority: highest (above thermal_safe). Cleared exactly
// once on the first MQTT-connected edge — see main.cpp.
//
// (added in phase 6.5+ polish)

#pragma once

#include <cstring>

#include <Adafruit_Protomatter.h>

#include "backgrounds/image_palette_bg.h"
#include "bitmaps/_index.h"
#include "config.h"
#include "scene.h"

class SplashScene : public Scene {
public:
  const char* name() const override { return "splash"; }

  // Splash IS the screen — corner clock chrome would clutter the
  // logo (FR-9.3 opt-out).
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
    // Locate "observatory" in the generated registry and bind our
    // local renderer to it. Registry index isn't trustworthy because
    // bmp_to_header.py emits entries in directory order.
    for (int i = 0; i < kImageRegistryCount; ++i) {
      const ImageEntry& e = kImageRegistry[i];
      if (e.name != nullptr && std::strcmp(e.name, "observatory") == 0) {
        m_bg.set(e);
        return;
      }
    }
    // Asset missing — m_bg falls back to rendering black via the
    // nullptr-palette branch in ImagePaletteBg::render().
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    m_bg.render(matrix, now_ms);
  }

private:
  ImagePaletteBg m_bg;
};
