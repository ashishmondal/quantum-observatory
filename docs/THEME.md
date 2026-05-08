# Theming System

**Status:** Draft (T-1) — design only, no code yet. Implementation tracked under FR-15 in [REQUIREMENTS.md](REQUIREMENTS.md).

The Quantum Observatory ships with five named themes drawn from canonical retro
sci-fi reference points. A theme is more than a color swap: it bundles **inks,
fonts, layout hints, bracket conventions, and background palettes** so that
switching themes feels like a different *device*, not a recoloring of the same
one. Themes are selected over MQTT (`observatory/theme`) and resolved on every
frame — scenes never hardcode color, font, or accent rules.

---

## 1. Goals

1. **Authentic feel per theme.** Each reference (Apollo MOCR, Nostromo CRT,
   Vectrex vector glow, Blade Runner neon, Star Trek TOS LCARS-precursor)
   gets at minimum one distinct font and one distinct layout primitive. A
   user staring at the panel from across the room should be able to tell
   themes apart in under a second.
2. **One source of truth.** Scenes consume `theme::ink(...)`, `theme::font(...)`,
   `theme::has(Hint)`, `theme::brackets()`. No literal RGB565 values, no
   `setFont(&Picopixel)` calls in scene code.
3. **Director-controllable.** HA picks the theme; firmware obeys. Same model
   as scenes (FR-1).
4. **No regressions.** Default theme reproduces the current look pixel-for-pixel.

---

## 2. The five themes

### 2.1 APOLLO_AMBER (default)
70s NASA Mission Operations Control Room — burnt amber phosphor on black,
seven-segment digits, all-caps bracketed labels.

- **Fonts** (the four roles, see §3.1):
  - `MICRO`  — Tiny3x3 (shared by all themes)
  - `BODY`   — Picopixel (3×5 with descenders)
  - `HEADER` — Press Start 2P (8×8 chunky, the MOCR look)
  - `CLOCK`  — Digital-7 mono 14pt (ships today)
- **Inks:** amber `0xFD60`, warm amber `0xFCA0`, white accent, dim amber halo
- **Brackets:** `[` `]`
- **Hints:** `GIANT_DIGIT_GHOST` (the "18:88" unlit-segment shadow)
- **BG retoning:** none — passthrough (§6.3). Images render in their original colors.

### 2.2 NOSTROMO_GREEN
80s *Alien* / Nostromo MU/TH/UR computer — high-contrast phosphor green CRT
with cursor blocks, scanlines, and command-line voice.

- **Fonts:**
  - `MICRO`  — Tiny3x3
  - `BODY`   — TomThumb
  - `HEADER` — VT323 (CRT terminal, larger size)
  - `CLOCK`  — Digital-7 mono 14pt (recolored green)
- **Inks:** phosphor green `0x07E0`, dim green `0x0560`, yellow alert `0xFFE0`
- **Brackets:** `>` `_`
- **Hints:** `SCANLINES` (every other row dimmed by ~30%), `CURSOR_BLOCK`
  (blinking 2×5 block after each header)
- **BG retoning:** highlight `#40FF40` phosphor green, shadow `#003000` deep green, white anchor `#D8FFD8` pale green-white.

### 2.3 VECTREX_NEON
Atari/Vectrex vector arcade — thin glowing strokes, cyan + magenta
complementary halos, deep black background.

- **Fonts:**
  - `MICRO`  — Tiny3x3
  - `BODY`   — Picopixel (3×5 with descenders — thin grid)
  - `HEADER` — Pixel Operator (vector-arcade feel comes from
    `NEON_OUTLINE` + the cyan/magenta inks, not the typeface)
  - `CLOCK`  — Digital-7 mono 14pt (cyan, with NEON_OUTLINE halo)
- **Inks:** cyan `0x07FF`, magenta glow `0xF81F`, white accent
- **Brackets:** `<` `>`
- **Hints:** `NEON_OUTLINE` (halo drawn in glow color, not black)
- **BG retoning:** highlight `#00FFFF` cyan, shadow `#FF00FF` magenta (deliberately complementary — sells the vector-glow look across midtones), white anchor `#FFFFFF`.

### 2.4 BLADE_RUNNER
Late-80s neo-noir HUD — saturated cyan + magenta on deep blue, framed
panels, occasional Japanese-katakana flavor (out of scope for v1).

