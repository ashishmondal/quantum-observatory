// Shared "deck of 3-line cards" helper for the FR-21 text scenes
// (weather, sky_now, light_travel, relativity, black_holes, aurora,
// artemis, hail_mary). Each card types its three lines out one glyph
// at a time, holds, then the deck advances. All timing is derived
// from (now_ms - deck_start) so scenes stay stateless across frames
// apart from the anchor (CODING_PRACTICES §2 wrap-safe deltas).
//
// Layout matches iss_pass: header band y=0..7 (scene draws it, the
// compositor adds the FR-9.2 HH:MM chrome top-right), body lines at
// BODY-font baselines 14 / 22 / 30.

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "gfx_text.h"
#include "theme.h"

namespace cards {

constexpr uint8_t  kLineCap   = 17;     // 16 glyphs + NUL
constexpr uint32_t kCharMs    = 60;
constexpr uint32_t kLinePause = 250;
constexpr uint32_t kHoldMs    = 4500;
constexpr int16_t  kBaseY[3]  = { 14, 22, 30 };

struct Card {
  char     line[3][kLineCap];
  uint16_t ink[3];
};

inline void set(Card& c, uint8_t i, uint16_t ink, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));
inline void set(Card& c, uint8_t i, uint16_t ink, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c.line[i], kLineCap, fmt, ap);
  va_end(ap);
  c.ink[i] = ink;
}

inline void clear(Card& c) {
  for (auto& l : c.line) l[0] = '\0';
  for (auto& k : c.ink) k = 0;
}

// Total on-screen time for a card: typing + inter-line pauses + hold.
inline uint32_t duration_ms(const Card& c) {
  uint32_t t = kHoldMs;
  for (const auto& l : c.line) t += static_cast<uint32_t>(strlen(l)) * kCharMs + kLinePause;
  return t;
}

// Draw `c` as of `elapsed` ms into the card. `x0` is the left margin;
// `backdrop` paints a black strip behind typed glyphs so text stays
// legible over an animated visual.
inline void draw(Adafruit_Protomatter& m, const Card& c, uint32_t elapsed,
                 int16_t x0 = 2, bool backdrop = false) {
  m.setFont(theme::font(theme::FontRole::BODY));
  m.setTextSize(1);
  uint32_t t = elapsed;
  for (uint8_t i = 0; i < 3; ++i) {
    const uint8_t len = static_cast<uint8_t>(strlen(c.line[i]));
    if (len == 0) continue;
    const uint32_t typing = static_cast<uint32_t>(len) * kCharMs;
    uint8_t shown = len;
    bool    typing_now = false;
    if (t < typing) {
      shown = static_cast<uint8_t>(t / kCharMs);
      typing_now = true;
    }
    char buf[kLineCap];
    memcpy(buf, c.line[i], shown);
    buf[shown] = '\0';
    int16_t bx = 0, by = 0;
    uint16_t bw = 0, bh = 0;
    if (shown > 0) m.getTextBounds(buf, x0, kBaseY[i], &bx, &by, &bw, &bh);
    if (backdrop && (shown > 0 || typing_now)) {
      m.fillRect(x0 - 1, kBaseY[i] - 6, static_cast<int16_t>(bw) + 6, 7, 0x0000);
    }
    if (shown > 0) {
      m.setTextColor(c.ink[i]);
      m.setCursor(x0, kBaseY[i]);
      m.print(buf);
    }
    if (typing_now) {
      if (((elapsed / 280u) & 1u) == 0u) {
        m.fillRect(x0 + static_cast<int16_t>(bw) + 1, kBaseY[i] - 5, 2, 5, c.ink[i]);
      }
      return;
    }
    if (t < typing + kLinePause) return;
    t -= typing + kLinePause;
  }
}

// Word-wrap `text` into up to `max_lines` lines of ≤ `width` glyphs.
// Returns the number of lines written.
inline uint8_t wrap(const char* text, uint8_t width,
                    char out[][kLineCap], uint8_t max_lines) {
  if (width >= kLineCap) width = kLineCap - 1;
  uint8_t n = 0;
  const char* p = text;
  while (*p != '\0' && n < max_lines) {
    while (*p == ' ') ++p;
    if (*p == '\0') break;
    size_t take = strlen(p);
    if (take > width) {
      take = width;
      size_t cut = take;
      while (cut > 0 && p[cut] != ' ') --cut;
      if (cut > 0) take = cut;
    }
    memcpy(out[n], p, take);
    out[n][take] = '\0';
    ++n;
    p += take;
  }
  return n;
}

// Pulsing scene identity header, same idiom as iss_pass.
inline void header(Adafruit_Protomatter& m, const char* tag, uint32_t now_ms,
                   theme::Ink bright = theme::Ink::HEADER,
                   theme::Ink dim = theme::Ink::HEADER_DIM) {
  const bool low = (now_ms % 1500u) < 200u;
  gfx::draw_scene_header(m, tag, 0, 0, theme::ink(low ? dim : bright));
}

// 8-point compass label for an azimuth in degrees.
inline const char* compass8(int az) {
  static const char* const kDir[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
  az %= 360;
  if (az < 0) az += 360;
  return kDir[((az * 2 + 45) / 90) % 8];
}

// Advances through a deck of `count` cards. Each scene owns one Deck;
// reset() on init(). current() returns the card index and the elapsed
// ms inside it; scenes call it once per frame with the freshly-filled
// card so duration tracks the real text length.
class Deck {
 public:
  void reset() { m_idx = 0; m_start_ms = 0; }
  // Returns elapsed ms inside the current card; advances when done.
  uint32_t step(uint32_t now_ms, uint32_t card_ms, uint8_t count) {
    if (m_start_ms == 0) m_start_ms = now_ms;
    if (count == 0) count = 1;
    if (m_idx >= count) m_idx = 0;
    uint32_t e = now_ms - m_start_ms;
    if (e >= card_ms) {
      m_idx = static_cast<uint8_t>((m_idx + 1) % count);
      m_start_ms = now_ms;
      e = 0;
    }
    return e;
  }
  uint8_t index() const { return m_idx; }

 private:
  uint8_t  m_idx      = 0;
  uint32_t m_start_ms = 0;
};

}  // namespace cards
