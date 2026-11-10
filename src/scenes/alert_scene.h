// alert scene (FR-21.3) — "GO LOOK NOW".
//
// Pushed by HA on observatory/alert a minute or two before something
// worth stepping outside for: a visible ISS pass, a meteor-shower
// peak, a bright conjunction / opposition, or a geomagnetic storm
// strong enough for aurora at this latitude. mqtt_link switches here
// at priority 4 for the alert's duration and plays a fanfare.
//
// Unlike the card scenes the text is drawn instantly (no typewriter)
// and a pulsing frame grabs attention from across the room.

#pragma once

#include <Adafruit_Protomatter.h>

#include "cards.h"
#include "feeds.h"
#include "scene.h"
#include "theme.h"

class AlertScene : public Scene {
 public:
  const char* name() const override { return "alert"; }

  void init(Adafruit_Protomatter& matrix) override { matrix.setTextWrap(false); }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    matrix.fillScreen(0x0000);
    feeds::Alert a{};
    uint32_t age_ms = 0;
    const bool fresh = feeds::get(now_ms, &a, &age_ms);

    // 1 Hz pulsing frame (brighter half of each second).
    const bool hi = (now_ms % 1000u) < 600u;
    const uint16_t frame = theme::ink(hi ? theme::Ink::ALERT : theme::Ink::HEADER_DIM);
    matrix.drawRect(0, 0, PANEL_WIDTH, PANEL_HEIGHT, frame);

    cards::header(matrix, "LOOK", now_ms, theme::Ink::ALERT, theme::Ink::HEADER_DIM);

    matrix.setFont(theme::font(theme::FontRole::BODY));
    matrix.setTextSize(1);
    if (!fresh) {
      matrix.setTextColor(theme::ink(theme::Ink::STATUS_DIM));
      matrix.setCursor(3, cards::kBaseY[1]);
      matrix.print("ALL CLEAR");
      return;
    }
    const uint16_t inks[3] = {
      theme::ink(theme::Ink::ACCENT),
      theme::ink(theme::Ink::STATUS_OK),
      theme::ink(theme::Ink::STATUS_WARN),
    };
    const char* lines[3] = { a.title, a.line2, a.line3 };
    for (uint8_t i = 0; i < 3; ++i) {
      matrix.setTextColor(inks[i]);
      matrix.setCursor(3, cards::kBaseY[i]);
      matrix.print(lines[i]);
    }
    // Remaining-time bar along the bottom frame edge.
    const uint32_t dur = static_cast<uint32_t>(a.duration_s) * 1000u;
    const int16_t w = dur == 0 ? 0 : static_cast<int16_t>(
        (static_cast<uint64_t>(dur - (age_ms < dur ? age_ms : dur)) * (PANEL_WIDTH - 2)) / dur);
    matrix.drawFastHLine(1, PANEL_HEIGHT - 1, w, theme::ink(theme::Ink::STATUS_OK));
  }
};