- **Fonts:**
  - `MICRO`  — Tiny3x3
  - `BODY`   — Org_01 (5×6 sans, true lowercase)
  - `HEADER` — Pixel Operator
  - `CLOCK`  — Digital-7 mono 14pt (cyan)
- **Inks:** cyan `0x07FF`, orange `0xFD20`, magenta accent `0xF81F`
- **Brackets:** `:` `:` (colons as flanking dividers — built-in glyph)
- **Hints:** `FRAME_BORDER` (1-px outer cyan rectangle), `NEON_OUTLINE`
- **BG retoning:** highlight `#00E0FF` cyan, shadow `#FF7000` deep orange, white anchor `#E0F8FF` cool white.

### 2.5 LCARS_TOS
Star Trek TOS LCARS-precursor — colored solid blocks, no brackets, blocky
sans-serif labels in orange/yellow/red.

- **Fonts:**
  - `MICRO`  — Tiny3x3
  - `BODY`   — Org_01 (5×6 sans)
  - `HEADER` — Pixel Operator
  - `CLOCK`  — Digital-7 mono 14pt (orange)
- **Inks:** orange `0xFD20`, yellow `0xFFE0`, red alert `0xF800`
- **Brackets:** none — replaced by colored block bars (`drawFillRect`)
- **Hints:** `BLOCK_BARS` (header is preceded by a 4×7 colored block instead
  of a `[`), `FRAME_BORDER` (single-color block frame)
- **BG retoning:** highlight `#FF9933` LCARS orange, shadow `#CC0000` red, white anchor `#FFE080` warm yellow-white.

> **Note on LCARS:** Authentic LCARS typography (Okuda / Federation fonts)
> is licensed "personal use only" by most uploaders. Shipping it in a
> source-distributed firmware repo is not safe. The LCARS *look* at 64×32
> px is dominated by the colored block layout, not the typeface — Pixel
> Operator in LCARS colors reads as LCARS to the eye.

---

## 3. Architecture

### 3.1 Module: `theme`

```cpp
namespace theme {
  enum class Id : uint8_t {
    APOLLO_AMBER = 0, NOSTROMO_GREEN, VECTREX_NEON,
    BLADE_RUNNER, LCARS_TOS, COUNT
  };

  enum class Ink : uint8_t {
    CHROME, CHROME_HALO,
    HEADER, HEADER_HALO, HEADER_GLOW, HEADER_DIM,
    BODY,   BODY_HALO,   BODY_GLOW,
    ACCENT, ACCENT_MAGENTA, ALERT,
    GHOST, DIVIDER, GIANT_DIGITS,
    // Status family — semantic state shared by typewriter scenes
    // (iss_pass, jupiter_visibility, moon_phase, constellation_now).
    // The *_DIM siblings of OK / WARN are the pulse-low value of an
    // identity-coloured header (e.g. ISS [ISS] pulses STATUS_OK ↔
    // STATUS_OK_DIM); generic-header scenes use HEADER ↔ HEADER_DIM.
    STATUS_OK, STATUS_OK_DIM,
    STATUS_WARN, STATUS_WARN_DIM,
    STATUS_INFO, STATUS_STALE, STATUS_DIM,
    LABEL, VALUE,
    SAFETY,                 // night + thermal_safe deep red
  };

  enum class FontRole : uint8_t {
    MICRO,         // 1–3 char indicators only — Tiny3x3, all themes
    BODY,          // readable lines: stat readouts, prompts
    HEADER,        // bracketed scene labels, button prompts
    CLOCK,         // Digital-7 14pt giant HH:MM, all themes
  };

  enum class Hint : uint8_t {
    GIANT_DIGIT_GHOST, SCANLINES, CURSOR_BLOCK,
    NEON_OUTLINE, FRAME_BORDER, BLOCK_BARS,
  };

  void            set(Id id);                       // Core 0 (MQTT)
  Id              current();                        // Core 1 (render)
  uint16_t        ink(Ink role);
  const GFXfont*  font(FontRole r);                 // nullptr = built-in 5x7
  bool            has(Hint h);
  const char*     bracket_open();
  const char*     bracket_close();
  palette::Id     bg_palette_for(BgType bg);        // FR-15.6
}
```

Concurrency: a single naturally-aligned `uint8_t` holds the active id.
Atomic on RP2040 — same pattern as `g_render_fps`. No mutex.

### 3.2 Scene refactor

