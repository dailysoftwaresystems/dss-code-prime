# The fail-loud gate + the cross-plan update

### Step 6 — Fail-loud gate
This is the canonical gate checklist (§A.6 is its one-line statement). Verify every item:
- `cmake --build build` clean (no link errors).
- `ctest --test-dir build --output-on-failure` 100%, including the new tests.
- anchor-registry guard OK: `tools/check-anchor-registry.ps1` (or `.sh`).
- agnosticism scan clean (no hardcoded language/CPU/format in shared substrate).
- CI-hazard screen clean (from Step 5): no GCC-vs-MSVC portability traps. Local green ≠ CI green.
- review folded clean.
- **§A.7 issue-anchoring — nothing worked around.** Every issue this cycle *came across* — including
  out-of-scope / later-cycle ones — is ANCHORED in the registry (this commit) **and** handled (fixed
  now, or pinned as a deferred anchor with a trigger). If you excluded, disabled, skipped, or
  "green-modulo"-ed ANYTHING to reach green, it MUST carry an anchor + a proper fix-or-defer decision;
  a silent workaround is a gate failure, not a pass.
- **★★★ THE ANCHOR BALANCE GATE — COUNTABLE, AND IT FAILS THE CYCLE.**
  **A cycle MUST NOT end with more OPEN rows than it started.** Not a target, not a trend to watch:
  a gate item with a number, checked exactly like ctest. Count before and after and report both:

  ```bash
  python tools/check-anchor-balance.py
  ```

  It prints OPEN-at-base, OPEN-now, and **the name of every row that opened or closed** — the count
  alone cannot tell you which, and "which" is the question. Exit 1 when the cycle leaves more open
  than it found.

  `after > before` ⇒ **the gate FAILS.** Close the difference, or escalate the one you cannot close
  as a **§B decision** — the user chooses to carry it; the cycle does not decide that for itself.

  ⚠ **DO NOT go back to an inline `grep -c` over a list of status glyphs.** That was the first
  version of this gate and it was wrong in the direction that flatters the cycle: it enumerated
  `🔴|🟠|⚠` and was blind to `⏳`, so it reported **269** open rows where there were **579**, and a
  cycle that closed one 🟠 row while opening one ⏳ row would have been congratulated for an
  improvement. ✔MEASURED 2026-08-11, on the very cycle that introduced the gate — the row it could
  not see was `D-OPT6-LICM-SPECULATIVE-LOAD-HOIST`, a HIGH miscompile. The tool inverts the rule:
  **a row is OPEN unless its status cell carries an explicit `✅`**, so a glyph nobody has thought of
  yet counts as open, which is the safe direction. `--self-test` pins that inversion (including a
  deliberately novel glyph); run it if you touch the script. Enumerating the open glyphs is the same
  mistake as enumerating build-directory layouts — define the complement, not the variants.
  Opening rows is fine and often right; ENDING ON A HIGHER NUMBER is what is forbidden, because that
  is the arithmetic by which a 3,000-row audit trail became a 350-row backlog nobody reads.

  ★★★ **THE DENOMINATOR IS `registry + plans`, NOT THE REGISTRY ALONE — widened 2026-08-13, and
  finding this was the THIRD and FOURTH times this one instrument under-counted.** §F.2 sanctions
  **two homes** for an anchor (this registry AND the owning plan's deferral table) and §F.4 lets a
  `src/` citation resolve to either — but the tool counted only registry rows. ⇒ **a cycle that
  closed a registry row and deferred the work into a plan row was reported as an IMPROVEMENT.**
  Both homes are now counted, so MOVING a deferral between them is arithmetically NEUTRAL.
  - **✔MEASURED the day it was fixed: `661 → 662 → 987`**, decomposing with no residue. The
    registry-only number was itself wrong (**+2 −1**): the row regex `^\| \`(D-[A-Z0-9-]+)\` \|`
    admitted no `_`, so **two OPEN rows were INVISIBLE** (`D-TEST-QEMU_LD_PREFIX-AMBIENT-ONLY`,
    `D-TEST-CORPUS-NO-QEMU-X86_64-ON-ARM64-HOST`) — ★ **a row the gate cannot SEE cannot be seen
    to OPEN either** — and one row it counted lives in the registry's own **“Allowlist (code-internal
    pins, NOT deferrals)”** table. The remaining **+325** is the plan side (231 deferred-items, 84
    reserved, 10 registry-shaped).
  - **No stored baseline exists and none is needed:** both sides of the comparison use the same rule,
    so widening moves the HEADLINE, never the DELTA. Cross-checked at `--base HEAD~1`, which
    reproduces the recorded asm-arm64 **+20** exactly. `--denominator registry` reproduces the
    pre-2026-08-13 headline on demand, and the output always NAMES which denominator gated.
  - ⚠⚠ **RECOGNITION IS BY COLUMN SHAPE, NEVER BY HEADING NUMBER — “§3.1” is NOT the contract, and
    assuming it was would have missed most of the tables.** ✔MEASURED: `17-shader-gpu-plan` keeps its
    anchor table at **§5.4** while its §3.1 is a `Tier | Example` prose table; `23-full-c-plan`'s §3.1
    is **not a table at all**; `09.5`/`24`/`28` use §9/§6/§12 and `08` uses §2.5–§2.8. Three anchor
    shapes are counted (the registry's 4-column; `# | Deferred item |` with **five** different tails;
    and `Anchor | Owns`, which has **no status column** so every row is unconditionally OPEN); four
    non-deferral shapes are excluded BY NAME. Row inclusion is decided **by table, never by how an
    anchor is spelled** — which is what makes the underscore blind spot unrepeatable by construction.
  - **Severity rule, and it is not a softening: FATAL iff the measurement is INCOMPLETE.** An
    unrecognized table shape or an orphan row means rows exist that could not be counted → exit 1. A
    merely *interrupted* table (e.g. an inline HTML comment parked mid-body, which severs the rows
    below it from their header) loses nothing once the reader steps over it → loud WARN. Never
    silently skip a table: a silently skipped table is the exact defect this whole rule exists about.
  - `--self-test` covers **32** cases including a deliberately novel glyph in a plan row, a mid-prose
    `✅` that must still count OPEN, a `✅` in a non-status column, an underscore name, a strikethrough
    name, an orphan row, and a mid-table comment. Run it if you touch the script.

  ★★ **THE FAILURE MODE THIS EXISTS TO KILL, ✔MEASURED 2026-08-11 and it is not subtle:** a lane was
  dispatched to FIX the predefined-macro set. When it was stopped it had written **nine new OPEN rows
  describing predefined-macro gaps** — `D-PP-COMPILER-IDENTITY-INCOHERENT`,
  `D-PP-IMPL-DETAIL-PREDEFINES-ABSENT`, `D-PP-CODE-MODEL-PREDEFINES` and six more — i.e. it had
  converted its own assignment into nine reasons to do it later, each one honestly written and
  correctly cross-referenced. **A row that restates the task you were given is not documentation, it
  is the task not being done.** Before writing ANY row, answer in one sentence: *is this the work I
  was sent to do?* If yes, the row is forbidden and the work is mandatory. That cycle's 22 committed
  rows over five commits are the same arithmetic at a slower rate.
