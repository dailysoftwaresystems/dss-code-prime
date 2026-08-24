# The fail-loud gate + the cross-plan update

### Step 6 — Fail-loud gate
This is the canonical gate checklist (§A.6 is its one-line statement). Verify every item:
- `cmake --build build` clean (no link errors).
- **★★★ THE BUILD IS VERIFIABLE — run this BEFORE trusting any ctest result:**

  ```bash
  python scripts/check-ninja-deps/check-ninja-deps.py            # no argument: it finds the tree
  python scripts/check-ninja-deps/check-ninja-deps.py build/p29-x  # or name YOUR lane's tree
  ```

  ⚠⚠ **THIS LINE USED TO READ `… check-ninja-deps.py build-dbg`, AND THAT PATH HAS NOT EXISTED SINCE
  THE ONE-ROOT `build/` MIGRATION.** ✔MEASURED 2026-08-23 at `6dc63be0`: the tool printed
  `SKIP build-dbg -- no build.ninja` and exited **0**, so a cycle following this reference verbatim
  ran the build-verifiability check against a nonexistent directory and read `rc=0` as a pass.
  Both halves are fixed: the invocation above, and the tool — a named directory that does not exist
  is now **exit 2**, never a skip (`D-GATE-NINJA-DEPS-EXITS-ZERO-ON-A-DIRECTORY-THAT-DOES-NOT-EXIST`).
  A directory that exists but is not a ninja tree is also exit 2 unless you pass `--allow-non-ninja`,
  which prints a SKIP naming the flag — an unasked-for skip is indistinguishable from a pass.
  ⓘ The no-argument form auto-picks `build/dbg`, else the pre-migration `build-dbg`, else fails loud
  on `build/dbg`. **Name your own tree when you are a lane** — the whole point is to verify the tree
  the ctest result came from.
  ⓘ Its `--self-test` (7 parser cases + 5 target-verdict cases) now rides ctest as
  `ninja_deps_selftest_guard`. The PROBE form still cannot be a ctest entry: it needs a build
  directory, and a source checkout has none.

  A green ctest proves nothing about the current source if the objects it linked were never
  rebuilt, and that is not hypothetical here: `ninja -t deps` has twice reported `#deps 0` on
  live objects — **10 of 403** on 2026-08-13, then **51 of 430** after a seven-lane concurrent
  cycle, **16 of those being `src/` TUs compiled into the shipped DLL**. A gate run in that state
  is a green suite over a partly-stale compiler: the same class as a false-green red-on-disable,
  arriving through the build system instead of through a test
  (`D-BUILD-NINJA-RECORDS-ZERO-HEADER-DEPS-UNDER-CONCURRENT-BUILDS`).
  ⚠ `#deps 0` is NEVER legitimate — ✔MEASURED that even a TU with zero `#include` directives
  records `#deps 1` (gcc lists the source itself), so a zero count is always a lost record, never
  a header-free file. The tool treats an EMPTY parse as FATAL too: "nothing found" and "nothing
  ran" look identical from outside, and this project has been burned by that ambiguity before.