**Before:**
```cpp
constexpr uint16_t kInfoInk = 0xF940;
matrix.setFont(&Picopixel);
gfx::draw_text_halo(matrix, x, 29, str, kInfoInk, 0x0000);
```

**After:**
```cpp
matrix.setFont(theme::font(theme::FontRole::BODY));
gfx::draw_text_halo(matrix, x, 29, str,
                    theme::ink(theme::Ink::BODY),
                    theme::ink(theme::Ink::BODY_HALO));
```

### 3.3 Header / bracket helper

```cpp
char hdr[12];
snprintf(hdr, sizeof(hdr), "%sISS%s",
         theme::bracket_open(), theme::bracket_close());
```

For `BLOCK_BARS`-themed renders, a helper `gfx::draw_themed_header(matrix,
"ISS", x, y)` fills in a colored block instead of a bracket glyph.

### 3.4 Overlay layer (post-scene, pre-show)

In `loop1()` between scene render and `matrix.show()`:

```cpp
g_current_scene->render(matrix, now_ms);
if (g_current_scene->wants_clock_chrome()) {
  gfx::draw_clock_chrome(matrix, now_ms);
}
if (theme::has(Hint::FRAME_BORDER))   gfx::draw_theme_frame(matrix);
if (theme::has(Hint::SCANLINES))      gfx::draw_theme_scanlines(matrix);
matrix.show();
```

Cheap: a frame is one rect outline; scanlines are 16 hlines.

---

## 4. MQTT contract

**Topic:** `observatory/theme`
**Payload:** `{"id": "apollo_amber"}` — string id matches `theme::Id` names
in lowercase. Unknown ids are logged and ignored (FR-1.3 spirit).
**Persistence:** none — boots to `apollo_amber`, HA pushes desired theme
on connect (same lifecycle as scenes; no flash wear).
**Heartbeat:** the existing `observatory/status` payload gains a `"theme"`
field so HA can confirm what's active without round-tripping.

HA exposes a `select` entity (`select.observatory_theme`) backed by this
topic — added to `homeassistant/setup_mqtt.py` alongside the scene select.

---

## 5. Fonts to bundle

All themes share a **four-role size ladder** (§3.1 `FontRole`):
`MICRO < BODY < HEADER < CLOCK`. Themes differ in which font they
bind to BODY and HEADER — MICRO and CLOCK are the same across every
theme.

- **MICRO** — Tiny3x3 (2 pt, ships with Adafruit_GFX). Every theme.
  Used for 1–3 character indicators only (priority dots, status
  flags, badges); never for words. Kept in the build deliberately so
  scenes can opt in as the design evolves.
- **CLOCK** — Digital-7 mono 14 pt. Every theme. Recolored per theme;
  geometry never changes.
- **BODY** and **HEADER** — per-theme, see §2.

The `BODY` slot is filled by one of three Adafruit_GFX bundled pixel
fonts, picked to match each theme's identity:

| Theme | BODY font (built-in, no bundling cost) |
|---|---|
| `apollo_amber`    | Picopixel (3×5 with descenders) |
| `nostromo_green`  | TomThumb (3×5, no descenders)   |
| `vectrex_neon`    | Picopixel                         |
| `blade_runner`    | Org_01 (5×6 sans, true lowercase)|
| `lcars_tos`       | Org_01                           |

> **TomThumb baseline quirk.** TomThumb's GFX glyphs sit one pixel
> *above* the baseline that Picopixel/Org_01 use, so any scene that
> swaps fonts at a fixed `y` will see TomThumb text float one row
> high. When drawing TomThumb (directly or via the BODY role on
> `nostromo_green`), add **+1 to `y`** so it lines up with the other
> BODY fonts at the same baseline.

`HEADER` is the biggest theme differentiator but draws from a fixed
roster of **only three** TTF conversions — deliberately small so the
set is easy to license, attribute, and PROGMEM-budget. Each theme
picks one:

| Theme              | HEADER font           |
|---|---|
| `apollo_amber`     | Press Start 2P        |
| `nostromo_green`   | VT323                 |
| `vectrex_neon`     | Pixel Operator        |
| `blade_runner`     | Pixel Operator        |
| `lcars_tos`        | Pixel Operator        |

Three themes share Pixel Operator — their visual identity rides on
inks + layout hints (`NEON_OUTLINE` for Vectrex, `FRAME_BORDER` for
Blade Runner, `BLOCK_BARS` for LCARS), not the typeface. This keeps
the bundled-TTF count at three.

