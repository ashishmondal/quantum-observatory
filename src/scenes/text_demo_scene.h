// Demo scene: 2-line layout (header + body) composed over the nebula
// background. Canonical example of the gfx::draw_header / draw_body
// primitives — scenes pass strings, the helpers own font/centering/Y
// placement and halo legibility (FR-3.3, FR-4.3).
//
// (added in phase 3.1; refactored to use 2-line primitives in phase 3.4)

#pragma once

#include <Adafruit_Protomatter.h>

#include "backgrounds.h"
#include "config.h"
#include "gfx_text.h"
#include "scene.h"

class TextDemoScene : public Scene {
public:
  const char* name() const override { return "text_demo"; }

  void init(Adafruit_Protomatter& matrix) override {
    matrix.setTextWrap(false);
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    g_backgrounds.render(BgType::NEBULA, matrix, now_ms);
    gfx::draw_header(matrix, "OBS");
    gfx::draw_body  (matrix, "OBSERVATORY");
    // matrix.show() is called by loop1() after chrome (phase 3.5.2).
  }
};
