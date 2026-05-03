---
agent: agent
description: Read REQUIREMENTS.md (source of truth) and PLAN.md, verify alignment, then execute the next unchecked step in the plan.
---

# /continue-work

You are continuing work on the **Quantum Observatory** project. Follow this procedure exactly.

## Step 1 — Load source of truth

Read [docs/REQUIREMENTS.md](../../docs/REQUIREMENTS.md) in full. This is the **single source of truth** for what the system must do. If anything in the plan or code conflicts with requirements, **requirements win**.

Then read [docs/CODING_PRACTICES.md](../../docs/CODING_PRACTICES.md) in full. These rules apply to **every** code change you make in this session.

## Step 2 — Load the plan

Read [docs/PLAN.md](../../docs/PLAN.md) in full.

## Step 3 — Verify plan ↔ requirements alignment

Quickly scan: does every checked-and-unchecked step in the plan still map to a requirement (FR-* or NFR-*) in REQUIREMENTS.md? Are there any requirements with **no** plan step covering them?

- If you find drift (plan step contradicts a requirement, or a requirement is uncovered): **you MAY edit PLAN.md** to add the missing steps, fix wording, reorder phases, or strike obsolete bullets so it once again maps cleanly to REQUIREMENTS.md. Do this *before* picking the next step. Then briefly note the drift fixes in your Step 5 confirmation so the user can sanity-check.
  - You still MUST NOT mark any step `[x]` (or change `[ ]`/`[~]` status of in-flight work) — status updates remain the user's job after a successful build (see Hard Rules).
  - If the drift is large, ambiguous, or could itself contradict a requirement (e.g. requires renumbering completed phases), stop and report instead. Use judgement: small additive fixes → just do them; structural surgery → ask first.
- If aligned: proceed.

## Step 3.5 — Verify plan ↔ source-code alignment (ABORT-on-mismatch)

Before picking the next step, quickly sanity-check that every PLAN.md item still marked `[ ]` (not started) really *isn't* started in the code. For the next 1–2 unchecked items, look for the obvious smoking guns: file/symbol names from the step's "win", new headers under `include/`, new modules under `src/`, references in `main.cpp`, etc.

If you find that an unchecked `[ ]` step looks **already implemented**:

- **STOP. Do not edit PLAN.md, do not start any new code work.**
- Report to the user: which step looks done, what evidence (file links, symbols), and ask whether (a) they forgot to mark it `[x]`, (b) it's a partial implementation that needs finishing, or (c) the evidence is misleading.
- Wait for the user's response before doing anything else. The user is the only authority on `[ ]`→`[x]` transitions (Hard Rules), so guessing wrong here would either skip real work or duplicate finished work.

This check is a cheap heuristic, not a proof. False positives are fine (just ask). False negatives — silently re-implementing something — are the failure mode this step exists to prevent.

## Step 3.6 — Commit any PLAN.md edits before code work begins

If Step 3 caused you to edit PLAN.md (drift fixes only — not checkbox flips, those are forbidden), commit those edits *before* starting any code in Step 6:

```
git add docs/PLAN.md
git commit -m "plan: <one-line summary of the drift fix>"
```

Rationale: PLAN.md edits are doc-only and reversible; bundling them with code in a single commit makes the code diff harder to review and conflates "we changed our mind about scope" with "we built the next thing". Separate commits keep the history honest.

If Step 3 made no PLAN.md edits (the common case), skip this step silently.

If Step 3.5 aborted, you never reach this step.

## Step 4 — Identify the next step

Find the **first** step in PLAN.md marked `[ ]` (not started) or `[~]` (in progress), reading top to bottom. That is your task.

If `[~]` exists, resume it. If only `[ ]` exists, start the earliest one. If everything is `[x]`, report "all phases complete" and stop.

## Step 5 — Confirm the task

Briefly state to the user (1–3 lines):
- The step number and title (e.g., "Phase 1.3 — Scene base class & dispatcher")
- Which requirement(s) it satisfies (e.g., "supports NFR-5.1, FR-3.2")
- Your concrete plan for this step (1–2 sentences)

Do **not** wait for permission unless the step has open decisions or hardware risk — proceed to Step 6.

## Step 6 — Execute the step

Implement the step. **All rules in [CODING_PRACTICES.md](../../docs/CODING_PRACTICES.md) apply.** Key reminders distilled from there and REQUIREMENTS.md:

- **Never** introduce dynamic allocation in render or MQTT loops (NFR-2.2).
- **Never** use software-emulated `float` math in render loops (NFR-1.3).
- Centralize pin/geometry config in `include/config.h` (NFR-5.2).
- Preserve the FM6126A init sequence (FR-8) — never remove it.
- Do not change unrelated files. No drive-by refactors.
- Touch only files needed for the current step.

**Do NOT run the build yourself.** PlatformIO builds in this environment are slow and have repeatedly hung the agent loop. The user will run the build (via the **"Build (pico)"** VS Code task) after you hand off. If the build fails, the user will come back with the errors and you can fix them in a follow-up turn.

## Step 7 — Hand the plan update back to the user

