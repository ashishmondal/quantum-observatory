// Font demo scene — diagnostic.
//
// Cycles through the fonts that ship with the Adafruit GFX library so
// the operator can pick one by eye on the actual 64×32 panel before
// committing to it for a theme (THEME.md §5). LEFT / RIGHT on the IR
// remote step the active font; UP / DOWN keep their global semantics
// (jump out of the demo into the curated scene cycle).
//
// Selectable over MQTT via {"scene_id":"font_demo"}. wants_clock_chrome
// is false so the chrome readout doesn't fight the sample text.
//
// Layout (64×32):
//   rows  0..6   font label  "n/N NAME"   (built-in 5×7, white)
//   rows  8..31  sample text "Aa Bb 123"  rendered in the ACTIVE font
//                at baseline y=24. Tall fonts (≥18 pt) clip on
//                purpose — the demo's whole point is to see how the
//                glyphs render at native size on this panel.
//
// The font index is a single naturally-aligned uint8_t. Core 0 (IR
// action) writes; Core 1 (render) reads. Atomic on RP2040 — same
// pattern as g_render_fps. No mutex needed.
//
// diagnostic — bypasses theme:: by design (FR-15.3 exemption,
// CODING_PRACTICES §4): the whole point of this scene is to render
// raw GFXfonts directly so the operator can pick one by eye. Do not
// route font selection here through theme::font().

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

// Practical subset of the Adafruit GFX bundle. 24 pt fonts are
// excluded — at 64×32 not even one full glyph fits, so they read as
// a stripe of pixels rather than a font. The built-in 5×7 (entry 0,
// font pointer = nullptr) is included as the baseline reference.
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMono12pt7b.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoOblique9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansOblique9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerifBold9pt7b.h>
#include <Fonts/FreeSerifItalic9pt7b.h>
#include <Fonts/Org_01.h>
#include <Fonts/Picopixel.h>
#include <Fonts/Tiny3x3a2pt7b.h>
#include <Fonts/TomThumb.h>

#include "config.h"
#include "scenes/scene.h"

namespace font_demo_scene_detail {

struct FontEntry {
  const GFXfont* font;        // nullptr = built-in 5×7 glcdfont
  const char*    name;        // shown in the top label, ≤ 10 chars
};

// Order is operator-facing — what they'll page through with LEFT /
// RIGHT. Built-in first as the "you are here" anchor, then small
// pixel fonts (most useful at 64×32), then the FreeFonts grouped by
// family at 9 pt and 12 pt.
inline constexpr FontEntry kFonts[] = {
    {nullptr,                 "5x7 GLCD" },
    {&Picopixel,              "Picopixel"},
    {&TomThumb,               "TomThumb" },
    {&Org_01,                 "Org_01"   },
    {&Tiny3x3a2pt7b,          "Tiny3x3"  },
    {&FreeMono9pt7b,          "Mono9"    },
    {&FreeMonoBold9pt7b,      "MonoB9"   },
    {&FreeMonoOblique9pt7b,   "MonoO9"   },
    {&FreeMono12pt7b,         "Mono12"   },
    {&FreeMonoBold12pt7b,     "MonoB12"  },
    {&FreeSans9pt7b,          "Sans9"    },
    {&FreeSansBold9pt7b,      "SansB9"   },
    {&FreeSansOblique9pt7b,   "SansO9"   },
    {&FreeSans12pt7b,         "Sans12"   },
    {&FreeSansBold12pt7b,     "SansB12"  },
    {&FreeSerif9pt7b,         "Serif9"   },
    {&FreeSerifBold9pt7b,     "SerifB9"  },
    {&FreeSerifItalic9pt7b,   "SerifI9"  },
    {&FreeSerif12pt7b,        "Serif12"  },
};
inline constexpr uint8_t kFontCount =
    sizeof(kFonts) / sizeof(kFonts[0]);

}  // namespace font_demo_scene_detail

class FontDemoScene : public Scene {
public:
  const char* name() const override { return "font_demo"; }
  bool wants_clock_chrome() const override { return false; }

  void init(Adafruit_Protomatter& /*matrix*/) override {
    // Preserve m_index across re-init so re-entering the scene keeps
    // the operator's last selection. Only clamp defensively in case
    // the table shrank between flashes.
    using namespace font_demo_scene_detail;
    if (m_index >= kFontCount) m_index = 0;
  }

  void render(Adafruit_Protomatter& matrix, uint32_t /*now_ms*/) override {
    using namespace font_demo_scene_detail;

    matrix.fillScreen(0x0000);

    const uint8_t        idx   = m_index;  // single read — atomic snapshot
    const FontEntry&     entry = kFonts[idx];

    // --- top label: "n/N NAME" in built-in 5×7 ------------------------
    matrix.setFont(nullptr);
    matrix.setTextSize(1);
    matrix.setTextColor(0xFFFF);  // white
    char label[16];
    snprintf(label, sizeof(label), "%u/%u %s",
             static_cast<unsigned>(idx + 1),
             static_cast<unsigned>(kFontCount),
             entry.name);
    matrix.setCursor(0, 0);
    matrix.print(label);

    // --- sample text in the active font -------------------------------
    matrix.setFont(entry.font);
    matrix.setTextColor(kSampleInk);
    if (entry.font == nullptr) {
      // Built-in glcdfont: top-left anchor.
      matrix.setCursor(0, 10);
    } else {
      // GFX font: baseline anchor. y=24 keeps 12 pt fonts mostly
      // on-panel; taller fonts intentionally clip.
      matrix.setCursor(0, 24);
    }
    matrix.print(kSampleText);
  }

  // Called from Core 0 (IR dispatch). delta is +1 (right / next) or
  // -1 (left / prev). Modular step. Single naturally-aligned uint8_t
  // write — atomic on RP2040.
  void cycle(int8_t delta) {
    using namespace font_demo_scene_detail;
    const uint8_t cur = m_index;
    const uint8_t next = static_cast<uint8_t>(
        (cur + kFontCount + (delta > 0 ? 1 : -1)) % kFontCount);
    m_index = next;
  }

private:
  // Cyan: visible against black, distinct from the white label so the
  // operator can tell label chrome from the actual font sample at a
  // glance. Not theme-aware on purpose — this scene exists to evaluate
  // raw fonts, not theme inks.
  static constexpr uint16_t kSampleInk = 0x07FF;

  // Mixed case + digits + punctuation: catches ascender, descender,
  // bowl, terminal, and digit shape differences in one line.
  static constexpr const char* kSampleText = "Aa Bb 0123";

  volatile uint8_t m_index = 0;
};
