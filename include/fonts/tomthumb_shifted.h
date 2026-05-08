// TomThumb with the baseline shifted +1 row down.
//
// Adafruit's bundled TomThumb encodes its glyph cells one pixel above
// the baseline used by Picopixel / Org_01 / our HEADER faces, so any
// scene that swaps a font in at a fixed `y` sees TomThumb text float
// one row high. Rather than scatter "+1 if TomThumb" plumbing across
// every scene + helper (THEME.md §2.3), we ship a corrected glyph
// table here: identical to upstream except every yOffset has +1 baked
// in. The bitmap data is reused unchanged from <Fonts/TomThumb.h> —
// only the per-glyph offset table is duplicated (~570 B PROGMEM).
//
// Bind via `&fonts::TomThumbShifted`; behaves like a drop-in for the
// upstream `TomThumb` symbol but renders one row lower so it shares a
// baseline with the other BODY-role fonts.
//
// Glyph table mirrors the ASCII range (0x20..0x7E) of upstream
// TomThumb.h — TOMTHUMB_USE_EXTENDED is not enabled in this build.

#pragma once

#include <Adafruit_GFX.h>
#include <Fonts/TomThumb.h>  // pulls in TomThumbBitmaps[]

namespace fonts {

// {bitmapOffset, width, height, xAdvance, xOffset, yOffset}
// yOffset = upstream value + 1 (every entry).
static const GFXglyph TomThumbShiftedGlyphs[] PROGMEM = {
    {0,   1, 1, 2, 0, -4},  /* 0x20 space */
    {1,   1, 5, 2, 0, -4},  /* 0x21 exclam */
    {2,   3, 2, 4, 0, -4},  /* 0x22 quotedbl */
    {3,   3, 5, 4, 0, -4},  /* 0x23 numbersign */
    {5,   3, 5, 4, 0, -4},  /* 0x24 dollar */
    {7,   3, 5, 4, 0, -4},  /* 0x25 percent */
    {9,   3, 5, 4, 0, -4},  /* 0x26 ampersand */
    {11,  1, 2, 2, 0, -4},  /* 0x27 quotesingle */
    {12,  2, 5, 3, 0, -4},  /* 0x28 parenleft */
    {14,  2, 5, 3, 0, -4},  /* 0x29 parenright */
    {16,  3, 3, 4, 0, -4},  /* 0x2A asterisk */
    {18,  3, 3, 4, 0, -3},  /* 0x2B plus */
    {20,  2, 2, 3, 0, -1},  /* 0x2C comma */
    {21,  3, 1, 4, 0, -2},  /* 0x2D hyphen */
    {22,  1, 1, 2, 0,  0},  /* 0x2E period */
    {23,  3, 5, 4, 0, -4},  /* 0x2F slash */
    {25,  3, 5, 4, 0, -4},  /* 0x30 zero */
    {27,  2, 5, 3, 0, -4},  /* 0x31 one */
    {29,  3, 5, 4, 0, -4},  /* 0x32 two */
    {31,  3, 5, 4, 0, -4},  /* 0x33 three */
    {33,  3, 5, 4, 0, -4},  /* 0x34 four */
    {35,  3, 5, 4, 0, -4},  /* 0x35 five */
    {37,  3, 5, 4, 0, -4},  /* 0x36 six */
    {39,  3, 5, 4, 0, -4},  /* 0x37 seven */
    {41,  3, 5, 4, 0, -4},  /* 0x38 eight */
    {43,  3, 5, 4, 0, -4},  /* 0x39 nine */
    {45,  1, 3, 2, 0, -3},  /* 0x3A colon */
    {46,  2, 4, 3, 0, -3},  /* 0x3B semicolon */
    {47,  3, 5, 4, 0, -4},  /* 0x3C less */
    {49,  3, 3, 4, 0, -3},  /* 0x3D equal */
    {51,  3, 5, 4, 0, -4},  /* 0x3E greater */
    {53,  3, 5, 4, 0, -4},  /* 0x3F question */
    {55,  3, 5, 4, 0, -4},  /* 0x40 at */
    {57,  3, 5, 4, 0, -4},  /* 0x41 A */
    {59,  3, 5, 4, 0, -4},  /* 0x42 B */
    {61,  3, 5, 4, 0, -4},  /* 0x43 C */
    {63,  3, 5, 4, 0, -4},  /* 0x44 D */
    {65,  3, 5, 4, 0, -4},  /* 0x45 E */
    {67,  3, 5, 4, 0, -4},  /* 0x46 F */
    {69,  3, 5, 4, 0, -4},  /* 0x47 G */
    {71,  3, 5, 4, 0, -4},  /* 0x48 H */
    {73,  3, 5, 4, 0, -4},  /* 0x49 I */
    {75,  3, 5, 4, 0, -4},  /* 0x4A J */
    {77,  3, 5, 4, 0, -4},  /* 0x4B K */
    {79,  3, 5, 4, 0, -4},  /* 0x4C L */
    {81,  3, 5, 4, 0, -4},  /* 0x4D M */
    {83,  3, 5, 4, 0, -4},  /* 0x4E N */
    {85,  3, 5, 4, 0, -4},  /* 0x4F O */
    {87,  3, 5, 4, 0, -4},  /* 0x50 P */
    {89,  3, 5, 4, 0, -4},  /* 0x51 Q */
    {91,  3, 5, 4, 0, -4},  /* 0x52 R */
    {93,  3, 5, 4, 0, -4},  /* 0x53 S */
    {95,  3, 5, 4, 0, -4},  /* 0x54 T */
    {97,  3, 5, 4, 0, -4},  /* 0x55 U */
    {99,  3, 5, 4, 0, -4},  /* 0x56 V */
    {101, 3, 5, 4, 0, -4},  /* 0x57 W */
    {103, 3, 5, 4, 0, -4},  /* 0x58 X */
    {105, 3, 5, 4, 0, -4},  /* 0x59 Y */
    {107, 3, 5, 4, 0, -4},  /* 0x5A Z */
    {109, 3, 5, 4, 0, -4},  /* 0x5B bracketleft */
    {111, 3, 3, 4, 0, -3},  /* 0x5C backslash */
    {113, 3, 5, 4, 0, -4},  /* 0x5D bracketright */
    {115, 3, 2, 4, 0, -4},  /* 0x5E asciicircum */
    {116, 3, 1, 4, 0,  0},  /* 0x5F underscore */
    {117, 2, 2, 3, 0, -4},  /* 0x60 grave */
    {118, 3, 4, 4, 0, -3},  /* 0x61 a */
    {120, 3, 5, 4, 0, -4},  /* 0x62 b */
    {122, 3, 4, 4, 0, -3},  /* 0x63 c */
    {124, 3, 5, 4, 0, -4},  /* 0x64 d */
    {126, 3, 4, 4, 0, -3},  /* 0x65 e */
    {128, 3, 5, 4, 0, -4},  /* 0x66 f */
    {130, 3, 5, 4, 0, -3},  /* 0x67 g */
    {132, 3, 5, 4, 0, -4},  /* 0x68 h */
    {134, 1, 5, 2, 0, -4},  /* 0x69 i */
    {135, 3, 6, 4, 0, -4},  /* 0x6A j */
    {138, 3, 5, 4, 0, -4},  /* 0x6B k */
    {140, 3, 5, 4, 0, -4},  /* 0x6C l */
    {142, 3, 4, 4, 0, -3},  /* 0x6D m */
    {144, 3, 4, 4, 0, -3},  /* 0x6E n */
    {146, 3, 4, 4, 0, -3},  /* 0x6F o */
    {148, 3, 5, 4, 0, -3},  /* 0x70 p */
    {150, 3, 5, 4, 0, -3},  /* 0x71 q */
    {152, 3, 4, 4, 0, -3},  /* 0x72 r */
    {154, 3, 4, 4, 0, -3},  /* 0x73 s */
    {156, 3, 5, 4, 0, -4},  /* 0x74 t */
    {158, 3, 4, 4, 0, -3},  /* 0x75 u */
    {160, 3, 4, 4, 0, -3},  /* 0x76 v */
    {162, 3, 4, 4, 0, -3},  /* 0x77 w */
    {164, 3, 4, 4, 0, -3},  /* 0x78 x */
    {166, 3, 5, 4, 0, -3},  /* 0x79 y */
    {168, 3, 4, 4, 0, -3},  /* 0x7A z */
    {170, 3, 5, 4, 0, -4},  /* 0x7B braceleft */
    {172, 1, 5, 2, 0, -4},  /* 0x7C bar */
    {173, 3, 5, 4, 0, -4},  /* 0x7D braceright */
    {175, 3, 2, 4, 0, -4},  /* 0x7E asciitilde */
};

static const GFXfont TomThumbShifted PROGMEM = {
    (uint8_t*)TomThumbBitmaps,
    (GFXglyph*)TomThumbShiftedGlyphs,
    0x20, 0x7E, 6,
};

}  // namespace fonts