| File (`include/fonts/`) | Source TTF (`assets/fonts/`) | Used by |
|---|---|---|
| `digital_7__mono_14pt7b.h` *(shipped)* | `digital-7 (mono).ttf` *(shipped)* | All themes (`CLOCK`) |
| `press_start_2p_8pt7b.h`       | `PressStart2P.ttf` (codeman38, OFL)         | APOLLO `HEADER` |
| `vt323_8pt7b.h`                | `VT323-Regular.ttf` (Peter Hull, OFL)       | NOSTROMO `HEADER` |
| `pixel_operator_8pt7b.h`       | `PixelOperator8.ttf` (Jayvee Enaguas, CC0)  | VECTREX + BLADE_RUNNER + LCARS `HEADER` |

License attribution stubs land alongside each header in T-6. All
three bundled TTFs above are confirmed permissive (OFL / CC0).
Blade Runner and LCARS marquee fonts are deliberately **not** bundled
— the look is achieved with permissive lookalikes plus the theme's
ink/hint primitives.

PROGMEM cost (measured from `// Approx. N bytes` in each generated
header): Press Start 2P 2508 B + VT323 1065 B + Pixel Operator 1998 B
≈ **5.6 KB** flash for the three HEADER fonts. Zero SRAM cost (read
directly from flash via Adafruit_GFX). The three BODY fonts and the
MICRO font are already linked because they ship with Adafruit_GFX —
no additional cost.

---

## 6. Backgrounds — runtime duotone retoning

Background scenes (`STARFIELD`, `PARALLAX`, `NEBULA`, `BITMAP`, `IMAGE`)
currently bind a hardcoded `palette::Id`. With themes added, every theme
should be able to retone the same pixel data — but artists shouldn't have
to author against an abstract index space, and the default theme should
stay full-color.

The model is **runtime duotone synthesis**: artwork ships in its original
colors. The default theme passes through. Any other theme supplies two
colors (highlight + shadow), and at theme-switch time the firmware
synthesizes a per-image runtime palette by mapping each source-palette
entry's luminance through a 4-stop ramp.

### 6.1 The ramp

Every non-default theme declares two RGB values plus optional anchor
overrides:

```
BG_BLACK     (default #000000  — hard-clamped to true black)
BG_SHADOW    (required)
BG_HIGHLIGHT (required)
BG_WHITE     (default #FFFFFF, but typically tinted toward the highlight
              hue, e.g. #FFE8C0 for Apollo)
```

A 256-entry RGB565 LUT is built once per theme switch by linear
interpolation across these stops:

```
lum 0   → BG_BLACK
lum 85  → BG_SHADOW       (1/3)
lum 170 → BG_HIGHLIGHT    (2/3)
lum 255 → BG_WHITE
```

### 6.2 Per-image runtime palette

Each themable image carries its **original baked palette** in flash
(unchanged) plus a build-time-precomputed `uint8_t lum[192]` luminance
table. At theme-switch time, for each themable image:

```cpp
for (int i = 0; i < 192; i++) {
  runtime_palette[i] = ramp_lut[image.lum[i]];
}
```

No per-pixel work, no math — a 192-entry table copy per image.

Luminance uses ITU-R BT.601 (`0.299 R + 0.587 G + 0.114 B`) computed at
build time. The build-time pipeline also **histogram-stretches** each
image's luminance against its 1st–99th percentile range so bunched
images (most photos / most art) use the full ramp instead of a thin
slice. Stretching is per-image and bakes into the `lum[]` table; runtime
sees only the stretched values.

### 6.3 Default theme = passthrough

APOLLO_AMBER (the default) does **not** declare a duotone pair for
backgrounds. When it is active, the background renderer uses each image's
original baked palette — today's full-color look is preserved bit-for-bit.
Non-default themes opt **in** to retoning by declaring `BG_HIGHLIGHT` /
`BG_SHADOW`.

### 6.4 Per-image opt-out

An artist can mark a single image as never-retoned by dropping an empty
sidecar file: `assets/<name>.notheme`. The pipeline then sets
`themeable: false` on that registry entry and skips emitting `lum[]`.
Every theme renders such an image in its original palette, regardless of
theme choice.

Use for: scenes where color *is* the information (e.g. a hypothetical
Saturn-rings scene where ring color carries meaning).

