// Scene interface — all visual programs implement this.
//
// Adding a new scene = (a) one new file under src/scenes/ implementing this
// interface, (b) one entry in the dispatcher / registry. Nothing else in
// MQTT, dispatch, or core-split logic should change. (NFR-5.1)
//
// Lifecycle:
//   init(matrix)            called once when the scene becomes active.
//   render(matrix, now_ms)  called every frame while active. now_ms is the
//                           wall clock from millis(); use deltas (wrap-safe)
//                           never compare directly. (CODING_PRACTICES §2)
//
// Implementations MUST NOT allocate dynamically after init(). (NFR-2.2)

#pragma once

#include <stdint.h>

class Adafruit_Protomatter;

class Scene {
public:
  virtual ~Scene() = default;

  // Short stable identifier — used in logs and (eventually) the registry.
  virtual const char* name() const = 0;

  // One-time setup at scene activation. Safe place for static-buffer reset.
  virtual void init(Adafruit_Protomatter& matrix) = 0;

  // Per-frame draw. Must not block, must not allocate, must not call delay().
  //
  // IMPORTANT: scenes MUST NOT call matrix.show() themselves. The main
  // render loop in loop1() draws the scene, layers the shared chrome
  // (clock readout, etc.), then calls show() exactly once per frame.
  // (changed in phase 3.5.2 to satisfy FR-9.3 — chrome cannot be
  // composited if scenes have already pushed the framebuffer.)
  virtual void render(Adafruit_Protomatter& matrix, uint32_t now_ms) = 0;

  // Should the shared HH:MM clock chrome be drawn over this scene? Default
  // true (FR-9.2: every scene shows the time). Override to false only with
  // explicit justification (FR-9.3) — currently just the giant clock
  // scene, which renders its own large HH:MM and would be defaced by an
  // overlapping chrome readout.
  virtual bool wants_clock_chrome() const { return true; }
};
