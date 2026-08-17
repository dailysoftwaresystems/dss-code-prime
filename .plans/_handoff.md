# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-08-15 (AP5/AP6 CLOSE-OUT, written mid-cycle as insurance — the THIRD such
write of this arc) · **Branch:** `feature/finish-hooks-and-dependson-surface`
· 📄 **PR [#53](https://github.com/dailysoftwaresystems/dss-code-prime/pull/53) is OPEN against `main`.**

⚠⚠ **READ THIS FIRST: THE TREE IS NO LONGER CLEAN, AND THE CLOSE-OUT IS UNCOMMITTED.**
✔MEASURED 2026-08-15: **508 dirty paths — 455 under `examples/`** (the corpus arming, §1.0) **and 53
elsewhere**, plus 3 untracked. ⇒ **the earlier "AP6 is committed and pushed, the working tree is
CLEAN" line was true on 2026-08-14 and is now FALSE**; five parallel lanes ran after it. Nothing in
§1.0 is committed. If this tree is lost, that work is lost.

✅ **AP6 ITSELF IS COMMITTED AND PUSHED** — `867fa81`, `e3fd4e1`, `6a4dac6`, `293d069`. What follows
in §1.0 is the close-out ON TOP of those. ✔MEASURED commits ahead of
`origin/main`: `867fa81` (AP6 itself), `e3fd4e1` (plan-06 B.12 corrected), `6a4dac6` (WSL leg +
harness rules + DCO authorization), `293d069` (the macOS leg). ⚠ The first two are **UNSIGNED** —
they predate the operator's sign-off authorization (§4) and were pushed unsigned by explicit
instruction; the branch is exactly these commits, so a sign-off rebase reaches all of it. The
operator merges when the PR is finished.

⚠ **The earlier "AP6 lives entirely in the working tree, if the tree is lost the cycle is lost"
warning is RETIRED** — it was true for most of this cycle and is now false. ✔**The scratchpad plan (`ap6-plan-v2-LOCKED.md`) is NO LONGER a single point of failure** — its
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

### 1.0 ⚠ THE 2026-08-15 CLOSE-OUT — UNCOMMITTED, five parallel lanes plus orchestrator work

**Operator instruction for this stretch:** finish everything artifact-profile-related; defer the
rest to another session. ⛔ **`D-TEST-CLI-CORPUS-RUNNER-IGNORES-OPTIMIZED-PIPELINES-AND-STDOUT` is
EXPLICITLY EXCLUDED** by standing instruction (it would conflict with another session's PR) — do not
touch it or `integrated_tests/**`.

**a. ★ NEW MECHANICAL GATE — `tools/check-diagnostic-codes.py`.** ✔Built because two concurrent lanes
allocated `0xD029`, caught only because one lane RE-MEASURED instead of trusting its brief. Reads the
ENUM, never a hand-maintained table — the contiguity pin in `test_parse_diagnostic.cpp` structurally
cannot catch this, since it only checks rows somebody remembered to add. Three checks: duplicate
value (fatal), enumerator with no explicit value (fatal), code no compiled test names (ratchet vs a
frozen 37-name baseline). Prints next-free-per-band on the green path. `--self-test` included; wired
into the Step 6 battery in `references/gate-and-cross-plan.md`.
✔**RED-ON-DISABLE on the production header** (268,981 bytes): clean unmutated; re-staging the real
collision reports it; dropping an explicit value reports it — each mutation guarded by an assert that
the bytes changed.

**b. ✅ ALL FOUR LEGS GREEN — 865/865 EACH. ✔MEASURED 2026-08-15/16, rc captured DIRECTLY from each
command and never inferred from a wrapper.** Baseline was 864/864 at the AP6 commit; the +1 is the
new `program/test_cross_validate_language_target` binary, so the delta reconciles exactly.

| leg | build | ctest |
|---|---|---|
| Windows MSVC-Debug | `BUILD-EXIT=0`, zero warnings | **`FULL-CTEST-RC=0`, 865/865** |
| WSL x86_64 gcc (native ext4) | rc=0, only pre-existing `-Winvalid-pch` | **865/865**, 0 committed CRLF |
| qemu-aarch64, `DSS_STRICT_ARM_VERDICTS=ON` | rc=0, `file` confirms aarch64 | **`CTEST-RC=0`, 865/865** |
| macOS Apple Silicon, Apple clang 21.0.0 | rc=0, 1 warning **in vendored googletest only** | **`CTEST-RC=0`, 865/865** |

⚠⚠ **EVERY NON-WINDOWS LEG FIRST FAILED ON AN INVISIBLE AMBIENT PRECONDITION, NOT ON THE PRODUCT.
Read these three rows BEFORE re-running any leg or you will re-diagnose them from scratch:**
- **arm64** reported **595 of 865 FAILED (31% passing)** — all from ONE harness self-check
  (`RunHarnessStack`, `RLIMIT_STACK` 8 MiB vs 256 MiB wanted) linked into all 595 example binaries.
  `Examples.RunFromManifest` passed throughout. `ulimit -s 262144`, no rebuild ⇒ 865/865.
  ★ The orchestrator's first hypothesis (strict verdicts converting skips to failures) was
  **REFUTED by the ledger** (`environmental: 0 emulator-missing`). Do not re-derive it.
  [[D-TEST-ARM64-LEG-NEEDS-AMBIENT-ULIMIT-STACK-OR-595-ENTRIES-RED]]
- **macOS** died at configure with `cmake: command not found` because the emsdk login profile
  **REPLACES** `PATH`, hiding `/opt/homebrew/bin`. Fix: prepend it, plus `EMSDK_QUIET=1`.
  ⚠ Never pipe binary OUT of that host — the profile writes to stdout.
  [[D-TEST-MACOS-LEG-EMSDK-PROFILE-REPLACES-PATH-HIDING-HOMEBREW]]
- **WSL** reported `line_endings_guard` red purely because the rsync excluded `.git/`; the guard
  fails closed by design and is RIGHT. With `.git` present it passes and reports 0 CRLF files.
ⓘ macOS passed with `ulimit -s` still at 8176, which CORROBORATES that the harness's stack self-bump
works natively and the arm64 cascade was specifically a `qemu-user` limitation.

★★ **THREE TIMES THIS CYCLE A BACKGROUND TASK REPORTED `exit code 0` OVER WORK THAT NEVER RAN** — a
wrong MSBuild target (`MSB1009`), the macOS `cmake` failure, and an orphaned macOS task whose whole
output was a missing input file plus `Connection reset by peer`. **In every case the only thing that
caught it was an explicit `echo "RC=$?"` immediately after the command.** ⇒ `tools/run-gate.sh`'s
rule applies to REMOTE and BACKGROUND work too, not just local gates.

**c. Lane work landed (all uncommitted).**
- **`-Werror=switch` / `/we4062` tree-wide** at ONE chokepoint (`CMakeLists.txt:220-227`), retiring
  FIVE per-file ratchets across 8 hand-listed files. ✔Coverage 434/434 of our TUs, 0/4 googletest.
  ✔Zero fallout (257 warnings = baseline exactly). ✔Red-on-disable on the REAL defect: the
  `CfClass::Switch` fallthrough is now a build error on the local MSVC gate, not a macOS-only
  sighting. Closes plan-07 **G-711**, pending ~3 months.
- **`emitsArtifact` fully reverted** (byte-identical to HEAD); `0xD027` freed; **`module` added to
  all TEN `lib`/`staticlib` formats**; the AP3 reject split (`0xD011` vs `0xD028`) pinned
  three-sidedly. ✔`ModuleIsALibrary.StandaloneModuleBuildEmitsAnArchive` + `…EmitsNoSecondArtifact`.
  ★ Zero engine code — the archive fork dispatches on the format's declared `container`.
- **`0xD029` `D_DependencyBuildFailed`** split out of `0xD022`. NOT unsuppressable, and that is a
  measured VERDICT: it is an attribution line above merged inner diagnostics that survive.
- **Byte-identity detector** in `examples_runner.cpp` (+549) with the per-arm
  `mustDifferFromBaseline` lever; then **548 arms armed across 385 manifests**, 97 vacuous units
  triaged and documented, **zero examples edited, zero arms deleted**.
- **ISA gate behavioural test** — `tests/program/test_cross_validate_language_target.cpp` (new, 857
  lines, 19 tests), all four verdicts non-vacuous, impostor built from target twins differing ONLY in
  `target.isa`, both call sites reached, red-on-disable turning exactly ONE pin red.

**d. ★ ORCHESTRATOR FIX — `D_LanguageTargetIsaMismatch` IS NOW UNSUPPRESSABLE.** The ISA lane
refuted the landed note, which claimed suppression costs only "less explanation". ✔MEASURED via the
real CLI: `--suppress=D_LanguageTargetIsaMismatch` gave **rc=1, stdout 0 bytes, stderr 0 bytes** — a
silent non-zero exit, prong (2) verbatim. Added to the table; the allocation note rewritten. ⓘ The
addition overflowed `std::array<UnsuppressableEntry, 150>` and broke the build — extent raised to
151, and the explicit count is now documented as deliberate (it makes an unconsidered append a
compile error, which is what it just did).

**e. ✔ANCHOR STATE.** Guard **OK (1163 src anchors all resolve, 0 cell-width violations)**.
Balance **created 4, closed 1 — FAILING, and deliberately so (§B carry, operator-authorized).** The
four: `D-DEPS-NO-ARTIFACT-SHARING-ACROSS-BUILDS-AT-ONE-CONFIGURATION` and
`D-DEPS-SOURCEMERGE-INHERITS-THE-CONSUMERS-COMPILATION-ENVIRONMENT` (both requested by the operator);
`D-TEST-CORPUS-DARWIN-LEG-BYTE-IDENTITY-UNMEASURED`; `D-AP6-NO-CORPUS-EXAMPLE-FOR-A-STANDALONE-MODULE-BUILD`.
⚠ **Do NOT close any of them by weakening a row.**

**f. ★★ AP7 IS SCOPED AND TRIGGER-GATED.** Operator, 2026-08-15: *"we'll have an AP7 that will allow
any language project to import any other language project and we'll use the IRs to make it work…
allow a C language project to import a python one."* **TRIGGER: *"we need a second language for that
before starting this anchor."*** ⛔ Not a TODO — if the trigger has not fired, report "trigger not
fired" and skip. ⓘ Whether a given shipped language satisfies it is a **§B call, not the cycle's**.
Recorded in plan-06 §5 (AP7 row) and `D-DEPS-SOURCEMERGE-INHERITS-THE-CONSUMERS-COMPILATION-ENVIRONMENT`.

**h. ★★★ MERGE GUIDANCE FOR PR [#54](https://github.com/dailysoftwaresystems/dss-code-prime/pull/54)
(`feature/c23-conformance-burndown-3`, "GNU extended inline asm"). READ BEFORE MERGING EITHER PR.**
✔MEASURED 2026-08-15 (`gh pr view 54 --json files` ∩ `git status`): **17 files are touched by BOTH
branches.** None of the overlaps is a semantic fight — they are independent additions to shared
files — but **two of them break silently if merged naively**, so they are called out first.

- ⚠⚠ **`src/core/types/unsuppressable_codes.cpp` — THE DANGEROUS ONE.** The table is a FIXED-EXTENT
  `std::array<UnsuppressableEntry, N>`. This branch raised **150 → 151** (adding
  `D_LanguageTargetIsaMismatch`); #54 adds its own rows and will have raised it too. **Taking both
  sides' rows while keeping ONE side's extent does not compile — and a merge that "resolves" it by
  dropping rows to fit the number is silently wrong.** ⇒ take ALL rows from both sides and set the
  extent to `150 + (this branch's 1) + (#54's count)`. The explicit count is deliberate: it makes an
  unconsidered append a compile error, and it already caught this branch doing exactly that.
- ⚠⚠ **`src/asm/asm_text_to_lir.cpp` + `src/asm/CMakeLists.txt` — #54 MAY FAIL TO BUILD AFTER THE
  MERGE, THROUGH NO FAULT OF ITS OWN.** This branch enabled **`-Werror=switch` / `/we4062`
  tree-wide** at one chokepoint (`CMakeLists.txt:220-227`) and RETIRED the five per-file ratchets,
  including `src/asm`'s. ⇒ every switch #54 adds over an enum is now a hard error if non-exhaustive,
  on a file #54 heavily edits. 📄Expect to fix a few arms; do NOT re-add a per-file ratchet or a
  `default:` to silence it — the whole point is that the compiler walks you to every site.
  ✔This already caught a live silent fallthrough (`CfClass::Switch`) that only macOS had seen.
- **`src/core/types/parse_diagnostic.{hpp,cpp}` — no conflict in substance: DIFFERENT BANDS.** This
  branch took `D_` **0xD028 / 0xD029 / 0xD02A**; #54 took `S_` **0xE065..0xE06B** and `L_`
  **0xB010..0xB012**. ⇒ take both sides' enumerators verbatim. ⛔ **RENUMBER NOTHING** — these are
  published `error[Dxxxx]` identities. ✔Run `python tools/check-diagnostic-codes.py` immediately
  after the merge: it fails on any duplicate value and on any unvalued enumerator. ⓘ `0xD027` is a
  WITHDRAWN HOLE, deliberately not back-filled.
  ★ **AND RETIRE THE RESERVATION:** `RESERVED_ELSEWHERE` in that script currently holds #54's two
  ranges so this branch could not allocate into them. **The moment #54 merges, DELETE both rows** —
  the codes are then in the enum and the ordinary duplicate check covers them, whereas a stale
  reservation starts refusing legitimate ordinals. See `D-DIAG-ORDINAL-SPACE-HAS-NO-CROSS-BRANCH-VIEW`.
- **`.plans/_deferred-anchor-registry.md` — both sides APPEND rows. Take both; never drop one** (the
  audit trail is load-bearing, §F). ⚠ Watch the cell-count guard: a literal `|` inside a cell is read
  as a column boundary and silently truncates the row — escape it `\|`. Run
  `tools/check-anchor-registry.sh` after merging; it catches exactly that.
- **`.plans/_handoff.md` — do NOT hand-merge hunks.** This file is REWRITTEN wholesale every cycle by
  contract. Whoever merges second rewrites it for the merged state.
- **`src/dss-config/targets/{x86_64,arm64}.target.json` and `sources/asm-{x86_64-att,arm64-gas}.lang.json`
  — take both sides' keys.** This branch added the **`isa` axis** (`x86_64` / `aarch64`; the target
  `arm64` deliberately declares `aarch64`, ARM's psABI name — **do not "tidy" that to match the
  target name**, a test pins the divergence on purpose). #54 edits the same documents for inline-asm
  vocabulary. The keys are disjoint.
- **`src/core/types/target_schema.{hpp,_json.cpp}`, `grammar_schema_json.cpp`** — this branch added
  the `isa` accessor and its loader; #54 adds inline-asm fields. Disjoint additions, take both.
- **`tests/core/test_parse_diagnostic.cpp`** — this branch extended the contiguity run through
  `0xD02A` and pinned the `0xD027` hole explicitly (`kWithdrawnSlot`). If #54 adds value pins,
  keep both and raise the hand-maintained `EXPECT_EQ(checked, N)` to the combined total.
- **`examples/c-subset/c_inline_asm/expected.json`** — the only example both touch. This branch added
  `mustDifferFromBaseline` / `$commentByteIdentical` corpus keys; #54 owns the example's content.
  Take #54's content plus this branch's keys. 📄**For #54's OTHER new examples:** if an
  `examples/asm/**` example declares `optimizedPipelines`, it will be byte-identical to its baseline
  by construction (assembly never forms HIR/MIR), so give it a `$commentByteIdentical` note —
  **not** `mustDifferFromBaseline`, which would red it forever.
- `src/lir/CMakeLists.txt` — this branch retired its per-file switch ratchet; take that plus #54's edits.

📄 **What #54 does NOT need to worry about:** `integrated_tests/runner.cpp` is untouched by this
branch, and `D-TEST-CLI-CORPUS-RUNNER-IGNORES-OPTIMIZED-PIPELINES-AND-STDOUT` was deliberately left
undone by operator decision precisely to keep it that way.

**g. NEXT, in order.** (1) finish the full ctest; (2) re-run the other three legs — nothing since the
close-out began; (3) the MinGW `ConfigMirror` fix, which reds two of B.10's fail-closed pins on every
gcc leg; (4) commit code AND plans together. ⚠ **`.plans/_handoff.md` must be staged in THAT commit.**

### 1.1 ✅ Landed in the working tree
- **Substrate** — `spawnAndWaitRedirectStdout(argv, cwd, stdoutFile)`, an OS file handle and never a
  pipe. Three latent defects found and fixed: `dup2(n,n)` is a no-op that does **not** clear
  `FD_CLOEXEC`; the exec handshake's two-arm form reported a redirect fault as "program not found";
  Windows `STARTF_USESTDHANDLES` is all-or-nothing, so supplying only `hStdOutput` **closed the
  child's stdin and stderr**. ✔Windows 48/48, ✔WSL x86_64 60/60.
- **Diagnostics** — `0xD022` Unresolvable · `0xD023` Ambiguous · `0xD024` DerivedNameInvalid ·
  **`0xD025` `D_DependencyOutputNameCollision`** · **`0xD026` `D_DependencyGraphTooDeep`**.
  ⛔ **"NEXT FREE SLOT IS `0xD027`" IS STALE — DO NOT USE IT.** ✔RE-MEASURED 2026-08-15: `0xD027` was
  allocated and then **WITHDRAWN**, and is a permanent HOLE that must not be back-filled (renumbering
  rewrites a published `error[Dxxxx]`). Since then `0xD028` `D_ArtifactProfileNoServingFormat`,
  `0xD029` `D_DependencyBuildFailed` and `0xD02A` `D_LanguageTargetIsaMismatch` landed.
  ✔**NEXT FREE IN THE `D_` BAND IS `0xD02B`.**
  ★ **STOP READING THAT NUMBER OUT OF THIS FILE.** A stale next-free slot in a lane brief is exactly
  how two lanes collided on `0xD029` this cycle. Ask the header instead:
  `python tools/check-diagnostic-codes.py` prints the next free ordinal **per band** on its green
  path, derived from the enum itself.
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
3. **Legs — ✅ ALL FOUR GREEN.** ✔**Windows MSVC-Debug 864/864**, 0 warnings. ✔**qemu-aarch64 STRICT
   863/864** (`DSS_STRICT_ARM_VERDICTS=1`, qemu + sysroot present, so skips would have HARD-FAILED) —
   same single `line_endings_guard` artifact as the gcc leg, same disproof. ✅ **macOS (Apple Silicon,
   26.5.2, Xcode clang) — BUILD rc=0 and the AP5/AP6 ctest surface 9/9 PASSED** over a FRESH CLONE of
   `6a4dac6`, with the checkout asserted to contain `DSS_SPAWN_USE_POSIX_SPAWN` (3 occurrences) BEFORE
   building so the build could not be vacuous. ★ **This is what closed
   `D-SPAWN-APPLE-POSIX-SPAWN-ARM-COMPILED-BY-NO-LOCAL-LEG`** — an Apple-only mechanism that no local
   leg compiled, and both new corpus examples' **Mach-O binaries executed on real arm64 hardware for
   the first time**. ✔The CRLF worktree defect behind the two `line_endings_guard` reds is FIXED at
   source (`dependency_resolver.cpp` normalized; `git hash-object` == `HEAD` blob, so a provable
   no-op).
   ⚠ **THREE clang-only warnings, NONE of them AP5/AP6 and NONE introduced by this PR** — googletest's
   own header, `tests/core/test_diagnostic_reporter.cpp:289` (`-Wdangling-gsl`), and
   `src/asm/asm_text_to_lir.cpp:2561` (`-Wswitch`, enumerator `Switch` unhandled). ✔That last is in a
   file this PR touched but NOT in a region it changed (the PR's only hunk there is line 3357). All
   three are invisible to the MSVC and gcc legs. **DISCLOSED, not created** — registering them is
   blocked by the same counting rule as the 219 (§1.5), which is why they are recorded here.
4. **Legs (historical detail).** ✅ **WSL x86_64 (gcc) GREEN** — ✔native ext4 build, **BUILD_OK, 0 warnings**, ctest
   **863/864**; the single failure was `line_endings_guard` and it was **MY HARNESS, not the repo**:
   the run rsynced the Windows WORKING TREE, which carries `w/crlf` on `dependency_resolver.cpp`,
   while ✔`git ls-files --eol` shows **ZERO `i/crlf`** committed blobs. Re-verified against a
   **faithful `git clone` of the pushed branch inside WSL: 0 CRLF files, `line-endings: OK`** (2418
   committed text blobs). ★ **This leg is what CONFIRMS the CR-byte fix** — `examples_runner.cpp` now
   compiles under gcc, which is the toolchain that rejected it.
   ⚠ **TWO HARNESS LESSONS FROM THIS LEG, both of which produced a fake result before being caught:**
   (a) `rsync --exclude 'build*'` also excludes **`build_scripts.cpp`** — use directory-only patterns
   (`/build/`, `/build-*/`) and assert a known source arrived before configuring; (b) **rsyncing a
   working tree is not a faithful CI simulation** — line endings differ from the committed state, so
   use `git clone` when the thing under test is a property of the committed tree.
   ⛔ **REMAINING: the qemu-aarch64 leg**, then the PR is fully evidenced.
   📄 **PR [#53](https://github.com/dailysoftwaresystems/dss-code-prime/pull/53) is OPEN** against
   `main`; `867fa81` + `e3fd4e1` pushed, both UNSIGNED (see §4).
4. **NEXT CYCLE — ✔OPERATOR-RULED 2026-08-14: BOTH, IN ONE CYCLE.** (a) plan-06 §5.1
   **B.12-CORRECTED** — the declared `emitsArtifact` column, the load-time bidirectional rule, the
   corrected impostor test, the three-way diagnostic split, and AP6's SourceMerge example dropping its
   `targets[]` **and its comment**; (b) the anchor-guard work — counting rule first, then the
   row-vs-prose predicate honouring the wrap invariant (§1.5), then registering the **219**.
   📄 They parallelise cleanly by **disjoint file sets** — (a) is `src/core/types/artifact_profile.hpp`
   + `project_config.{hpp,cpp}` + `tests/program/test_project_config.cpp` + the corpus example;
   (b) is `tools/check-anchor-*.{sh,ps1,py}` + `.plans/_deferred-anchor-registry.md`. No overlap.
   ⚠ **THE RISK THE OPERATOR ACCEPTED, STATED SO THE NEXT CYCLE MITIGATES IT RATHER THAN REDISCOVERS
   IT:** this cycle demonstrated twice that a large multi-lane cycle is exactly where verification
   claims go unchecked — a lane reported green over a test its build dir never compiled, and the
   orchestrator read a false pass off a piped exit code. ⇒ **every lane must name the build directory
   its claim was measured in, and show the subject binary exists there.**

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

★★ **THE TRAP IS ENTIRELY IN STEP (2), SO STATE THE INVARIANT BEFORE WRITING THE CHECK.** ✔The 203
legitimate wrapped fragments pass TODAY **because** the guard matches on SUBSTRING. A naive *"every
citation must have a row"* check **REDS ALL 203**. The wrap contract and the row requirement must be
satisfied by **ONE predicate, not two that disagree**: a WRAPPED citation resolves to its full anchor
name FIRST, and only then is the row requirement applied. **Prove it with a fixture carrying one
wrapped citation WITH a row (must PASS) and one wrapped citation WITHOUT a row (must FAIL).** If both
cannot be satisfied at once, the wrap FORMAT is itself the defect and that is a different row.
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
📄 **DCO:** every commit needs `Signed-off-by` (`git commit -s`).
★★ **THE RULE CHANGED 2026-08-14 — OPERATOR AUTHORIZATION, RECORDED HERE BECAUSE IT OVERRIDES A
STANDING PROHIBITION.** This file previously read *"an agent must not add it on the operator's
behalf"*, on the ground that a sign-off is a legal attestation in a named human's name. ✔The operator,
asked directly and shown that the branch carried no pre-DCO history, answered: *"push unsigned now,
**remember to sign from now on**… I can merge this once the PR is fully finished."* ⇒ **from
2026-08-14, cycle commits ARE signed (`git commit -s`)**, on the authority of the named human whose
attestation it is. ⚠ The authorization is recorded rather than merely obeyed **because the agent that
acts on it is not the one who can grant it** — a future cycle finding a signed commit must be able to
see who permitted it and when, or the attestation is unauditable, which is the very property that made
the prohibition right in the first place.
⚠ **`867fa81` and `e3fd4e1` are UNSIGNED** — they predate the authorization and were pushed unsigned
by explicit instruction. ✔MEASURED: they are the ONLY two commits on this branch, so a sign-off
rebase reaches all of it; there is no pre-DCO tail that would leave the check red after amending only
the tip. The operator merges the PR when it is finished, so the sign-off decision on these two is
theirs to take at that point.

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