### 6.5 Concurrency — double-buffered runtime palettes

Core 1 reads palette tables every frame; Core 0 receives the MQTT theme
message and rebuilds. To avoid a torn frame each themable image owns
**two** runtime palette buffers and a one-byte active-index. Core 0
builds into the inactive buffer, then writes the active-index byte
(naturally aligned, atomic on RP2040 — same pattern as `g_render_fps`).
Next frame, Core 1 picks up the new palette automatically.

### 6.6 Costs

| | Cost |
|---|---|
| Flash, per themable image | +192 B (`lum[]` table) |
| SRAM, per themable image | 2 × 192 × 2 B = 768 B (double-buffered) |
| SRAM total at 5 themable images | ~3.8 KB (within NFR-2.1) |
| Theme-switch work | 256 ramp interpolations once + 192 LUT writes per image |
| Theme-switch wall time, 5 images | < 5 ms (well under FR-1.2's 250 ms) |

### 6.7 Procedural backgrounds

`STARFIELD`, `NEBULA`, `PARALLAX` are pixel-time-computed and don't have
a baked palette to retone. They consult `theme::bg_palette_for(BgType)`
which, for non-default themes, returns a synthetic `palette::Id` whose
stops are derived from the same ramp (same code path). Procedural BGs
therefore retone for free with no asset changes.

### 6.8 Build-time pipeline changes

`tools/bmp_to_header.py` gains:

- BT.601 luminance computation per palette entry.
- Per-image 1st–99th percentile histogram stretch over actually-used
  entries.
- `uint8_t k<Name>_lum[192]` emission alongside the existing palette /
  pixels arrays.
- `assets/<name>.notheme` detection → `themeable: false`, no `lum[]`.

No existing asset needs re-authoring. No existing scene needs API
changes.

---

## 7. Interaction with safety overrides

`THERMAL_SAFE` and `NIGHT` are firmware-owned overrides (FR-7.5) that
preempt the active scene. **They keep the active theme** — only the
brightness drops. Concretely:

- `night_scene` and `thermal_safe_scene` consult `theme::ink(Ink::BODY)`
  but render at the dim end of the FG ramp instead of full brightness.
- The theme-driven hints (scanlines, frame border, etc.) still apply.

This preserves theme identity even in the dim states — Nostromo green at
20% brightness is unmistakably still Nostromo, not generic dim-amber.

---

## 8. Phasing

| Phase | Scope | Visual change |
|---|---|---|
| **T-1** | This doc + FR-15 in REQUIREMENTS + README link | none (docs only) |
| **T-2** | `theme.h/.cpp` skeleton; APOLLO_AMBER only with today's exact colors/fonts | none — pixel-identical |
| **T-3** | Refactor every scene to consume `theme::ink/font/brackets`; delete hardcoded literals | none — pixel-identical |
| **T-4** | MQTT `observatory/theme` topic, status field, HA select entity | none — APOLLO still active |
| **T-5** | Add NOSTROMO_GREEN with scanline + cursor-block hints (no new fonts yet) | first themable demo |
| **T-6** | Convert + bundle all six TTFs (closes FR-4.1 placeholder) | APOLLO upgrades to real Silkscreen + Press Start 2P |
| **T-7** | Add VECTREX_NEON, BLADE_RUNNER, LCARS_TOS + extra star palettes | full theme set |
| **T-8** | Update `gfx_test` to cycle every theme + ink role + hint | diagnostic coverage |
| **T-9** | Migrate existing `.bmp` assets to palette-indexed wide-range form per §6; add per-theme BG palettes | backgrounds retone with theme |

Each phase is independently demoable. T-3 is the no-regression milestone.

---

## 9. Open questions

1. **Per-scene theme override** — should an MQTT scene request optionally
   carry a `"theme"` field that overrides the global theme just for that
   scene's lifetime? (e.g., "ISS now" callouts always render in
   alert-red regardless of theme.) Defer — adds API surface; not needed
   until we have a use case.
2. **Brightness coupling** — should themes carry a default global
   brightness multiplier? (Vectrex looks better dim; LCARS wants full
   blast.) Likely yes — add to the per-theme `Def` table. Decide in T-5.
3. **Custom glyphs** — VECTREX/BLADE_RUNNER would benefit from
   `‹›`, `‖`, `▮`. We can add these to the converted fonts at slots
   0x80+ instead of constraining to ASCII. Decide in T-6.
