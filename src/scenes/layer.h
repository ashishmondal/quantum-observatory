// Layer — Core 1's per-frame compositor primitive (FR-16.1).
//
// The legacy render path called a single Scene::render() then conditionally
// drew chrome. As of Phase D.1 the loop1() body instead walks a fixed-size
// `Layer*` array — `[bg, fg, overlay0, overlay1, chrome]` — composing the
// frame top-down by `render()` order. Each slot may be null for "skip".
//
// Why an interface separate from Scene:
//   - Scenes own their lifecycle (init/render). Layers own only per-frame
//     drawing — they're cheaper, optional, and may come and go without a
//     scene swap (e.g. crossfade D.2, safety overrides D.3, toasts D.8).
//   - Lets Core 1 add ambient compositor work (sky-snapshot driven chrome
//     micro-indicators, FR-16.5/16.8) without touching every Scene.
//
// Concurrency: Layers run on Core 1 only. Any cross-core data they consume
// must use the patterns in CODING_PRACTICES §3 (seqlock for read-mostly,
// take_*() for edge events, mutex_t for multi-field).
//
// Memory: same NFR-2.2 rules as Scene — no dynamic allocation after init.

#pragma once

#include <stdint.h>

class Adafruit_Protomatter;

class Layer {
public:
  virtual ~Layer() = default;

  // Short stable identifier — used in logs and the D.10 per-layer timing
  // table. Keep ≤ 12 chars so the timing dump stays one line.
  virtual const char* name() const = 0;

  // Per-frame draw. Same rules as Scene::render — no show(), no block,
  // no alloc, no delay. Must tolerate being called regardless of whether
  // a previous layer has drawn (it will, in normal compositor order).
  virtual void render(Adafruit_Protomatter& matrix, uint32_t now_ms) = 0;

  // Optional speculative pre-render hook (FR-16.4). Default no-op.
  // Called from Core 1 idle slack (D.7) on the *likely next* layer
  // before it becomes active. Idempotent; MUST NOT touch the live
  // framebuffer. Scenes that benefit (constellation line packing,
  // image palette LUT rebuilds) override; everything else inherits
  // the no-op.
  virtual void prepare(uint32_t /*now_ms*/) {}
};
