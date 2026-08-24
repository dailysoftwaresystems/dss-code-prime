# NEXT-SESSION BRIEF — handed over 2026-08-14

> Read `.plans/_handoff.md` first for full state. This file is the **actionable work list** only.
> Branch `feature/finish-hooks-and-dependson-surface`, PR **#53** open, tree clean, 4 commits pushed.
> ✔Legs: Windows **864/864** · WSL gcc **863/864** · qemu-arm64 strict **863/864** · macOS **9/9 AP5/AP6**.
> ✔Anchor balance **982 → 982, net 0, OK**. ✔Anchor guard **OK (1154)**. ✔Next free diagnostic slot **`0xD027`**.

---

## 0. ⛔ FIRST — AP6 IS BUILT BUT NO PLAN SAYS SO (PR-scoped, do this before anything else)

✔MEASURED 2026-08-14: the AP6 code is landed, committed and green on four legs, but **every plan still
declares it unbuilt.** A cold reader consulting the plans would conclude the resolver does not exist.

- `.plans/06-artifact-profile-plan - tbd.md` §5 PR-breakdown table has rows `~~AP1~~ ✅` … `~~AP5~~ ✅`
  and **NO AP6 ROW** (✔`grep -c "AP6.*✅"` → **0**). Add one, in the same shape as the AP5 row.
- `.plans/00-*.md` §0.1 says **"AP6 = `dependsOn` RESOLUTION, not yet landed"** (line ~42) and
  *"AP6 … is scoped and decided but unbuilt"* at lines ~85, ~313, ~396. All four are now false.
- Plan-06 §7 acceptance criteria need AP6's boxes ticked.

**What to write into the AP6 row** (all ✔MEASURED, safe to state): the resolver
(`src/program/dependency_resolver.{hpp,cpp}`) with canonical-path cycle detection (`0xD01A`), diamond
memoization and a depth cap (`0xD026`); composition dispatched on the dependency's OWN verb;
**B.10 consumer-driven derivation** (a dependency's own `targets[]` is never consulted — the
ArtifactLink corpus example builds **four** archives from a manifest declaring **one** ELF spec);
git acquisition (`IGitRunner`, `.dss-deps` four-outcome cache, `dss-lock.json`, `--force-git-cache`
as a GLOBAL flag — there is no `build` subcommand); the per-target library channel keyed by target
spec; the wrong-format `--resolve-library` guard at one chokepoint reaching both binders; and two
runnable corpus examples, both arms, `path` deps only per B.7.

---

## 1. THE NEXT CYCLE — ✔OPERATOR-RULED: BOTH, IN ONE CYCLE

They parallelise by **disjoint file sets**, so run them as two lanes.

### 1a. plan-06 §5.1 **B.12-CORRECTED** — `emitsArtifact` column
Read B.12-CORRECTED in full; the withdrawn format-table derivation is retained above it as an audit
trail and **must not be built**.
- Add `bool emitsArtifact` as a **NEW COLUMN** on `ArtifactProfileRow`
  (`src/core/types/artifact_profile.hpp`), true for all ten profiles **except `module`**.
  ⛔ NOT a fourth `DependencyComposition` enumerator — that enum is the consumer-side `dependsOn`
  axis and composition is unchanged here.
- **Bidirectional rule, enforced at LOAD time** in `project_config.cpp`:
  `emitsArtifact == true` ⇒ `targets[]` REQUIRED non-empty; `false` ⇒ declaring `targets[]` is a
  **LOAD ERROR**. ✔The layering objection is DISSOLVED: `artifact_profile.hpp` and
  `project_config.cpp` are in the SAME directory and the same `core` library.
- **★ The impostor test is mandatory:** flip the DECLARED property in a fixture, never the format
  table. `emitsArtifact=false` on a true profile ⇒ `targets[]` becomes a load error; `true` on
  `module` ⇒ becomes required. A hardcoded `if (profile=="module")` or `if (verb==SourceMerge)`
  passes every other test and **fails this one**.
- Also pin: `cli`/`lib` without `targets[]` ⇒ still a load error (proves CONDITIONAL, not deleted).
- **Migration:** `examples/c/project_dependson_module_source_merge/scalemod/.dss-project.json`
  DROPS its `targets[]` **and its explanatory comment**. ⚠ ✔It still has `targets[]` today
  (1 occurrence) — if the comment survives, the fix did not land.
