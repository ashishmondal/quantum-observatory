// SafetyOverlayLayer — D.3 firmware safety overrides as compositor
// overlays (FR-16.2).
//
// Phase 5.5/6.4/6.5+ originally modeled NIGHT, THERMAL_SAFE, OFFLINE,
// and SPLASH as dispatcher-preempting SceneIds: scene_state::resolve()
// returned the override id, take_pending() forced a Scene swap, and the
// underlying Director scene was destroyed (init() rerun on revert).
// FR-16.2 inverts that: the dispatcher's active scene is whatever the
// Director or default policy chose, and the safety override sits in
// LAYER_OVERLAY_SAFETY drawn on top of it. So when an override clears
// (LDR uncovered, panel cooled, MQTT reconnected), the underlying
// scene resumes from where it was — e.g. ConstellationNow's slow
// reveal continues, no restart.
//
// Priority (matches the pre-D.3 resolve() and the merged FR-13.1 +
// FR-7.5 + FR-5.1 contract): SPLASH > THERMAL > NIGHT > OFFLINE.
// Whichever override flag is the highest-priority active one each
// frame becomes the "target"; if it differs from what we're currently
// drawing, we run a 200 ms fade-out then 200 ms fade-in envelope.
//
// Visual envelope (200 ms total per direction):
//   FADE_IN  first 100ms: scene visible, Bayer-black ramps 0→255
//            second 100ms: override drawn, Bayer-black ramps 255→0
//   ON       override drawn at full opacity, no overlay
//   FADE_OUT first 100ms: override drawn, Bayer-black ramps 0→255
//            second 100ms: scene visible, Bayer-black ramps 255→0
//
// The Bayer dither comes from src/scenes/bayer_dither.h — same primitive
// as D.2 fade-through-black. Override scenes paint the entire panel
// (fillScreen(0x0000) + content), so when we delegate to their
// render() we deliberately overwrite whatever the fg layer drew.
//
// Boot caveat (FR-13.1): if splash_active is true on the very first
// frame we render, skip FADE_IN and jump straight to ON. The user
// expects the splash to be on-screen the moment the panel comes up;
// a 200 ms fade-from-clock-scene at boot would be visually wrong.
//
// (added in phase D.3)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "scene_state.h"
#include "scenes/bayer_dither.h"
#include "scenes/layer.h"
#include "scenes/scene.h"

class SafetyOverlayLayer final : public Layer {
public:
  // Override scene pointers in priority order. Caller supplies them
  // because the file-scope Scene instances live in main.cpp; we don't
  // own their storage. nullptr is permitted (treated as "this override
  // is unimplemented") so the layer compiles even if a future variant
  // wants to e.g. omit OFFLINE.
  void bind(Scene* splash, Scene* thermal, Scene* night, Scene* offline) {
    m_scenes[kSlotSplash]   = splash;
    m_scenes[kSlotThermal]  = thermal;
    m_scenes[kSlotNight]    = night;
    m_scenes[kSlotOffline]  = offline;
  }

  const char* name() const override { return "safety_overlay"; }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    Scene* target = pick_target();

    // Boot fast-path (FR-13.1): if splash is engaged on first frame,
    // it must already be on-screen — no fade-from-scene-below.
    if (m_first_call) {
      m_first_call = false;
      if (target != nullptr) {
        m_visible    = target;
        m_state      = State::ON;
        m_phase_started_ms = now_ms;
      }
    }

    advance_state(target, now_ms);
    draw(matrix, now_ms);
  }

