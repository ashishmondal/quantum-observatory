// Pure background renderers + dispatcher.
//
// A "background" is a render-to-buffer routine that owns the entire
// frame's pixels (writes every pixel; no compositing assumption) but does
// NOT call matrix.show() — the caller swaps buffers, allowing foreground
// layers (text, transitions) to draw on top first.
//
// One global Backgrounds instance holds state for every type so multiple
// scenes can share the same animated bg without duplicating ~520 B of
// star/cell arrays per scene. Init once in setup() with init_all().
//
// Adding a new background type: implement a *Bg class with the same shape
// (init, render-without-show), add an enum value, add a case in the
// dispatcher.
//
// (added in phase 2.5)

#pragma once

#include <stdint.h>

#include "backgrounds/starfield_bg.h"
#include "backgrounds/parallax_bg.h"
#include "backgrounds/nebula_bg.h"

class Adafruit_Protomatter;

enum class BgType : uint8_t {
  NONE = 0,
  STARFIELD,
  PARALLAX,
  NEBULA,
};

class Backgrounds {
public:
  // Call once during setup() — seeds every backend's deterministic state.
  void init_all(Adafruit_Protomatter& matrix) {
    (void)matrix;
    m_star.init();
    m_para.init();
    m_neb.init();
  }

  // Per-frame draw of the chosen background. Caller is responsible for
  // matrix.show() afterwards.
  void render(BgType type, Adafruit_Protomatter& matrix, uint32_t now_ms) {
    switch (type) {
      case BgType::STARFIELD: m_star.render(matrix, now_ms); break;
      case BgType::PARALLAX:  m_para.render(matrix, now_ms); break;
      case BgType::NEBULA:    m_neb.render(matrix, now_ms);  break;
      case BgType::NONE:
      default:                matrix.fillScreen(0x0000);     break;
    }
  }

  // Stable lower-case identifier — handy for logs / future scene registry.
  static const char* name(BgType type) {
    switch (type) {
      case BgType::STARFIELD: return "starfield";
      case BgType::PARALLAX:  return "parallax";
      case BgType::NEBULA:    return "nebula";
      case BgType::NONE:      return "none";
    }
    return "unknown";
  }

private:
  StarfieldBg m_star;
  ParallaxBg  m_para;
  NebulaBg    m_neb;
};

// Single shared instance, defined in src/backgrounds.cpp.
extern Backgrounds g_backgrounds;