- **★ FOUR ROWS THAT LOOK DIFFERENT AND ARE THE SAME ROW.** Before appending, grep the registry for
  the SYMPTOM, the ARTEFACT and the FILE NAMES in your evidence — not the title you have in mind
  (§C.-1 1b). Nine rows about one absent macro family is one row, or better, one fix.

**Any red the cycle cannot self-repair → STOP and report the blocker. Do not push broken.**
Better to wake the user to "stopped at step N, here is the blocker" than to push something
subtly wrong.

### Step 8 — Cross-plan update
Keep the plans honest in the **same commit** as the code:
- Update plan 00 §0 status table + §0.1 stepper row (flip status, update ctest count).
- Update the owning sub-plan: flip the §0 status row AND stamp the §3.1 deferred-items row
  (status flip in §0; `✅ CLOSED` stamp in §3.1 — update both, not one).
- In `_deferred-anchor-registry.md`: mark closed anchors `✅ CLOSED <date>` with the commit;
  **never delete a row** (the audit trail is load-bearing); add new anchors.
- Record the cycle in the running cycle-log (memory entry per the established convention).
- Update the `dss-code-prime` skill if a convention changed.
- **★★★ Update the HANDOFF (below). MANDATORY — the cycle does not reach Step 9 without it.**

#### Step 8.1 — THE HANDOFF DOCUMENT (`.plans/_handoff.md`) — MANDATORY EVERY CYCLE
✔**Operator instruction, 2026-08-13:** *"create and keep a handoff document that must be updated
at the end of each cycle … must be in `.plans` and must have: where we are and where we need to
get, along with our defined priorities."*

**Path: `.plans/_handoff.md`.** One file, rewritten in place each cycle — never a per-cycle copy,
never a dated sibling. If it does not exist, **create it this cycle**.

It answers exactly five questions, in this order, and nothing else:

1. **WHERE WE ARE** — the true present state. Branch + HEAD, the gate result *with its numbers*
   (ctest counts, anchor balance before→after), which legs actually ran and **which did not**, and
   what shipped this cycle. State capability in terms of what has been *witnessed by execution*,
   not what was implemented.
2. **WHERE WE NEED TO GET** — the destination, and the named gap between here and it. Not a wish
   list: each entry says what is missing and what would close it.