- **Separate row to open** (disclosed, not created): the six case-(B) profiles
  (`gui`,`script`,`sproc`,`transpile`,`shader`,`hdl`) are registered, emit artifacts, and are
  implemented by NO shipped format, so building one misreports `D_ArtifactProfileFormatMismatch`
  ("you picked the wrong format") when the truth is "no format implements this anywhere". The
  diagnostic splits three ways — see B.12-CORRECTED.

### 1b. The anchor guard — **the counting rule FIRST**
✔MEASURED: **1304** distinct `D-*` cited in scanned roots; **882** have a real row; **422** rowless;
**203** of those are legitimate wrapped fragments; ⇒ **219 GENUINELY ROWLESS** — they pass only
because some plan's prose mentions them. The guard reports `OK (1154)` over that.
📄 Operator ruling: *"THE GUARD BUG OUTRANKS THE BALANCE… the balance gate forbids a cycle that OPENS
NEW debt; it does not forbid a cycle that DISCLOSES PRE-EXISTING debt. Fix the counting rule, do not
take the §B shrug and do not suppress the rows."*
1. **Counting rule** — teach `scripts/check-anchor-balance/check-anchor-balance.py` to count DISCLOSED-pre-existing rows
   separately from CREATED ones. Until this lands, honest disclosure is penalised.
2. **Row-vs-prose predicate** — ⚠ **THE TRAP IS ENTIRELY HERE.** The 203 wrapped fragments pass
   *because* matching is by SUBSTRING. A naive "every citation needs a row" check **reds all 203**.
   One predicate, not two: resolve a wrapped citation to its FULL anchor name **first**, then apply
   the row requirement. **Prove it with a fixture: one wrapped citation WITH a row (must pass) and
   one WITHOUT (must fail).** If both cannot hold, the wrap FORMAT is the defect — different row.
3. **Then register the 219** under the new counting rule.
4. Also finish `D-GATE-ANCHOR-GUARD-SCOPE-STILL-EXCLUDES-TOOLS-AND-TESTS`: ✔`tests/` +
   `integrated_tests/` widening LANDED this cycle; **`scripts/` and `tests/` are still unscanned** —
   that half keeps the row open. ⚠ `D-32-BIT-WORD` is NOT an instance: it only ever appears inside
   the longer phrase `FIXED-32-BIT-WORD`, which the guard's `\b` correctly skips.

---

## 2. DISCLOSED THIS CYCLE — need rows once the counting rule allows it

| Finding | Evidence |
|---|---|
| **CLI corpus runner is blind to `optimizedPipelines` / `expectedStdout`** | ✔0 occurrences of `--config`/`shippedPipeline`/`optimizedPipelines` in `integrated_tests/runner.cpp`, so **473** manifests' optimizer arms get no CLI-surface verdict. The driver already has `--config=release\|debug`, so the fix is teaching the runner the flag. ⓘ NOT a product gap — `--config` is the shipped way to pick a pipeline and `.dss-project.json` carries no pipeline field, which is correct. |
| **A `shippedPipeline: "release"` arm can be byte-identical to baseline, undetected** | ✔The AP6 staticlib example was exactly that until an inlinable helper was added: release and debug emitted the SAME pe64 bytes, so the arm asserted a no-op stayed a no-op. Nothing in the harness detects it. |
| **Three clang-only warnings — pre-existing, NOT this PR** | ✔macOS leg: googletest's own header; `tests/core/test_diagnostic_reporter.cpp:289` (`-Wdangling-gsl`); `src/asm/asm_text_to_lir.cpp:2561` (`-Wswitch`, enumerator `Switch` unhandled). ✔The last is in a file this PR touched but NOT in a region it changed (the PR's only hunk there is line 3357). All three are invisible to the MSVC and gcc legs. **`-Wswitch` on an unhandled enumerator deserves a look — it is the silent-fallthrough class.** |

---

## 3. THE MAC-BLOCKED BACKLOG — legacy, NOT AP5/AP6, triage before treating as a work list

✔MEASURED: **ZERO** of these appear in this PR's registry diff. All pre-date the branch.
⚠ The list came from a keyword match over row text, so it is an UPPER BOUND — at least one
(`D-HARNESS-PS1-WINDOWS-LEG-UNEXECUTED`) matched on wording and is not Mac-blocked at all.
**Triage first; do not cost it at face value.** A Mac is now reachable (`scripts/ssh-macos/ssh-macos.sh`, and
mDNS may not resolve from Git Bash — pass a literal IP via `DSS_MACOS_HOST`).

