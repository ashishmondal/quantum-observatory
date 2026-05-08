// A Scene whose only job is to render a chosen background. Picks the bg
// at construction (or via set()) and dispatches through the global
// Backgrounds instance. Future composite scenes (bg + text) will follow
// the same pattern: call g_backgrounds.render(...) first, then draw their
// foreground, then matrix.show().
//
// (added in phase 2.5)
//
// diagnostic — bypasses theme:: by design (FR-15.3 exemption,
// CODING_PRACTICES §4): this is the bg-only smoke-test scene used to
// eyeball each renderer in isolation. No theme-affected literals to
// refactor (no foreground inks/fonts), but the file is in the
// diagnostic family so the exemption is documented for completeness.

#pragma once

#include <Adafruit_Protomatter.h>

#include "backgrounds.h"
#include "scene.h"

class BackgroundScene : public Scene {
public:
  explicit BackgroundScene(BgType type) : m_type(type) {}

  const char* name() const override { return Backgrounds::name(m_type); }

  void init(Adafruit_Protomatter& /*matrix*/) override {
    // Per-bg state is initialised once by g_backgrounds.init_all() in
    // setup(); nothing scene-specific to do here.
  }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    g_backgrounds.render(m_type, matrix, now_ms);
    // matrix.show() is called by loop1() after chrome (phase 3.5.2).
  }

  void set(BgType type) { m_type = type; }
  BgType type() const   { return m_type; }

private:
  BgType m_type;
};