3. **PRIORITIES** — ordered, with the reason for the order. Mark each as `NEXT` / `QUEUED` /
   `BLOCKED (on X)` / `OPERATOR DECISION`. An item nobody can start is not a priority; it is a
   blocker, and it says so.
4. **CONCURRENT BRANCHES / PRs — see Step 8.2.**
5. **TIMELINE — see Step 8.3.**

#### Step 8.2 — CONCURRENT BRANCHES / PRs (the rebase-conflict surface)
✔**Operator instruction, 2026-08-13:** *"in case of concurrent branches/PRs, let the handoff also
know the branches actions/plans/priorities in separated sessions so it never conflicts when
rebasing."*

**Other sessions are working other branches on this same repo, and this cycle cannot see them.**
The handoff is the only channel between them, so it carries — **measured, not remembered**:

- `gh pr list --state open` → every open PR: number, title, head branch, last-updated. **Say which
  one is THIS branch**; a reader who cannot tell ours from theirs will rebase the wrong way.
- For each *other* active PR: `gh pr view <n> --json files` → the **overlap set** with what this
  cycle touched. Name the individual files, not a count — "59 files" tells a rebaser nothing;
  `.plans/_deferred-anchor-registry.md`, `src/link/format/macho.cpp` tells them everything.
- **What that branch is DOING** — its goal in one line, so its edits are interpretable rather than
  merely conflicting. A conflict you understand is a merge; one you do not is a coin flip.
- ⚠ **Call out the known-hot files explicitly.** `_deferred-anchor-registry.md` is edited by every
  session every cycle and is the most likely conflict in the repo. Plan 00 and the shared
  `CMakeLists.txt`/`parse_diagnostic.hpp` slot tables are next.
- 📄 **Restate the staging rule**, because it is the mitigation: **stage by explicit path, never
  `git add -A`** (`D-CYCLE-CANNOT-ASSUME-IT-OWNS-THE-WORKING-TREE`). A concurrent workstream's
  edits can be sitting in this very working tree.
- ⚠ **Diagnostic-code slots and anchor names are cross-branch resources.** Two sessions taking the
  same `K_*` slot or minting the same `D-*` name conflict *semantically* — git merges both cleanly
  and the result is wrong. Record the next-free slot this cycle consumed.

#### Step 8.3 — TIMELINE
✔**Operator instruction, 2026-08-13:** *"also keep a timeline please."*

A dated, newest-first list of what actually landed, one line per cycle: date · commit · what
shipped · the gate numbers · **which legs ran**. This is the one part of the handoff that
**accumulates rather than being replaced** — it is the project's memory of its own arc, and it is
what makes "two cycles running shipped on one leg" visible as a pattern instead of a surprise.

- Newest first. New cycles are prepended, never appended.
- One line each. If a cycle needs a paragraph, it needs a plan file, not a timeline row.
- **Include the cycles that did NOT go well** — a paused cycle, a reverted commit, a leg that was
  skipped. A timeline that records only successes is a marketing document and will be read as one.
- Trim only when it stops earning its length: keep every entry back to the current major arc, then
  compress older ones to one line per *month* rather than deleting them.

#### Step 8.4 — THE RULES THAT KEEP THE HANDOFF WORTH READING (all five sections)
- **Sections 1–4 are a REPLACEMENT, not a journal.** Stale lines are deleted, not appended past.
  A handoff whose *state* sections grow monotonically is a changelog wearing the wrong name, and
  nobody reads to the bottom of it. **§5 TIMELINE is the sole exception and accumulates by
  design** (Step 8.3) — that asymmetry is deliberate: state is what is true now, timeline is how
  it got here, and merging the two loses both.
- **Every claim carries MEASURED / DOCUMENTED / INFERRED**, per §C.-1. A handoff is read by
  someone with no context, which is precisely when an unlabelled inference does the most damage.
- **Name what is NOT known and NOT run.** "WSL and native-arm64 legs not run" belongs in the
  handoff louder than anything that passed. The reader's first question is what to trust.
- **It does not duplicate the registry.** Anchors live in `_deferred-anchor-registry.md`; the
  handoff carries only the *few* that gate the next cycle, by name, with a link.
- **Keep it short enough to be read in full** — target one screen per section. If a section
  cannot be compressed, that is a signal the project has more open fronts than priorities, and
  saying so IS the handoff.
- ⚠ **A handoff written from memory is worse than none.** Re-measure the numbers at cycle end
  (`ctest`, `tools/check-anchor-balance.py`, `git log --oneline -1`) and paste what they printed.
  This repo has recorded three counts written from memory that all erred LOW; the handoff is the
  single most-quoted document in the project, so a wrong number there propagates furthest.
