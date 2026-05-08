// FadeBlackLayer — D.2 scene transition (FR-16.3).
//
// When loop1() detects a pending scene swap, instead of swapping
// g_current_scene immediately we activate this layer. It runs a
// 250 ms fade-through-black envelope on top of whatever the active
// scene + chrome layered into the framebuffer:
//
//   t in [0,    125ms)  outgoing scene fades to black (alpha 0 → 255)
//   t == 125ms          loop1() swaps g_current_scene (panel is fully
//                       black at peak alpha, so the swap is invisible)
//   t in (125ms, 250ms] incoming scene fades up from black (alpha → 0)
//
// Rendering is an 8×8 ordered Bayer dither: the layer walks the panel
// and over-writes pixels with 0x0000 wherever bayer[y%8][x%8] < alpha.
// At alpha=0 nothing is black (no-op pass); at alpha=255 every pixel
// is black (Bayer max value is 252 in 0..255 scaling). No framebuffer
// readback, no off-screen scratch buffer, no Scene::render signature
// change — the layer just stamps black on top.
//
// Rationale for the dither over a per-pixel scaled-down RGB565:
//   - Adafruit_Protomatter provides no getPixel() readback. A scaled
//     fade would need either a 4 KB mirror buffer (and a ~25-file
//     signature refactor to feed it) or a getPixel patch into the
//     library. The Bayer dither is signature-free, allocation-free,
//     and the resulting "shrinking dot pattern" matches the panel's
//     retro pixel-art aesthetic better than a true crossfade would.
//
// (added in phase D.2)

#pragma once

#include <stdint.h>

#include <Adafruit_Protomatter.h>

#include "config.h"
#include "scenes/bayer_dither.h"
#include "scenes/layer.h"

class FadeBlackLayer final : public Layer {
public:
  // Activate the envelope. Called from loop1() when take_pending()
  // returns a new SceneId different from g_current_scene. Idempotent —
  // calling start() while already active restarts the envelope (so a
  // rapid burst of scene swaps re-fades each time rather than getting
  // stuck mid-transition).
  void start(uint32_t now_ms) {
    m_active      = true;
    m_swap_done   = false;
    m_started_ms  = now_ms;
  }

  // Total + half durations. Public so loop1() can match the contract
  // in its own logging without re-declaring constants.
  static constexpr uint32_t kHalfMs  = 125;  // outgoing fade-out
  static constexpr uint32_t kTotalMs = 250;  // outgoing + incoming

  bool active() const { return m_active; }

  // Caller polls this once per frame while active(). Returns true
  // EXACTLY ONCE per envelope, at the midpoint — that's the cue to
  // swap g_current_scene + run the new scene's init() while the
  // panel is fully black. After this returns true once, subsequent
  // calls in the same envelope return false.
  bool ready_to_swap(uint32_t now_ms) {
    if (!m_active || m_swap_done) return false;
    if (static_cast<int32_t>(now_ms - m_started_ms) >= static_cast<int32_t>(kHalfMs)) {
      m_swap_done = true;
      return true;
    }
    return false;
  }

  const char* name() const override { return "fade_black"; }

  void render(Adafruit_Protomatter& matrix, uint32_t now_ms) override {
    if (!m_active) return;

    const uint32_t elapsed = now_ms - m_started_ms;
    if (elapsed >= kTotalMs) {
      // Envelope complete — incoming scene is fully visible.
      m_active = false;
      return;
    }

    // Triangular alpha envelope, integer math (NFR-1.3):
    //   first half  [0, kHalfMs)    — alpha ramps 0   → 255 (fade out)
    //   second half [kHalfMs, kTotalMs) — alpha ramps 255 → 0 (fade in)
    // Peaks at exactly kHalfMs (panel fully black for 1 frame).
    uint16_t alpha;
    if (elapsed < kHalfMs) {
      alpha = static_cast<uint16_t>((elapsed * 255u) / kHalfMs);
    } else {
      const uint32_t after = elapsed - kHalfMs;
      alpha = 255u - static_cast<uint16_t>((after * 255u) / kHalfMs);
    }
    if (alpha > 255u) alpha = 255u;

    // alpha=0 is a no-op pass. Skip the per-pixel walk to keep idle
    // frames cheap during the very edges of the envelope.
    if (alpha == 0u) return;

    // Shared Bayer-dither helper (lifted out in phase D.3 so the
    // safety overlay can reuse the same primitive).
    bayer::apply_black_overlay(matrix, alpha);
  }

private:
  bool     m_active     = false;
  bool     m_swap_done  = false;
  uint32_t m_started_ms = 0;
};
