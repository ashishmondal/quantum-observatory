---
agent: agent
description: Read REQUIREMENTS.md, PLAN.md, and CODING_PRACTICES.md, then sweep the codebase for stale artifacts (debug pokes, demo toggles, obsolete comments, dead helpers, drifted docstrings) and remove or update them. Conservative — proposes a plan first, executes after confirmation.
---

# /cleanup

You are doing a **codebase hygiene pass** on the **Quantum Observatory**. Goal: remove cruft that has accumulated as phases land — temporary pokes, demo toggles, "phase X.Y debug" prints, comments that describe a previous architecture, dead `[[maybe_unused]]` instances, etc. — without changing behaviour the current PLAN/REQUIREMENTS expect.

This is **not** a refactor. It's a deletion / wording-update pass.

## Step 1 — Load source of truth

Read in full:
- [docs/REQUIREMENTS.md](../../docs/REQUIREMENTS.md) — what the system must do.
- [docs/PLAN.md](../../docs/PLAN.md) — what's checked done (`[x]`), in progress (`[~]`), or future (`[ ]`/`[!]`).
- [docs/CODING_PRACTICES.md](../../docs/CODING_PRACTICES.md) — what counts as a "rule".

Build a mental model of which features are *current truth* and which are *historical scaffolding*.

## Step 2 — Scan for cleanup candidates

Walk the workspace (skip `.pio/`, `.git/`, build artifacts) and look for these specific categories. Cite each candidate with a file link and 1-line justification.

### A. Debug / bootstrap code that has been superseded
- Hardcoded "fake" values poked in `setup()` (epochs, sensor readings, scene_ids) where the real source has since landed.
- One-shot `Serial.print` lines marked as a phase "win" whose phase is now `[x]` and whose info is no longer interesting.
- `#ifdef`-gated bootstraps (e.g. `RTC_SEED_LOCAL_EPOCH`) — keep the gate but check the surrounding comment still matches reality.
- Demo toggles in `loop()` / `loop1()` that exist only to prove an IPC path which a later phase has replaced (e.g. a 5-second scene swap that 5.4 will replace with MQTT-driven swaps).

### B. Comments that describe an older architecture
- "Phase 5 will replace this with…" where Phase 5 has already landed.
- "For now we…" / "Until 4.2 lands…" where the referenced phase is `[x]`.
- Pin or peripheral comments that contradict `include/config.h`.
- Doc comments at the top of a file describing a different responsibility than the code currently has.

### C. Dead or one-use helpers
- `[[maybe_unused]]` symbols whose original consumer is gone.
- File-static helpers called from exactly one place where inlining would be clearer (only flag; don't auto-inline unless trivial).
- Unused `#include`s that are no longer pulled in by any symbol used in the file.

### D. Drifted documentation inside source files
- Function comments that promise behaviour the function no longer has (e.g. "returns false on unknown id" when the function now returns nullptr).
- TODO comments that reference a phase number that's now done.
- "(added in phase X.Y)" tags on code that has since been substantially rewritten — re-tag to the rewriting phase or drop the tag.

### E. PLAN.md / REQUIREMENTS.md hygiene (read-only here)
You MAY note (but MUST NOT silently fix) drift between the docs themselves. The `/continue-work` prompt owns those edits. Just list them in your report.

## Step 3 — Present the cleanup plan

Before touching any file, output a numbered list to the user:

```
1. <file:line> — <what to remove/update> — <why> (justification cites FR-x or PLAN phase status)
2. ...
```

Group by category (A/B/C/D). For each item state whether it's a **delete**, **reword**, or **keep-with-note** action. If anything looks borderline (e.g. a debug print that might still be useful), default to **keep-with-note** and ask the user.

If the list is empty: say so and stop. No change needed.

## Step 4 — Confirm with the user

Ask: "Proceed with items [N..M] / all / a subset?" Wait for an explicit response.

Do NOT run the build. Do NOT make any edits before getting confirmation, even if you're certain.

## Step 5 — Execute approved items

Apply the approved deletions / rewordings. **Rules of engagement:**

- **One edit per concern.** Don't bundle a deletion with an unrelated reword — keeps the diff readable.
- **Preserve all FR-mandated behaviour.** If removing something would touch FR-8 (FM6126A init), FR-9.5 (RTC reads), or FR-7 (LDR/thermal sensor reads + safety scene swaps), STOP and report — these are load-bearing even if they "look like cruft".
- **Never remove a `[[maybe_unused]]` static instance just because nothing references it** — many of those exist to keep linker symbols alive for future phases. Confirm with the user case-by-case.
- **No drive-by formatting changes.** Whitespace, brace style, reordering of unrelated lines: out of scope.
- **No test changes** unless the test itself is dead.
- **No CODING_PRACTICES.md edits** unless cleanup revealed an outdated rule (e.g. removed a pattern the rule references).

For each edit, prefer the smallest possible patch. Removing 5 lines is fine; rewriting a 30-line function "while you're there" is not.

## Step 6 — Hand off

End your turn with:

- A short summary of what was deleted / reworded, grouped by file.
- Any items the user *declined* — note them so they're not re-flagged next sweep.
- Any drift between PLAN.md and REQUIREMENTS.md you noticed in Step 2.E (for `/continue-work` to address later).
- A reminder that **the user runs the build** (Build (pico) task) to confirm nothing regressed.
- Suggested commit message: `cleanup: <one-line summary>` (NOT `phase X.Y: …` — this is a hygiene pass, not a phase deliverable).

Then call `task_complete`.

---

## Hard rules

- **Source of truth ordering:** REQUIREMENTS.md > CODING_PRACTICES.md > PLAN.md > existing code. If code conflicts with requirements, the code is the cruft, not the requirement.
- **Conservative bias.** When in doubt, keep. The cost of a bad deletion (lost context, broken FR-mandated behaviour) is much higher than the cost of carrying a slightly stale comment for one more session.
- **Never touch PLAN.md checkbox status.** Period — that's the user's job, same rule as `/continue-work`.
- **Never edit REQUIREMENTS.md.** Same as `/continue-work`.
- **No new files.** This prompt only deletes and rewords.
- **No behavioural changes** beyond removing dead paths. If a deletion would alter what the device does on boot or under any FR, it's out of scope — report it and let the user decide.
- **Build is the gate.** You do not run it; the user does. If anything fails to compile after cleanup, the user comes back with errors and you fix in a follow-up.