- `ctest --test-dir build --output-on-failure` 100%, including the new tests.
- ★★★ **A CTEST RUN THAT OVERLAPPED A BUILD IS VOID IN BOTH DIRECTIONS — AND IT CAN STILL LOOK
  GREEN.** ✔MEASURED 2026-08-17: a lane ran `ninja` while its own gate was executing (the documented
  *mid-run DLL relink is never OK* hazard), and the run **reported 875/875** while emitting ~400
  spurious `***Exception` lines. Its one substantive failure, `examples/c-subset/double_to_unsigned`,
  **passed on a clean re-run** — so the poisoning invented a failure, and a run that can invent one
  can equally mask one. ⇒ **Never treat an overlapped run as evidence in either direction. Re-run it.**
  ⚠ **AND KILLING THE WRAPPER DOES NOT KILL `ctest`.** In the same incident `TaskStop` terminated the
  wrapper while `ctest` kept running and kept writing — so a gate you believe you stopped may still be
  live, competing for the DLL and the log. Confirm the process is gone before rebuilding, or the next
  run inherits the first one's corruption. ⓘ The honest reporting standard the same lane then met:
  *"did not reach a terminal state … the full-suite figure is unmeasured"* — a partial count plus a
  named reason beats a number nobody can trust.
  ★★★ **TRIAGE RULE THAT SETTLES THIS CLASS IN ONE GREP — `0xc0000142` EN MASSE IS ENVIRONMENTAL, NEVER
  A CODE REGRESSION.** ✔MEASURED on the poisoned log: tests **1–100 all passed**, then **775 failures,
  every single one `Exit code 0xc0000142`** (STATUS_DLL_INIT_FAILED — the process never *started*), with
  **zero** `(Failed)`, zero `(Subprocess aborted)`, zero `(Timeout)`, and the cutover exactly at
  teardown. **A code defect produces assertion failures; it cannot stop 775 unrelated binaries from
  initialising.** ⇒ classify by FAILURE KIND before believing a count: a uniform startup code across
  hundreds of unrelated suites is the harness or the DLL, and the number is not a verdict about the
  compiler at all.
  ⛔ **Two handling rules from the same incident.** (1) When you kill an orphaned `ctest`, match on the
  COMMAND LINE and kill by explicit PID — killing by process name murders a concurrent operator gate
  (there were two unrelated `ctest.exe` processes running from `C:\Program Files\CMake\bin`). (2) Do
  NOT edit a poisoned log to annotate it; drop a `*.POISONED.README.txt` marker beside it. Editing the
  evidence is how a suspect log later gets read as a clean one.
- **no NEW `abort()` in test code:**

  ```bash
  python scripts/check-no-abort-in-tests/check-no-abort-in-tests.py
  ```

  `std::abort()` in a fixture kills the whole test PROCESS, so every sibling test in that
  executable loses its verdict and the harness cannot report which unit failed. ✔MEASURED
  2026-08-17: a config-mutating pin drove `loadShipped` to a **legitimate** refusal and the binary
  died with `0xc0000409` mid-suite, taking **nine passing tests' results** with it and reporting an
  exception code instead of the load error — the arm that was working correctly is the one that
  destroyed the evidence. ★★ The guard is a **RATCHET, not a clean bill**: the row that demanded it
  named 2 occurrences and the first scan found **61 live sites across 29 files** (the same
  `ADD_FAILURE(); std::abort();` idiom copy-pasted), so those are recorded in an `INVENTORY` whose
  per-file ceilings may only come **down**. A new site reds; a *fixed* site also reds until the
  ceiling is lowered, because unclaimed headroom is where the next regression hides. ⚠ Do not
  confuse `INVENTORY` with `ALLOWLIST` — the latter is by PROOF and is empty. Burn-down is
  `D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD`, which stays OPEN until the inventory is.
  ⓘ Self-tests: `python scripts/check-no-abort-in-tests/check-no-abort-in-tests.py --selftest` (the comment/string stripper is
  the whole correctness — a bare token grep would red on the very file that documents the fix).
- anchor-registry guard OK: `scripts/check-anchor-registry/check-anchor-registry.ps1` (or `.sh`).
  ⓘ Exit **4** now means *a citation names a RETIRED anchor id* — a name whose registry row opens
  with the `RETIRED-ID` marker. Resolution is substring-anywhere (load-bearing: it is what lets a
  line-wrapped citation resolve), so it cannot otherwise tell a live name from a dead one — a stale
  id resolved for weeks on the strength of the row written to report it as stale
  (`D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT`).
- script-index guard OK: `python scripts/check-scripts-index/check-scripts-index.py`.
  Rides ctest as `scripts_index_guard`, so it runs anyway — run it directly when this cycle
  added, renamed, deleted or REPURPOSED a script. It reds when the tree and the two indexes
  (`scripts/README.md`, `references/scripts.md`) disagree, or when a script's own `PURPOSE:`
  line differs from its index row. Regenerate both with `--write`; never hand-edit the block
  between the generated-index markers.
