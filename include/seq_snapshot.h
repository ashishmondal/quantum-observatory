// Single-producer / single-consumer seqlock for cross-core, read-mostly
// state. Generalizes the pattern already proven for `g_render_alive_ms`
// and `g_render_fps` (CODING_PRACTICES §3) to multi-field snapshots.
//
// FR-16.7: Core 1's per-frame read path SHALL NOT block on Core 0's
// network jitter. The classic solution is a seqlock — writer increments
// an odd seq before mutating the snapshot, then increments to even
// after; reader reads seq, snapshot, seq again, retries on mismatch.
// No mutex is acquired on the reader side, so a slow Core 0 writer
// never stalls the renderer.
//
// Constraints baked into this template:
//
//   - Single producer. Writer-writer coordination on Core 0 (e.g. the
//     MQTT callback racing scene_state::tick()) is the caller's job;
//     this template assumes one core has exclusive write access. We
//     keep scene_state's `mutex_t` for that purpose; the seqlock is
//     purely the Core 1 → Core 0 read fence.
//
//   - T must be a trivially-copyable value type (POD). The data slot
//     is copied byte-for-byte on every read — no virtual dtors, no
//     pointers into transient state, no embedded mutex_t.
//
//   - Reader spins on `tight_loop_contents()` while a write is in
//     progress (odd seq). On RP2040 a writer can't preempt itself,
//     and the read-side spin is bounded by the writer's snapshot
//     copy time (a few word stores), so the worst-case stall on Core
//     1 is microseconds — well below the kFrameIntervalMs budget.
//
//   - `__atomic_thread_fence(__ATOMIC_RELEASE/ACQUIRE)` flank the data
//     copy on both sides so the compiler can't reorder the seq write
//     across the payload write. The seq counter itself uses the
//     stricter `__atomic_store_n` / `__atomic_load_n` so different
//     translation units see consistent ordering. (RP2040 has no
//     hardware reordering between cores beyond the cache, but the
//     compiler does — and the SDK precedent of plain `volatile` for
//     single-word telemetry doesn't generalise to structs.)
//
// Usage:
//
//   struct Snapshot { ... };
//   static SeqSnapshot<Snapshot> s_pub;
//
//   // Core 0 (writer, after mutating internal state):
//   Snapshot snap = build_from(state);
//   s_pub.publish(snap);
//
//   // Core 1 (reader, no mutex acquire):
//   Snapshot snap;
//   uint32_t seen = s_pub.read(&snap);   // returns version observed
//
// (added in phase D.4 — generalises the seqlock pattern from FR-16.7)

#pragma once

#include <stdint.h>

#include <pico/platform.h>  // tight_loop_contents()

template <typename T>
class SeqSnapshot {
 public:
  SeqSnapshot() : m_data{} {}

  // Writer (Core 0). Atomically replaces the published snapshot. Cheap
  // — even seq → odd → copy → even — only the SPSC writer side calls
  // this, so no inter-writer ordering needed beyond what the caller
  // already has (scene_state's mutex_t).
  void publish(const T& v) {
    const uint32_t s = __atomic_load_n(&m_seq, __ATOMIC_RELAXED) + 1u;
    __atomic_store_n(&m_seq, s, __ATOMIC_RELAXED);   // odd: write in flight
    __atomic_thread_fence(__ATOMIC_RELEASE);
    m_data = v;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&m_seq, s + 1u, __ATOMIC_RELEASE);  // even: stable
  }

  // Reader (Core 1). Spins until a torn-free read of the snapshot is
  // observed. Returns the seq number that was consistently visible —
  // callers that need "exactly once per change" semantics keep their
  // own `last_seen` and act on advancement.
  uint32_t read(T* out) const {
    uint32_t s1, s2;
    do {
      s1 = __atomic_load_n(&m_seq, __ATOMIC_ACQUIRE);
      if (s1 & 1u) {                      // writer in flight
        tight_loop_contents();
        continue;
      }
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      *out = m_data;
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      s2 = __atomic_load_n(&m_seq, __ATOMIC_ACQUIRE);
    } while (s1 != s2);
    return s1;
  }

 private:
  // Seq layout: even = stable, odd = writer mid-update. Initial value 0
  // means "no publish yet"; readers must treat seq==0 as a special
  // "stale" case if they care (scene_state seeds an initial publish in
  // init(), so this never happens at runtime).
  mutable uint32_t m_seq = 0;
  T m_data;
};