private:
  enum class State : uint8_t { OFF, FADE_IN, ON, FADE_OUT };

  enum SlotIdx : uint8_t {
    kSlotSplash  = 0,
    kSlotThermal = 1,
    kSlotNight   = 2,
    kSlotOffline = 3,
    kSlotCount   = 4,
  };

  static constexpr uint32_t kHalfMs  = 100;
  static constexpr uint32_t kTotalMs = 200;

  Scene*   m_scenes[kSlotCount] = {nullptr, nullptr, nullptr, nullptr};
  Scene*   m_visible    = nullptr;
  State    m_state      = State::OFF;
  uint32_t m_phase_started_ms = 0;
  bool     m_first_call = true;

  // Snapshot the four flags under a single mutex acquire and pick the
  // highest-priority active override. Returns nullptr when no override
  // is active.
  Scene* pick_target() {
    bool splash = false, thermal = false, night = false, offline = false;
    scene_state::read_overrides(&splash, &thermal, &night, &offline);
    if (splash)  return m_scenes[kSlotSplash];
    if (thermal) return m_scenes[kSlotThermal];
    if (night)   return m_scenes[kSlotNight];
    if (offline) return m_scenes[kSlotOffline];
    return nullptr;
  }

  void advance_state(Scene* target, uint32_t now_ms) {
    switch (m_state) {
      case State::OFF:
        if (target != nullptr) {
          m_visible          = target;
          m_state            = State::FADE_IN;
          m_phase_started_ms = now_ms;
        }
        break;

      case State::FADE_IN: {
        const uint32_t elapsed = now_ms - m_phase_started_ms;
        if (target == nullptr) {
          // Override yanked mid-fade-in. Reverse to FADE_OUT at the
          // symmetric position so the visual continues smoothly
          // (panel was getting darker; keep darkening, then unfade).
          m_state            = State::FADE_OUT;
          m_phase_started_ms = now_ms - (kTotalMs - elapsed);
        } else if (target != m_visible) {
          // Override changed mid-fade-in (e.g. NIGHT engaged while
          // OFFLINE was fading up). Seamless target switch — alpha is
          // already low so the swap reads as a continuation.
          m_visible = target;
        } else if (elapsed >= kTotalMs) {
          m_state = State::ON;
        }
        break;
      }

      case State::ON:
        if (target == nullptr) {
          m_state            = State::FADE_OUT;
          m_phase_started_ms = now_ms;
        } else if (target != m_visible) {
          // Different override won (e.g. THERMAL preempts NIGHT mid-
          // engagement). Fade out the old one; the FADE_OUT
          // completion handler will fade in the new target.
          m_state            = State::FADE_OUT;
          m_phase_started_ms = now_ms;
        }
        break;

      case State::FADE_OUT: {
        const uint32_t elapsed = now_ms - m_phase_started_ms;
        if (elapsed >= kTotalMs) {
          if (target != nullptr) {
            m_visible          = target;
            m_state            = State::FADE_IN;
            m_phase_started_ms = now_ms;
          } else {
            m_visible = nullptr;
            m_state   = State::OFF;
          }
        }
        break;
      }
    }
  }

  void draw(Adafruit_Protomatter& matrix, uint32_t now_ms) {
    if (m_state == State::OFF || m_visible == nullptr) return;

    bool     draw_override = false;
    uint16_t alpha         = 0;

    switch (m_state) {
      case State::OFF:
        return;
      case State::FADE_IN: {
        const uint32_t elapsed = now_ms - m_phase_started_ms;
        if (elapsed < kHalfMs) {
          draw_override = false;
          alpha = static_cast<uint16_t>((elapsed * 255u) / kHalfMs);
        } else {
          draw_override = true;
          const uint32_t after = elapsed - kHalfMs;
          alpha = 255u - static_cast<uint16_t>((after * 255u) / kHalfMs);
        }
        break;
      }
      case State::ON:
        draw_override = true;
        alpha         = 0;
        break;
      case State::FADE_OUT: {
        const uint32_t elapsed = now_ms - m_phase_started_ms;
        if (elapsed < kHalfMs) {
          draw_override = true;
          alpha = static_cast<uint16_t>((elapsed * 255u) / kHalfMs);
        } else {
          draw_override = false;
          const uint32_t after = elapsed - kHalfMs;
          alpha = 255u - static_cast<uint16_t>((after * 255u) / kHalfMs);
        }
        break;
      }
    }

    if (draw_override && m_visible != nullptr) {
      // Override scenes do fillScreen(0x0000) then paint their
      // content — they completely overwrite whatever the fg layer
      // drew below. Then we Bayer-black on top.
      m_visible->render(matrix, now_ms);
    }

    bayer::apply_black_overlay(matrix, alpha);
  }
};
