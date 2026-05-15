// Fixed-point math helpers for the render loop.
//
// Why this exists: RP2040 has no FPU. Every `float` op is software-emulated
// and tanks frame rate. Render code MUST stay integer-only. (NFR-1.3,
// CODING_PRACTICES §2)
//
// Format: Q8.8 stored in int16_t.
//   - 8 integer bits (signed) → range [-128, +127]
//   - 8 fractional bits      → resolution 1/256 ≈ 0.0039
//   - Multiply uses an int32_t intermediate to avoid overflow.
//
// Trig: a 256-entry LUT covers a full turn (so a "fixed-angle" is just a
// uint8_t index — wraps for free at 256). LUT amplitude is Q8.8 with
// |sin| ≤ 1.0 → values in [-256, +256]. Populated once via sin_cos_lut_init()
// in setup() (float at boot is fine; the LUT itself is integer at runtime).
//
// (added in phase 2.1)

#pragma once

#include <stdint.h>

namespace fp {

// ---- Q8.8 fundamentals ---------------------------------------------------

using q8 = int16_t;
constexpr int Q8_SHIFT = 8;
constexpr int32_t Q8_ONE = 1 << Q8_SHIFT;          // 256

// Construct from a plain int (e.g. 3 → 3.0 in Q8.8).
constexpr q8 from_int(int16_t v) {
  return static_cast<q8>(v << Q8_SHIFT);
}

// Truncating integer extraction (round toward zero behaves like (int)v).
constexpr int16_t to_int(q8 v) {
  return static_cast<int16_t>(v >> Q8_SHIFT);
}

// Setup-time only: float → Q8.8 conversion. Do NOT call from render loops.
constexpr q8 from_float(float v) {
  return static_cast<q8>(v * Q8_ONE);
}

// Q8.8 multiply. Promotes to int32 to keep all 16 fraction bits during the
// product, then shifts back. Saturating wrap is the caller's problem.
constexpr q8 mul(q8 a, q8 b) {
  return static_cast<q8>((static_cast<int32_t>(a) * b) >> Q8_SHIFT);
}

// Add/sub are just integer ops; provided for symmetry / readability.
constexpr q8 add(q8 a, q8 b) { return static_cast<q8>(a + b); }
constexpr q8 sub(q8 a, q8 b) { return static_cast<q8>(a - b); }

// ---- 256-entry sin/cos LUT ----------------------------------------------
//
// Angle is a uint8_t index: 0..255 spans a full turn. Wraps automatically.
// Amplitude is Q8.8: sin/cos values in [-256, +256] (i.e. ±1.0).

constexpr int LUT_SIZE = 256;

// Defined in fixed_point.cpp — populated by sin_cos_lut_init().
extern int16_t g_sin_lut[LUT_SIZE];

// Call once during setup() before any sin_q8/cos_q8 use.
void sin_cos_lut_init();

inline int16_t sin_q8(uint8_t angle) {
  return g_sin_lut[angle];
}

inline int16_t cos_q8(uint8_t angle) {
  // cos(x) = sin(x + 90°); 90° = LUT_SIZE/4 = 64. uint8_t wrap handles it.
  return g_sin_lut[static_cast<uint8_t>(angle + 64)];
}

}  // namespace fp
