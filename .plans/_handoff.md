# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-08-14 (written MID-CYCLE, deliberately, as insurance against a context loss —
this is the SECOND such write of this cycle; the session that wrote the first one ran out of context)
· **Branch:** `feature/finish-hooks-and-dependson-surface` · **no PR yet**
**HEAD:** `d4c2836` ✔MEASURED — *identical to `origin/main`*. **NOTHING FROM THIS CYCLE IS COMMITTED.**

⚠⚠ **READ THIS FIRST IF YOU ARE PICKING UP COLD: AP6 IS IN FLIGHT AND LIVES ENTIRELY IN THE WORKING
TREE.** `git status` is dirty by design and there is no WIP commit. If the tree is lost the cycle is
lost. ✔**The scratchpad plan (`ap6-plan-v2-LOCKED.md`) is NO LONGER a single point of failure** — its
binding content was copied into `.plans/06-artifact-profile-plan - tbd.md` **§5.1 B.11** this cycle,
which is now the durable home of "AP6 plan v2" and where the closed rows citing *"plan v2 §N"*
resolve. B.10 (the operator's U-2 ruling) was already durable there.

---

## 1. WHERE WE ARE

### The cycle in flight: AP6 — `dependsOn` resolution
Plan-06 §5.1 **B.1–B.12** are the decisions of record. **B.10 amends U-2** (consumer-driven
derivation) and **B.11 carries the design-audit rulings M2–M8 + the U-8 correction** — read both
before touching anything dependency-shaped.

✔**MEASURED baseline of the MERGED tree** (Windows MSVC-Debug, `build/`, 2026-08-14): build **rc=0,
ZERO warnings tree-wide**; full ctest **859/861** at the merge point, with both reds diagnosed and FIXED (§1.3). ✅ **The FINAL gate after the resolver, the two corpus examples and the CR fix is 864/864, 0 failed** (§1.2). The pre-cycle baseline at `d4c2836` was 860/860.

### 1.1 ✅ Landed in the working tree
- **Substrate** — `spawnAndWaitRedirectStdout(argv, cwd, stdoutFile)`, an OS file handle and never a
  pipe. Three latent defects found and fixed: `dup2(n,n)` is a no-op that does **not** clear
  `FD_CLOEXEC`; the exec handshake's two-arm form reported a redirect fault as "program not found";
  Windows `STARTF_USESTDHANDLES` is all-or-nothing, so supplying only `hStdOutput` **closed the
  child's stdin and stderr**. ✔Windows 48/48, ✔WSL x86_64 60/60.
- **Diagnostics** — `0xD022` Unresolvable · `0xD023` Ambiguous · `0xD024` DerivedNameInvalid ·
  **`0xD025` `D_DependencyOutputNameCollision`** · **`0xD026` `D_DependencyGraphTooDeep`**.
  ✔**NEXT FREE SLOT IS `0xD027`.**
- **Driver (U-2 mechanism)** — `Program::setResolveLibraryAdditionsByTarget`, an INTERNAL per-target
  channel **keyed by target spec** (not index-parallel — keying removes the mis-alignment failure
  mode instead of diagnosing it). `compileOneTarget` returns `std::optional<fs::path>` with
  `Program::artifactPaths()` as its reader. `project_sources.{hpp,cpp}` extracted with a `baseDir`
  that re-bases **literals as well as globs**; dedup key `weakly_canonical`; source ORDER is a
  contract. ✔Zero existing tests changed (`144/0`, `456/0` — pure additions).
- **Git acquisition** — `IGitRunner` seam, `.dss-deps` four-outcome cache machine, `dss-lock.json`,
  the four `D_DependencyGit*` codes in `kUnsuppressableCodes`.
- **The wrong-format library guard (M8)** — `ffi::readImportsForTargetFormat`, ONE chokepoint, BOTH
  binders (`ingest.cpp:135` C/HIR and `compile_pipeline.cpp:2446` asm/encode). elf↔pe pinned in both
  directions. **This closed the row that paid the balance debt.**
- **Corpus harness** — recursive neighbour staging in both runners via
  `tests/test_support/stage_tree.hpp`; ctest entry renamed to `examples/corpus-lints`.
- **★ THE RESOLVER (the hard part) — `src/program/dependency_resolver.{hpp,cpp}` + driver wiring.**
  ✔MEASURED landed: `resolveProjectDependencies()` is the public API; `dependency_resolver.cpp` is in
  `src/program/CMakeLists.txt`; `program.cpp` includes it; **the `D_PlanNotLanded` reject is GONE**
  (✔`grep -c "dependency RESOLUTION is not yet" src/program/program.cpp` → **0**).
  `tests/program/test_dependency_resolver.cpp` is 90 KB. `--force-git-cache` is wired into
  `cli_args.cpp` (✔5 occurrences). ✅ **VERIFIED — 864/864 full gate, see §1.2.**
- **Docs** — `docs/project-config-spec.md` §2.6 rewritten around resolved `dependsOn`; three
  false statements fixed (0xD020's `(url, ref)` predicate; the "eight codes" cardinal → a named list
  + range `D019`–`D024`; `--force-git-cache` is a GLOBAL flag, ✔there is no `build` subcommand).
  B.6.1's harness-vs-product `dependsOn` distinction added to the spec **and** `examples/README.md`.
  ✔Eleven drifted corpus counts in `examples/README.md` re-derived (it claimed both 593 and 581
  manifests; **593** is correct).
- **Plans** — §5.1 **B.11** added (durable home of plan v2); B.4 and B.5 annotated as AMENDED rather
  than rewritten.

### 1.2 ⛔ NOT DONE — what remains, in priority order
1. ~~Verify the resolver.~~ ✅ **DONE.** ✔MEASURED: `build/` rebuilt **rc=0, ZERO warnings**, and
   `build/bin/dss/Debug/dss_program_test_dependency_resolver.exe` **exists** (the §1.3 check the D1
   lane failed). ✔`ctest -C Debug -R "dependency|project_config|cli_args|ffi_resolve"` → **5/5
   PASSED**, including `program/test_dependency_resolver` (21.5 s) and the repaired
   `program/test_dependency_git_cache`.
   ✅ **FULL GATE PASSED, 2026-08-14:** ✔`ctest -C Debug` (unpiped, so the exit code is ctest's own)
   → **864/864 PASSED, 0 failed**, 2631 s. Baseline at HEAD `d4c2836` was 860/860. Both new corpus
   examples ran green — #617 `project_dependson_module_source_merge` (4.46 s) and #618
   `project_dependson_staticlib_artifact_link` (9.50 s). ✔`check-anchor-registry.sh` **OK (1154)** ·
   ✔`check-anchor-balance.py` **982 → 982, net 0, OK** · ✔zero CR bytes across every tracked source.
   ⚠ **An earlier attempt at this run was a FALSE PASS and the lesson is load-bearing:** a
   `ctest … | tail` started while a lane was live in the same `build/` stopped at test 11 of 268,
   never wrote `LastTest.log`, and **reported exit 0 — which was `tail`'s status, not ctest's.** Two
   ctest processes in one build directory contend over `Testing/Temporary/`. Run the full gate only
   when no lane is live in `build/`, and never read a verdict off a piped command's exit code.
2. ~~The two corpus examples~~ ✅ **DONE — both arms, `path` deps only, BOTH runners, green in the full gate.** SourceMerge (`module`)
   + ArtifactLink (`staticlib`). Both exit **42** through arithmetic no pass can constant-fold;
   removing `dependsOn` yields `K_SymbolUndefined` on all four targets and **no binary at all**, so
   the exit code is a real function of the dependency. ✔The ArtifactLink example is B.10 under runtime
   test: its dependency manifest lists **ONE** ELF spec, yet all four archives were built —
   `pe64…/fold.lib`, `elf64-x86_64…/fold.a`, `elf64-aarch64…/fold.a`, `macho64-arm64…/fold.a`. The
   rejected superset rule would have refused that on the Windows gate itself. ✔Release arms SHIPPED on
   both after measuring the arm is live in project mode (`--config=release` vs `debug` emit different
   pe64 bytes). ✔`examples/README.md` counts re-derived by parsing every manifest (593 → **595**, and
   eleven further figures). ✔aarch64 witnessed directly under `qemu-aarch64`; WSL x86_64 green.
3. **⛔ REMAINING, and it is the whole of what is left:** the **cross-leg legs** (WSL x86_64 and
   qemu-aarch64 full `ctest`, per the standing instruction that every available leg runs, not just
   the local one), then **commit + push**. Everything else in AP6 is done and gate-green.
4. **NEXT CYCLES, both operator-ruled 2026-08-14 — see §1.4b:** (a) `targets[]` derived from the
   format table (plan-06 §5.1 **B.12**); (b) the anchor-guard counting rule and the **219** genuinely
   rowless anchors (§1.5). Neither belongs in the AP6 commit.

### 1.3 The two reds the merged-tree baseline found — BOTH FIXED, one is a lesson
- **`program/test_dependency_git_cache` FAILED.** `DependencyCacheName.UrlWithNoUsableSegmentIsRejected`
  expected `NoSegment` for `https://example.invalid/`; the derivation returns `Ok`, deriving the
  authority `example.invalid`. ★ **Root cause of the ESCAPE, ✔MEASURED across every build dir in the
  tree: the authoring lane's `build-ap6-d1/` contains NO binary for that target at all.** The test was
  never compiled, so it never ran — a lane's green over a target its build dir never contained.
  **Fixed the TEST, not the derivation** (rejecting a path-less URL means parsing
  `scheme://authority/path` inside the one function a user-visible diagnostic quotes, and that
  function's docblock already refused this exact class of invention for the scp form).
  Row: `D-TEST-A-LANES-GREEN-CLAIM-OVER-A-TARGET-ITS-BUILD-DIR-NEVER-CONTAINED`, born ✅ CLOSED.
- **`anchor_registry_guard` FAILED** on three names cited under the newly-scanned `tests/` +
  `integrated_tests/` roots. ✔All three exist at HEAD and none was added by this cycle — revealed
  debt, not new. Two registered as shipped-design labels (`D-EXAMPLES-RUNNER-PROJECT-MANIFEST`,
  `D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE`, both born ✅ CLOSED); the third recorded as a **retired
  spelling** on its live row, which is what its test comment already *claimed* had been done.

### 1.4 ✅ RESOLVED — the lane-R verification gap is closed (864/864). Kept for its lesson:
Lane R reported nothing before the session ended — **there is no report, no build result and no ctest
result for the resolver.** The files exist and are wired in; that is ALL that is measured. Treat every
resolver claim as unverified until a full build + ctest is run. Given §1.3's lesson, **do not accept a
green that cannot name the build directory it was measured in.**

### 1.4b ★★ OPERATOR RULINGS TAKEN 2026-08-14 — both are NEXT-CYCLE priorities, both recorded in full
1. **`targets[]` is DERIVED from the format table, never from the composition verb** — plan-06 §5.1
   **B.12**, written out in full there. The short version: `DependencyComposition` is a CONSUMER-side
   axis and `targets[]` is a PRODUCER-side question, so keying on `SourceMerge` would make one field
   carry two unrelated facts. The union of `format.artifactProfiles()` over shipped formats already
   IS the set of profiles that can produce a build product — derive from it, bidirectionally.
   ⚠ **Two measurements taken at the ruling change its disposition and are in B.12:** ✔**SEVEN**
   profiles are served by zero formats (`gui`,`hdl`,`module`,`script`,`shader`,`sproc`,`transpile`),
   not one; and ⛔ **the LOADER CANNOT ENFORCE IT** — `src/core/types/` never includes `src/link/`
   and CMake declares `link PRIVATE core`, so the rule must live at the DRIVER beside the AP3 gate.
   The mandatory impostor-test: add `module` to ONE format's `artifactProfiles[]` in a fixture and
   `targets[]` must become REQUIRED; a hardcoded name/verb check passes everything else and fails it.
2. **The anchor guard's rowless-anchor problem is ~18× bigger than the sample.** See §1.5.

### 1.5 ★★ THE ANCHOR GUARD ACCEPTS A PROSE MENTION AS A ROW — ✔MEASURED AT SCALE 2026-08-14
The guard resolves an anchor if the string appears **anywhere** in any `.plans/*.md` — a text match,
not a row lookup. It reports **`OK (1154 src anchors all resolve to plans)`**. ✔MEASURED over all
scanned roots, collapsing wrapped-name fragments (a cited name that is a strict PREFIX of a real row
name is a legitimate wrap, not a miss):

| | count |
|---|---|
| distinct `D-*` cited in scanned roots | **1304** |
| …backed by a real `\| \`NAME\`` ROW | **882** |
| …rowless in total | **422** |
| …of those, wrapped FRAGMENTS (legitimate) | **203** |
| **…GENUINELY ROWLESS — cited in code, no row anywhere** | **219** |

⇒ **~17% of cited anchors pass only because some plan's prose happens to mention them.** The twelve
names quoted inside `D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS` were a SAMPLE, not the
population; a lane resolved 10 of them this cycle, which moved the number by ten.
📄 **OPERATOR RULING 2026-08-14, and it sets the order:** *"THE GUARD BUG OUTRANKS THE BALANCE… That
count is the actual finding; this anchor is one instance of it."* And on the accounting: *"the balance
gate forbids a cycle that OPENS NEW debt; it does not forbid a cycle that DISCLOSES PRE-EXISTING debt.
Those are different quantities and the gate should count them separately — mark disclosed-not-created
rows explicitly. As currently read, the gate rewards NOT writing rows, which is the exact dishonesty
it exists to prevent. **Fix the counting rule, do not take the §B shrug and do not suppress the rows.**"*
⇒ **NEXT-CYCLE WORK, in this order:** (1) teach `tools/check-anchor-balance.py` to distinguish
DISCLOSED-pre-existing rows from CREATED ones; (2) make the guard require a ROW rather than any prose
mention — ⚠ **without breaking the substring contract**, which ✔203 legitimate wrapped citations
depend on; (3) then register the 219 honestly under the new counting rule.
⚠ Until (1) lands, registering them is blocked by the balance gate — which is precisely the perverse
incentive the ruling names.
The guard resolves an anchor if the string appears **anywhere** in any `.plans/*.md` file — a text
match, not a row lookup. The OPEN row `D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS`
quotes twelve unregistered names in its own complaint prose, so **those names resolve against the very
document reporting them as violations**. That was harmless while `tests/` was unscanned. **This cycle
widened the guard to `tests/` + `integrated_tests/`, which makes the false green REAL and TRUSTED.**
✔MEASURED 2026-08-14, all twelve are cited in code with **zero** rows each:
`D-32-BIT-WORD` · `D-FF1-PARTIAL-CORRUPTION-WAE-PIN` · `D-FF1-TEST-BYTE-EMIT` · `D-FF2-5-FEATURE` ·
`D-H1-SUPPRESSIBLE-PER-TARGET-PIN` · `D-LIR-BUILDER-OPERAND-COUNT-GATE` · `D-LK6-14-PAYLOAD-PIN` ·
`D-LK6-14-SIZEOFCMDS-DELTA-PIN` · `D-TEST-DIAG-SEVERITY-EXACT-PIN` · `D-TEST-LE-READ-HELPERS` ·
`D-TEST-LE-READ-SIGNED` · `D-TEST-MULTI-PAGE-FIXTURE-INVARIANT`.
📄The row prescribes the fix and forbids the shortcuts: **decide per name** (registry row vs
`## Allowlist` entry), **never** narrow the guard, delete a citation, or allowlist a root wholesale.
`D-32-BIT-WORD` is a separate case — it is a literal inside a comment in
`tools/check-anchor-registry.ps1` explaining the `\b` in the anchor regex, and the prescription is to
**reword it so it is not anchor-shaped** (✔still present, 1 occurrence; the `.sh` sibling has 0).
⚠ **Do NOT weaken the substring resolution to fix this** — ✔~62 wrapped-across-two-lines citations
under the test roots depend on it (a wrapped fragment is a PREFIX, hence a substring).
⚠ Whatever lands must keep the balance gate at net ≤0: a row born 🔴 OPEN costs +1 and FAILS the gate,
so an OPEN row here is a **§B operator decision**, not a lane's call.

### 1.6 Anchor balance — ✔CURRENTLY PASSING
✔MEASURED `python tools/check-anchor-balance.py` → **982 → 982, net 0, OK**. Closed 2
(`D-CI-DCO-CHECK-RED-ON-EVERY-COMMIT-OF-THIS-BRANCH`,
`D-FFI-RESOLVE-LIBRARY-WRONG-FORMAT-GUARD-IS-INCIDENTAL` — the latter paid the debt), opened 2, both
operator-sanctioned (`D-CI-DCO-GATE-IS-ADVISORY-A-PR-MERGED-WITH-IT-RED` and the trigger-gated
`D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET`). Several further rows were born ✅ CLOSED and cost
nothing. ✔`bash tools/check-anchor-registry.sh` → **OK (1154 src anchors)** — subject to §1.5.

---

## 2. WHERE WE NEED TO GET

| Destination | The named gap |
|---|---|
| **AP6 `dependsOn` resolution** | Resolver landed but UNVERIFIED (§1.4); two corpus examples not started; gate not run. |
| **sqlite round trip proven by execution** | ~15 of 20 build cells never *run*. Needs execution legs, not more building. |
| **Unwind info on all 5 formats** | ✔Executables: pe64 + ELF + Mach-O land, ELF `.o` round-trips through gcc. Remaining: COFF `.obj` (effort) and Mach-O `MH_OBJECT` (**blocked** — no clang on this host). |
| **Assembly reaches real `gcc -S` output** | `leaq X(%rip)` unreachable — no target declares `rip`. **OPERATOR DECISION.** |
| **FC18 — `D-DIAG-CORPUS-EVERY-CODE`** | Sole remaining C23 conformance phase. New PR. |
| **Any target inside any host** | `D-HARNESS-CROSS-HOST-ANY-TARGET` stays OPEN. Blocker: `D-HARNESS-MACHO-LEG-INPUTS-UNOBTAINABLE-OFF-MAC`. |
| **A strict linker** | rc=0 on an undefined EXEC symbol → runtime exit-127, not a link error. ⚠ The live row is **`D-LINK-EXTERN-IMPORT-REFERENCE-GATE`**; the older name `D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD` was REFRAMED 2026-07-21 and citing it finds a stale row. |

---

## 3. PRIORITIES

1. **`NEXT` — FINISH AP6**, in the §1.2 order: verify the resolver, then the two corpus examples,
   then G2, then the full gate on every available leg. §A.3 binds — the hard part does not get
   sliced, and B.6.3 already ruled ONE CYCLE, all of it.
2. **`PENDING CI` — the macOS `posix_spawn` arm.** ✔Its two new file actions are `#if defined(__APPLE__)`
   and were compiled by **neither** local leg. 📄`macos-latest` is the closure leg. Do not claim it
   verified before that leg is green.
3. **`OPERATOR DECISION` — the DCO gate.** ✔PR #52 merged with `DCO fail` reported ⇒ the check is
   **advisory, not required**, contradicting the workflow's own contract. Enforce it in branch
   protection, or retire it. Row: `D-CI-DCO-GATE-IS-ADVISORY-A-PR-MERGED-WITH-IT-RED`.
4. **`TRIGGER-GATED`, do not build early** — `D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` (the row also
   records the ENUMERATION design that was measured and rejected, so it is not re-proposed) and
   `D-LINK-ELF-MISSING-DT-NEEDED-FOR-RESOLVE-LIBRARY` (narrowed to ONE outstanding measurement on the
   next Linux-host sqlite corpus run).
5. **`QUEUED`** — the wrapped anchor citations · binary rename → `dsscp` · CI + pkg-publish INERT
   (PR #45) · public repo (PR #37) · the "byte-identical vs GCC" overclaim in `pitch.txt`.

### Two anchors that must NOT be closed — closing them would itself break the bar
- `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER` — no big-endian target exists to key the facet from.
- `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED` — no shipped target declares `condCodeFromPayload` on a
  return or branch-with-link.

📄 Both trigger-gated. Building either is the speculative build §A.2 forbids *in the other direction*.
Bring as a §B decision; never close one to improve a number.

---

## 4. CONCURRENT BRANCHES / PRs — the rebase-conflict surface

✔MEASURED 2026-08-14: `gh pr list --state open` → **`[]`**. No open PR.

⚠⚠ **DO NOT READ THAT AS "NO CONCURRENT SESSION."** The operator cited measurements taken on a branch
named **`feature/c23-conformance-burndown-3`**, ✔MEASURED to exist **neither locally nor on `origin`**
in this clone. Work is happening somewhere this clone cannot see.

### Resources this cycle consumed — a concurrent session taking the same ones merges CLEAN and WRONG
- **Diagnostic slots `0xD022`–`0xD026`.** ✔**Next free is `0xD027`.**
- **Anchor names minted this cycle:** `D-CI-DCO-GATE-IS-ADVISORY-A-PR-MERGED-WITH-IT-RED` ·
  `D-BUILD-WARNING-C4834-NODISCARD-DISCARDED-IN-ASM-TEXT-TO-LIR` ·
  `D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` · `D-AP6-CROSSVALIDATE-AS-FILTER-LEAKS-MISMATCH-DIAGNOSTICS` ·
  `D-AP6-DERIVATION-MUST-RUN-AFTER-CONSUMABILITY-GATE` · `D-AP6-NEW-DIAGNOSTIC-CODES-HAD-NO-VALUE-PIN` ·
  `D-SPAWN-REDIRECT-CAPTURE-FD-COLLIDES-WITH-CHILD-STDOUT` · `D-SPAWN-HANDSHAKE-STAGE-FALLTHROUGH-REPORTS-EXECV` ·
  `D-SPAWN-WIN-USESTDHANDLES-DROPS-STDIN-AND-STDERR` · `D-EXAMPLES-RUNNER-NEIGHBOUR-STAGING-NOT-RECURSIVE` ·
  `D-TEST-EXAMPLES-RUNNER-STAGING-COPY-THREW-PAST-THE-LEDGER` · `D-AP2-LITERAL-SOURCES-RESOLVE-AGAINST-THE-PROCESS-CWD` ·
  `D-AP2-SOURCE-DEDUP-KEY-MISSES-ABSOLUTE-VS-RELATIVE` · `D-AP2-ARTIFACT-NAME-RIDES-ON-WHICHEVER-SOURCE-IS-FIRST` ·
  `D-DRIVER-COMPILE-ONE-TARGET-DISCARDS-THE-ARTIFACT-PATH` · `D-DRIVER-RESOLVE-LIBRARIES-BROADCAST-TO-EVERY-TARGET` ·
  `D-TEST-A-LANES-GREEN-CLAIM-OVER-A-TARGET-ITS-BUILD-DIR-NEVER-CONTAINED` ·
  `D-EXAMPLES-RUNNER-PROJECT-MANIFEST` · `D-EXAMPLES-RUNNER-TWO-RUNNERS-MUST-AGREE`.
- **`.plans/_deferred-anchor-registry.md`** is the hottest file in the repo. On any conflict **keep
  both sets of rows** — never delete one, the audit trail is load-bearing — then re-run
  `python tools/check-anchor-balance.py`.

### 📄 The mitigation, restated because it is the whole defence
**Stage by explicit path — NEVER `git add -A`** (`D-CYCLE-CANNOT-ASSUME-IT-OWNS-THE-WORKING-TREE`).
📄 **DCO:** every commit needs `Signed-off-by` (`git commit -s`). It is a legal attestation in a named
human's name and **an agent must not add it on the operator's behalf** — see §3 item 3.

### Build directories
The shared `build/` holds the gate build. The `build-ap6-*/` lane dirs are gitignored leftovers.
⚠ ✔MEASURED that a lane dir can be MISSING the very target its lane claimed to verify (§1.3) — a lane
build dir is evidence only if it actually contains the binary.

---

## 5. TIMELINE

*Newest first. Accumulates — new cycles are prepended. Includes cycles that did not go well.*

| Date | Commit | What shipped | Gate |
|---|---|---|---|
| 2026-08-14 | *(uncommitted)* | **AP6 in flight — see §1.** Resolver + driver wiring landed (the `D_PlanNotLanded` reject is gone); git acquisition; per-target library channel; wrong-format guard at both binders; 3 latent spawn defects fixed; recursive corpus staging; docs rewritten; plan v2 rescued into §5.1 B.11. 5 diagnostic slots taken (`0xD022`–`0xD026`). **Two reds found by the merged-tree baseline and fixed**, one of them a test its authoring lane never compiled. | merged-tree build **rc=0, 0 warnings** · ctest **859/861** → both reds fixed · balance **982→982 OK** · anchor guard **OK** (with the §1.5 false-green residual) · ⚠ resolver **UNVERIFIED**, corpus examples **not started** |
| 2026-08-13 (post-push) | — | **Two findings after the cycle closed, neither moving a verdict.** (a) The WSL lane's build watcher span until killed **over a build that had SUCCEEDED** — its producer `tee -a`'d both FAILURE arms into the log but wrote `BUILD OK` to stdout only. (b) `e42ae5a5`'s message quotes **1018** anchor citations; the committed tree measures **1019**. | gates re-run: guard OK 1019 · balance 983→983 · line-endings OK |
| 2026-08-13 | `e42ae5a5` | **Unwind lands**: DWARF CFI + `.eh_frame` on ELF/Mach-O execs (gdb unwinds 4 DSS frames) and in ELF `.o` (round-tripped through gcc, 9 frames vs 2 stripped) · 2 silent pe64 unwind miscompiles · interior labels end-to-end · arm64 32-bit bitwise widening + MOVZ W-form · `.section`/`.space` · config key gates · **2 false-green red-on-disable mechanisms found** · handoff created | **Win 851/851 · WSL 851/851 · arm64 594/594 strict** |
| 2026-08-13 | `75ca4034` | asm-anchor burn-down: net −4 anchors; closed 2 silent miscompiles shipped one cycle earlier; a `.s` calls libc and RUNS | Win 838/838 · ⚠ **WSL + arm64 NOT run** |
| 2026-08-13 | `e5b60f6c` | Second assembly dialect (arm64). **Shipped 2 silent miscompiles** — negative scalars lost their sign; `[x29,#-8]` read as scale | Win 831/831 · ⚠ **1 leg of 3** |
| 2026-08-12 | `4969e9e2` | Inline asm P1+P2 — assembly becomes its own source language | — |
| 2026-08-12 | `ca2c6721` | DSS Axis + DSS HIR plan rework | — |
| 2026-08-12 | `60eb8ed8` | **PR #50 merged.** C23 burn-down: silent stringize miscompile, `__VA_OPT__`, GNU spellings, UCRT migration finished | — |
| 2026-08-11 | `0ecec160` | ELF copy relocations **deleted** — name-scoped copy reloc silently emptied glibc's `environ` alias set | 5/5 build · 2 legs by execution |
| 2026-08-10 | `3e86a187` | **PR #48.** pe CRT → UCRT; MIR call-site signature checking | — |
| 2026-08-03 | `f7c378be` | **PR #46.** SQLite compiled from full upstream source, suite green | — |
| 2026-07-20 | `4ccd6c6f` | **PR #47.** Static linking all formats · long double F80/F128 · type identity | — |
| 2026-07-15 | `d0c132c3` | **PR #41.** Cross-toolchain relocatable objects — DSS `.o` links + runs under gcc | — |
| 2026-07-09 | `c7a5377f` | **PR #36.** C23 FC16 + release-optimizer perf arc (>30 min → ~2 min) | — |
| ≤2026-07-08 | — | 🧠 Compressed: C23 FC17/17.5 (`_BitInt`, `thread_local`), C11 `<threads.h>`, arm64 Mach-O, `<stdbit.h>`, Apache-2.0 relicense (PRs #36–#45) | — |