**You MAY edit PLAN.md to keep it aligned with REQUIREMENTS.md** (Step 3 — add missing steps, fix wording, strike obsolete bullets). **You MUST NOT change checkbox status** — marking `[x]` (done), `[~]` (in progress), or `[!]` (blocked) is exclusively the user's job after a successful build. Your job for the *current* step's status is only to *propose* the update in the hand-off (Step 9):

- Quote the exact line from PLAN.md you believe should change, and the proposed new line.
- If you discovered a follow-up that needs its own step, you may insert it as a new sub-bullet (`[ ]`) directly — just call it out in the hand-off so the user sees it.
- If you hit a blocker, propose `[!] <step>` with a one-line note describing what's blocking, but leave the existing `[ ]` in place — the user converts it.

## Step 8 — Capture learnings into CODING_PRACTICES.md

Reflect on the change you just made. Update [docs/CODING_PRACTICES.md](../../docs/CODING_PRACTICES.md) **only if** at least one of the following is true:

- You introduced a **new pattern** other steps will reuse (e.g., a new IPC helper, a buffer convention, a logging tag).
- You hit a **gotcha** that future code could repeat (e.g., a Pico SDK quirk, a Protomatter constraint, a timing pitfall).
- You made a **deliberate trade-off** worth recording (e.g., chose static buffer size N because…).
- An **existing rule turned out to be wrong or incomplete** — fix or refine it.

Rules for editing CODING_PRACTICES.md:

- Add to the most relevant existing section (don't sprawl new top-level sections unless truly novel).
- Keep entries **short** — one-liners or short bullets. This file is read every session; brevity matters.
- Cite the phase that introduced the pattern, e.g. `(added in phase 1.3)`.
- If nothing qualifies, **make no change** — do not pad the doc.
- Do not weaken rules without user approval. Strengthening or clarifying is fine.

## Step 9 — Hand off

End your turn with:
- A one-sentence summary of what should now work once built (the "small win").
- A reminder that the **user needs to run the build** (via the "Build (pico)" task) and then mark the PLAN.md step accordingly.
- The proposed PLAN.md edit from Step 7 (old line → new line, plus any new sub-bullets).
- The exact suggested commit message: `phase X.Y: <what works now>` (per PLAN.md "Working Rhythm").
- If you updated CODING_PRACTICES.md, mention which section and why in one line.
- The next `[ ]` step the user will tackle next time (just the title, no action).
- **If applicable:** a one-line "refactor sniff" — see Step 10.

## Step 10 — Occasional refactor / cleanup nudge

As the codebase grows, watch for *structural* smells that no single phase will fix on its own. **Once every ~5 completed phases**, or whenever you notice one of the symptoms below, raise it in the hand-off as a one-line nudge — do NOT act on it inside `/continue-work`.

Symptoms worth flagging:
- A file has grown beyond ~250 lines and is doing more than one thing (e.g. `main.cpp` accumulating both core-orchestration and per-feature wiring).
- A pattern has been copy-pasted ≥ 3 times across files (candidate for a helper).
- A `Scene` / module is reaching across more globals than its peers (coupling drift).
- The same `[[maybe_unused]]` workaround appears in multiple files (suggests a missing registry).
- A header is included by ≥ 5 translation units but only 1–2 use most of its symbols (split candidate).
- New phases keep needing to edit the same `switch`/dispatch site (consider data-driven dispatch).

When you flag one, suggest the user invoke `/cleanup` (for cruft) or open a dedicated refactor session — `/continue-work` is the wrong vehicle because:
- Refactors touch many files; this prompt is one-step-per-session.
- Refactors don't map to a PLAN.md `[ ]` checkbox.
- Refactor risk needs explicit user buy-in, not the implicit "proceed to Step 6" flow.

If nothing structural is sniffed, say nothing — don't pad the hand-off.

Then call `task_complete`.

---

## Hard rules

- **Source of truth ordering:** REQUIREMENTS.md > PLAN.md > existing code. Never modify REQUIREMENTS.md as part of `/continue-work` unless the user explicitly asks.
- **One step per invocation.** Do not greedily knock out multiple steps in one run — small wins keep momentum.
- **No new markdown docs** unless the step explicitly calls for one.
- **No tests** unless a step explicitly calls for them (this is a hobby/hardware project, build success is the gate).
- **The agent never runs the build and never changes PLAN.md checkbox status.** Builds and `[ ]`→`[x]` (or `[!]`) transitions are the user's job at hand-off. The agent MAY edit PLAN.md *content* (add/reword/reorder/strike bullets) when needed to keep it aligned with REQUIREMENTS.md — see Step 3 and Step 7.
- **The agent MAY commit `docs/PLAN.md` content edits in Step 3.6** (drift fixes only, never checkbox flips). All other commits are the user's job at hand-off.
- **If Step 3.5 finds a `[ ]` step that looks already implemented, ABORT.** Do not start any new code work, do not edit PLAN.md. Report and wait for the user to disambiguate.
- If the next step requires hardware verification (e.g., "see twinkling sky"), say so explicitly in the hand-off so the user knows to flash and confirm before marking `[x]`.
