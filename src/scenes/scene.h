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

// Intra-scene navigation key, delivered to the focused scene via
// Scene::on_key() (see below). The IR remote's BACK / HOME keys are
// NEVER forwarded — they always exit focus + return to CLOCK at the
// global dispatch layer (src/input/ir_actions.cpp). LEFT / RIGHT lose
// their theme-cycle meaning while a scene is focused; UP / DOWN lose
// their scene-cycle meaning. OK enters focus the first time; subsequent
// presses are forwarded as SceneKey::OK so a scene can step through
// info panels etc.
enum class SceneKey : uint8_t {
  OK    = 0,
  UP    = 1,
  DOWN  = 2,
  LEFT  = 3,
  RIGHT = 4,
};

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

  // Should the theme-level decorations umbrella pass (FRAME_BORDER,
  // SCANLINES, …) draw on top of this scene? Default true so themes
  // get their signature look on every view. Currently overridden only
  // by the night scene, which forces every visible pixel to deep red
  // for dark-adaptation reasons (FR-7.2) — a Blade Runner cyan or
  // LCARS orange frame would defeat that.
  virtual bool wants_theme_decorations() const { return true; }

  // Speculative pre-render hook (FR-16.4, phase D.7). Called by the
  // compositor on Core 1 during slack windows for the most-likely
  // *next* scene (heuristic: incoming scene during the fade-through-
  // black window of a swap; could expand later to default-scene prep
  // during steady-state idle).
  //
  // Contract:
  //   - Idempotent. May be invoked many times before a swap or zero
  //     times if the swap is preempted; both must be safe.
  //   - MUST NOT touch the live framebuffer. The active scene's
  //     render() is still drawing every frame; any matrix.draw* call
  //     here would corrupt the visible output.
  //   - Bounded one-shot work only — palette LUT rebuilds, projection
  //     pre-pack, asset lookup. The hook runs inside Core 1's frame
  //     budget, so a >5 ms prep blows FR-3.1.
  //   - Default = no-op. Scenes that have nothing to amortize leave
  //     it alone; the compositor still calls it harmlessly.
  virtual void prepare(uint32_t /*now_ms*/) {}

  // Intra-scene input hook. Called on Core 1 by the compositor when
  // the IR remote dispatch layer has put this scene in "focused" mode
  // (operator pressed OK on this scene) and a navigation key arrives.
  //
  // Contract:
  //   - Runs on Core 1. Must not block, allocate, or call
  //     matrix.show() — same discipline as render().
  //   - now_ms is the millis() snapshot captured at frame start; use
  //     deltas (wrap-safe), never absolute compares (CODING_PRACTICES §2).
  //   - May safely mutate the scene's own state; the next render()
  //     will pick the change up.
  //   - Default = no-op. Scenes opt in by overriding. Even unhandled
  //     keys are intentionally swallowed (the scene is focused, so the
  //     global UP/DOWN scene cycle / LEFT/RIGHT theme cycle must NOT
  //     run — that's the whole point of focus mode).
  //   - BACK / HOME are NEVER delivered here; the dispatch layer
  //     exits focus on those keys and they fall through to the
  //     existing clear_sticky + return-to-CLOCK path.
  virtual void on_key(SceneKey /*key*/, uint32_t /*now_ms*/) {}
};