`D-HARNESS-CROSS-HOST-ANY-TARGET` · `D-CSUBSET-ATTRIBUTE-ACCESS-NO-CLANG-CLEAN-WITNESS` ·
`D-FFI-SHIPPED-LIBS-OS-ONLY` · `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED` · `D-FFI-ZLIB-DESCRIPTOR` ·
`D-FFI-DESCRIPTOR-EAGER-IMPORT` · `D-CSUBSET-TGMATH-SURFACE` · `D-CSUBSET-PER-TARGET-STRUCT-LAYOUT` ·
`D-FF1-MACHO-READER` · `D-LK1-MACHO-X8664-DARWIN-DATA-SECTION` · `D-LK-MACHO-X8664-DYLIB-RUNTIME` ·
`D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION` · `D-CSUBSET-DARWIN-SEMANTIC-RESIDUAL-COHORT` ·
`D-CSUBSET-INLINE-FUNCTION-SPECIFIER` · `D-LK-MACHO-STACK-RESERVE-LC-MAIN` ·
`D-LK-MACHO-BSS-ONLY-LINKEDIT-STRUCTURAL-PIN` · `D-FFI-SHIPPED-SYMBOL-PER-ARCH-LINK-NAME` ·
`D-HARNESS-PS1-WINDOWS-LEG-UNEXECUTED` *(false positive)* · `D-SQLITE-CLI-BUILT-ON-NO-LEG` ·
`D-SQLITE-CLI-TERMINAL-WIDTH-SHADOWED-SILENTLY` · `D-MIR-VA-OVERFLOW-ARM-DROPS-FIXED-STACK-DISPLACEMENT` ·
`D-TEST-CROSS-ARCH-SKIP-YIELDS-NO-VERDICT` · `D-LK3-DYLIB-TLS-MODEL` · `D-LK3-DYLIB-WEAK-EXPORT` ·
`D-FFI-RESOLVE-LIBRARY-DEMANDS-A-BINARY` · `D-HARNESS-RUN-ENV-LD-LIBRARY-PATH-INERT-ON-DARWIN` ·
`D-HARNESS-ELF-LEG-HOST-SYSTEM-PROVIDER-UNSATISFIABLE-OFF-LINUX` ·
`D-HARNESS-MACHO-IMPORT-NAME-IS-LOADER-PATH-NOT-A-SYSTEM-PATH` · `D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE` ·
`D-TEST-PLATFORM-GUARDED-ARM-COMPILES-ON-NO-GATE-LEG`

---

## 4. OPERATOR-CARRIED, do NOT close autonomously

- `D-CI-DCO-GATE-IS-ADVISORY-A-PR-MERGED-WITH-IT-RED` — ✔PR #52 merged with DCO red ⇒ the check is
  advisory, contradicting the workflow's own contract. Enforce in branch protection, or retire it.
  ⚠ `867fa81` and `e3fd4e1` are UNSIGNED (they predate the sign-off authorization); `6a4dac6`,
  `293d069` and `b93a410` are signed. The branch is exactly these commits, so a rebase reaches all.
- `D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` — **TRIGGER-GATED, do not build.** Fires on the first
  real dependency that must decline a target. The row records the ENUMERATION design already
  measured and rejected, so it is not re-proposed.
- `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER` and `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED` — **must NOT
  be closed**; building either is the speculative build §A.2 forbids in the other direction.

---

## 5. PROCESS RULES THIS CYCLE PAID FOR — carry them forward

- **A lane's green is only as good as the build directory it was measured in.** ✔A lane reported
  green over `test_dependency_git_cache` while its `build-ap6-d1/` contained **no binary for that
  target at all**. Every lane must name its build dir and show the subject binary exists there.
- **Never read a verdict off a piped command's exit code.** ✔A `ctest … | tail` reported exit 0 —
  `tail`'s status — over a run that died at test 11 of 268.
- **Two concurrent ctest runs in one build dir contend and yield NO verdict.** Run the gate only when
  no lane is live in `build/`.
- **Rsyncing a working tree is not a faithful CI simulation.** ✔Line endings differ from the
  committed state and produced `line_endings_guard` false reds on two legs. Use `git clone`.
  Also: `rsync --exclude 'build*'` silently excludes `build_scripts.cpp` — use directory-only patterns.
- **Sweep lane build dirs at end of cycle.** ✔13 lane dirs held **99 GB** for this one cycle.