- shell-portability guard OK: `python scripts/check-shell-portability/check-shell-portability.py`.
  Rides ctest as `shell_portability_guard`, so it runs anyway — run it directly when this cycle
  touched a `.sh`. It refuses two shapes that are fatal on **macOS bash 3.2** and invisible
  everywhere else: a `case` inside `$( … )` whose patterns are not `(`-prefixed, and a bash-4+
  construct in a file with no `BASH_VERSINFO` gate above it.
  ★★ ⚠ **`bash -n` CANNOT FIND THE FIRST ONE** — ✔MEASURED 2026-08-22: the probe file parses clean
  under 3.2 (exit 0) and fails only when the substitution is EXPANDED. Do not "check it with the old
  shell" instead; that instrument is blind by construction. The cost of not having this guard was
  `anchor_registry_guard` dying inside its OWN self-test on `macos-latest`, so the anchor registry
  had never been checked on that host (`D-SCRIPT-CASE-IN-COMMAND-SUBSTITUTION-BREAKS-BASH-3-2`).
  ⓘ It walks the TREE and asks git only what is IGNORED — never `git ls-files`. ✔MEASURED on the
  macOS leg the same day: a carriage's checkout sits at an old commit with the working tree rsynced
  over it, so the INDEX named seven deleted `tools/*.sh` paths and the guard reported seven
  violations on a host where nothing was wrong. Every carriage has that shape, so the distinction is
  operational, not academic (`D-GATE-SHELL-PORTABILITY-SCANNED-THE-INDEX-NOT-THE-TREE`).
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
  python scripts/check-anchor-balance/check-anchor-balance.py
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

  ⓘ ANCHOR-GUARD-QUOTED-NOT-CITED: `D-PP-COMPILER-IDENTITY-INCOHERENT` `D-PP-IMPL-DETAIL-PREDEFINES-ABSENT` `D-PP-CODE-MODEL-PREDEFINES`
  — the three ids in the paragraph above are **quoted, not cited**. They name rows that were correctly
  DELETED, so they resolve to nothing **by design**, and the sentence only carries weight because they
  are the real names. They must not be given rows, allowlisted, or blurred into placeholders to make a
  tool green: the record is the authority, the guard is the instrument. The anchor guard exempts them
  **in this file only**, and reds if any of them is ever cited elsewhere, acquires a plan row, or stops
  appearing above — see the QUOTED-NOT-CITED block in
  `scripts/check-anchor-registry/check-anchor-registry.sh`.
- **★ FOUR ROWS THAT LOOK DIFFERENT AND ARE THE SAME ROW.** Before appending, grep the registry for
  the SYMPTOM, the ARTEFACT and the FILE NAMES in your evidence — not the title you have in mind
  (§C.-1 1b). Nine rows about one absent macro family is one row, or better, one fix.
