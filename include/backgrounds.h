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
#include "backgrounds/bitmap_bg.h"
#include "backgrounds/image_palette_bg.h"
#include "bitmaps/_index.h"
#include "theme.h"

class Adafruit_Protomatter;

enum class BgType : uint8_t {
  NONE = 0,
  STARFIELD,
  PARALLAX,
  NEBULA,
  BITMAP,
  IMAGE,
  THEME_CLOCK,   // per-theme animated bg behind giant clock (FR-15)
};

class Backgrounds {
public:
  // Call once during setup() — seeds every backend's deterministic state.
  void init_all(Adafruit_Protomatter& matrix) {
    (void)matrix;
    m_star.init();
    m_para.init();
    m_neb.init();
    // Per-theme animated clock background (THEME_CLOCK) state lives
    // on each Theme subclass. theme::set()/cycle() re-init the new
    // theme's animator on every switch, but seed the boot-active
    // theme here so the very first render frame doesn't see uninit'd
    // member arrays.
    theme::current_theme().init_clock_bg();
    init_bitmap_demo();
    init_image_default();
  }

  // Per-frame draw of the chosen background. Caller is responsible for
  // matrix.show() afterwards.
  void render(BgType type, Adafruit_Protomatter& matrix, uint32_t now_ms) {
    switch (type) {
      case BgType::STARFIELD: m_star.render(matrix, now_ms); break;
      case BgType::PARALLAX:  m_para.render(matrix, now_ms); break;
      case BgType::NEBULA:    m_neb.render(matrix, now_ms);  break;
      case BgType::BITMAP:    m_bmp.render(matrix, now_ms);         break;
      case BgType::IMAGE:     m_img.render(matrix, now_ms);         break;
      case BgType::THEME_CLOCK: theme::current_theme().render_clock_bg(matrix, now_ms); break;
      case BgType::NONE:
      default:                matrix.fillScreen(0x0000);            break;
    }
  }

  // Stable lower-case identifier — handy for logs / future scene registry.
  static const char* name(BgType type) {
    switch (type) {
      case BgType::STARFIELD:   return "starfield";
      case BgType::PARALLAX:    return "parallax";
      case BgType::NEBULA:      return "nebula";
      case BgType::BITMAP:      return "bitmap";
      case BgType::IMAGE:       return "image";
      case BgType::THEME_CLOCK: return "theme_clock";
      case BgType::NONE:        return "none";
    }
    return "unknown";
  }

  // Expose the bitmap so a future scene/MQTT command can swap content
  // at runtime. For now it stays pointed at the built-in demo.
  BitmapBg&        bitmap() { return m_bmp; }
  ImagePaletteBg&  image()  { return m_img; }

private:
  StarfieldBg     m_star;
  ParallaxBg      m_para;
  NebulaBg        m_neb;
  BitmapBg        m_bmp;
  ImagePaletteBg  m_img;

  // Built-in demo bitmap. Splits the 192-entry BG ramp of NEBULA_CLOUDS
  // into two cycling sub-regions and lays out the panel as horizontal
  // bands so each region's motion is obvious:
  //   rows  0..15  -> indices in   0..95   (region 0, slow forward)
  //   rows 16..31  -> indices in  96..191  (region 1, fast backward)
  // Within each band, the index varies with x so you see a gradient
  // sweeping across the screen.
  void init_bitmap_demo() {
    static constexpr BitmapBg::Region kRegions[] = {
      { /*start=*/  0, /*length=*/ 96, /*speed=*/ +24 },
      { /*start=*/ 96, /*length=*/ 96, /*speed=*/ -48 },
    };
    m_bmp.init_generated(&demo_pixel_fn, palette::Id::NEBULA_CLOUDS,
                         kRegions,
                         sizeof(kRegions) / sizeof(kRegions[0]));
  }

  static uint8_t demo_pixel_fn(int x, int y) {
    // Two horizontal bands; each fills its half of the BG ramp.
    // The diagonal (x+y) skew gives the gradient a slight tilt so the
    // cycling motion reads as flowing rather than purely horizontal.
    if (y < PANEL_HEIGHT / 2) {
      return static_cast<uint8_t>(((x + (y >> 1)) * 96) /
                                  (PANEL_WIDTH + (PANEL_HEIGHT >> 2)));
    } else {
      return static_cast<uint8_t>(
          96 +
          (((x + ((PANEL_HEIGHT - 1 - y) >> 1)) * 96) /
           (PANEL_WIDTH + (PANEL_HEIGHT >> 2))));
    }
  }

  // Point ImagePaletteBg at the "starfield" entry by name, with a
  // fallback to the first registered image if starfield is missing.
  // Looking up by name (vs. trusting kImageRegistry[0]) keeps the
  // clock backgrounds stable when new BMPs are added to assets/ and
  // shuffle the registry's emitted order. Empty registry => leave the
  // renderer pointed at nullptr (renders black, by design).
  void init_image_default() {
    if (kImageRegistryCount <= 0) return;
    int chosen = 0;
    for (int i = 0; i < kImageRegistryCount; ++i) {
      const char* n = kImageRegistry[i].name;
      if (n != nullptr && n[0] == 's' &&
          n[1] == 't' && n[2] == 'a' && n[3] == 'r') {
        chosen = i;
        break;
      }
    }
    const ImageEntry& e = kImageRegistry[chosen];
    m_img.set(e);
  }
};

// Single shared instance, defined in src/backgrounds.cpp.
extern Backgrounds g_backgrounds;