- **★★ THE DIAGNOSTIC-CODE ALLOCATION GATE — the ordinal space has no lock, so this is the lock.**

  ```bash
  python scripts/check-diagnostic-codes/check-diagnostic-codes.py
  ```

  `DiagnosticCode` is one flat ordinal space whose values are PUBLISHED identities (`error[D0029]`,
  quoted in docs and `expected.json` fixtures), so a renumber rewrites a name users have already
  seen. It has two hazards and **the AP5/AP6 close-out hit both**:
  - ✔**Two concurrent lanes allocated `0xD029`.** One lane was told the slot was free; another had
    already taken it for `D_DependencyBuildFailed`. Nothing mechanical noticed — it was caught only
    because the second lane RE-MEASURED the header instead of trusting its brief. That is diligence,
    not a mechanism, and it is the same non-mechanism `scripts/run-gate/run-gate.sh` exists to replace.
  - ✔**A code shipped with no test at all.** `D-AP6-NEW-DIAGNOSTIC-CODES-HAD-NO-VALUE-PIN` closed on
    exactly this and **re-opened one cycle later**: `D_LanguageTargetIsaMismatch` (0xD02A) landed
    engine code in `src/` while appearing in ZERO test files.

  ★ **The instrument reads the ENUM, never a hand-maintained table.** The contiguity pin in
  `tests/core/test_parse_diagnostic.cpp` is a good pin and it structurally cannot catch either
  hazard: it only checks rows somebody remembered to add, so a lane that allocates and never touches
  the table is invisible to the very test meant to catch it. Anything asking a human to keep a second
  list in sync has the failure mode of the thing it is checking.

  Three checks: **duplicate value** → fatal, no baseline (hazard 1); **enumerator with no explicit
  value** → fatal, because it takes `predecessor + 1` and an inserted row above it silently
  renumbers a published code; **code no compiled test names** → RATCHET against a frozen baseline of
  37 pre-existing, since that debt is not one cycle's work but NEW debt is.
  - ⚠ **`UNCOVERED_BASELINE` shrinks freely and GROWS only as a §B decision.** Appending a name to
    reach green is the workaround this gate exists to forbid; the tool prints every name that becomes
    covered so the shrink is visible.
  - ★ Coverage is measured with **comments stripped** — 7 codes are named only in test prose, and
    counting a comment would let a lane satisfy the gate by MENTIONING its new code in a sentence.
    Identifiers are tokenized, not substring-matched: ✔the first real run corrected the very scan
    that built its baseline, which had called `P_InvalidEscape` covered on the strength of
    `P_InvalidEscapeSequence` appearing in a test.
  - A collapsed parse (enum block not found, implausibly few enumerators, no test sources) exits **2**
    rather than reporting "0 duplicates, OK" — the instrument that enforces run-gate's lesson must not
    embody its inverse.
  - `--self-test` covers the collision shapes a text-compare would miss (`0xd029` vs `0xD029`,
    decimal vs hex), the commented-out-enumerator false positive, the comment-strip property, and the
    collapse guards. Run it if you touch the script.

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

##### ⛔ REWRITE IT ENCODE-FIRST AND REPLACE ATOMICALLY — never open the real file for writing

✔MEASURED 2026-08-24 (cycle P29, `D-CYCLE-HANDOFF-PATCHER-TRUNCATES-ITS-TARGET-BEFORE-IT-CAN-FAIL`):
an orchestrator patch script did `io.open(path, "w", …)` and then `fh.write(...)`. The write raised
on a lone surrogate pair in one of its own literals — **and `"w"` had already TRUNCATED the file**, so
a **377 KB handoff became 0 bytes**. It was recovered with `git checkout --` only because the handoff
had not yet been edited that cycle; ten minutes later that recovery would have destroyed real work.

⇒ **Build the bytes first, then touch the target:**

```python
data = "\n".join(lines).encode("utf-8")   # any failure lands HERE, target untouched
tmp = path + ".tmp"
with io.open(tmp, "wb") as fh:
    fh.write(data)
os.replace(tmp, path)                     # atomic
```

★ **The repository's own scripts already do this** — `check-plan-citations` and
`check-wrapped-anchor-ids` both write `path + ".tmp"` and `os.replace`, and `check-scripts-index`
does the same. ⚠ **The gap was in the ORCHESTRATOR's own fold tooling, which no guard covers**, and
that is the general shape: the cycle's throwaway scripts hold the same power over the tree as the
registered ones and none of the discipline. A scratch script that rewrites a tracked file is subject
to every rule a shipped one is.

⚠ **And a lone surrogate pair is a real hazard here, not a curiosity.** `"🔵"` written as
two `\u` escapes is not a character — it is two unpaired surrogates, and it encodes to nothing. The
status glyphs this project uses are astral (🔵 is `U+1F535`), so **write them as the literal glyph or
as a single `\U0001F535`**, never as a surrogate pair.

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
  (`ctest`, `scripts/check-anchor-balance/check-anchor-balance.py`, `git log --oneline -1`) and paste what they printed.
  This repo has recorded three counts written from memory that all erred LOW; the handoff is the
  single most-quoted document in the project, so a wrong number there propagates furthest.
