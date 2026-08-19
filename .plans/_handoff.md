# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-08-19 — cycles **P14 + P15 + P16 + P17 + P18**. **P18 WITHDREW an anchor rather than closing it by building what it asked for**: `D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED` demanded a guard that every `.sh` have a `.ps1`, and ✔11 of 21 script directories correctly have none. The rule moved into the two skills as a judgement the author makes and writes down. Earlier:  **P17 is an operator-inserted cycle**: `tools/` was merged into `scripts/` under the one-directory-per-script convention, every script now declares a `PURPOSE:` line, and two generated indexes (`scripts/README.md` + the `/dss-cycle` skill's `references/scripts.md`) are held to the tree by a new `scripts_index_guard`. It also closed a gate that had been running **one test at a time on every host**. Earlier:  **P14 OVERTURNED its own premise**: there is no pe64 miscompile. The `scanstatus2-5.1` abort is an UPSTREAM sqlite portability bug (`sprintf("ptr:%p")` vs Tcl's `format "ptr:0x%llx"`), proven by a discriminating pair on the one crashing binary. P14 also opened `D-FFI-PE-DIRECT-H-TRANSITIVELY-EXPOSES-THE-WIN32-SURFACE`, and **P15 WITHDREW it — its central claim was false** (see §0.000000000000000). What survives is the operator ruling it triggered, now **bar §A.3b**: *the goal is to WORK; one working reference makes the behaviour REQUIRED*, ✔witnessed by `cl` compiling the construct rc=0 clean. P13/P12/P11 below.
**Branch:** `feature/c23-conformance-burndown-3` · **HEAD:** this commit (Cycle P18). ⚠ **Any path spelled `tools/…` in a commit message or a row older than 2026-08-19 is HISTORICAL, not stale** — that directory no longer exists; every script lives at `scripts/<name>/<name>.{sh,ps1,py}`. ⚠ The P14 WIP chain `8f1b3963`→`08989144` is pushed and its commit MESSAGES assert a miscompile that does not exist — read this file, not those subjects.

---

## 0.00000000000000000 ★★★ THE OPERATOR-SELECTED QUEUE — TICK EACH BOX AS IT LANDS

**Selected by the operator 2026-08-19** from the AP5/AP6 commit + asm-feature (this PR) pending deferrals, in
their stated ORDER: **pending feature completion → production errors → harness/test errors → others.**
★ **THIS IS THE LIVE QUEUE. Re-derive each pick from the REGISTRY before starting it** (a list is a pointer, not
a verdict — twice now an item on a list like this was already closed underneath it), tick its box in the SAME
commit that lands it, and never delete a ticked row.

- [x] **P16 — `D-UPSTREAM-SQLITE-FILEIO-WINDIRENT-IS-MSVC-ONLY` + `D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE`. ✅ DONE 2026-08-19.**
      BOTH CLOSED, net −1. The oracle-collapse anchor had been **cited since 2026-08-18 without ever existing** —
      written and fixed in the same commit. The windirent row closed because its central claim is measured false:
      DSS **and MSVC** both compile `ext/misc/fileio.c`; only mingw-on-Windows does not, which is a configuration
      upstream never claims. ⇒ nothing to report upstream for THAT one.
- [x] **P17 — OPERATOR-INSERTED, not from this list: the `scripts/` consolidation + the script index. ✅ DONE 2026-08-19.**
      Six rows, all BORN CLOSED, net 0 — two of them opened by the INDEPENDENT AUDITS at the end of the
      cycle rather than by the work itself, and a third rewritten because an audit measured its central
      claim false. `tools/` (24 tracked files) merged into `scripts/` one-directory-per-script;
      **17 repo-root derivations** were one level short afterwards and each was repaired and asserted;
      every script declares a `PURPOSE:` line; `scripts/README.md` and `references/scripts.md` are GENERATED
      from those declarations and held to the tree by `scripts_index_guard` (11 red-on-disable arms).
      `/dss-cycle` now MANDATES that a script added/renamed/deleted/repurposed updates the reference in the
      same commit. Also: the gate ran **serially on every host** — `run-gate` and `local-build` now default
      `CTEST_PARALLEL_LEVEL=8` (✔9741 ms → 2648 ms on six tests; the full suite was 899 tests / 2602 s).
- [x] **P18 — OPERATOR RULING: `D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED` WITHDRAWN. ✅ DONE 2026-08-19.**
      The anchor asked for a gate that every `.sh` carry a `.ps1`. ✔MEASURED: **11 of 21 script directories
      have no `.ps1` and every one is correct** — eight are Python (cross-platform already), two are POSIX-only
      by nature (`wsl-leg` runs inside a WSL distro; `profile-compile` drives a POSIX toolchain). A gate cannot
      tell a deliberate POSIX-only script from a forgotten twin, so it would need an eleven-entry allowlist —
      the convention written twice, reddening honest work by default. Rule now lives in `dss-cycle/SKILL.md`
      and the `dss-code-prime` skill (conventions authority). Net −1.
- [ ] **P19 — `D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` (label half).** Trigger FIRED. The operand half and
      the `%%` half are DONE; this is the asm-goto BLOCK-binding design and the last piece of the feature.
      Expect a real design fork — bring it as a §B rather than guessing.
- [ ] **P20 — `D-EXAMPLES-DEPENDSON-NO-RELEASE-OPTIMIZER-ARM`.** Trigger FIRED. AP5/AP6's `dependsOn` path has
      never been touched by the release optimizer on ANY leg, so a release-only fault in it is invisible today.
- [ ] **P21 — `D-HARNESS-PE64-LIB-ACQUISITION-IS-HOST-DEPENDENT` (HIGH).** Trigger FIRED; the only HIGH in either
      family, and it gates the pe64 sqlite CLI. ✔Measured to shrink to ONE library for the CLI case.
- [x] **OPERATOR CALL — ✅ FILED 2026-08-19: <https://sqlite.org/bugs/forumpost/97cd29ca44624113c73b30f5d2504729e6ffc5c5ebcba137078ea1a868cd97c9>.**
      The `%p` half only, which is the half that warranted it. ✔The toolchain table is what makes it
      compiler-independent: `sprintf("ptr:%p")` yields `ptr:0000007BD12FFB50` on **MSVC**, `ptr:00007ffffe2ffedc`
      on **mingw** (`ptr:00007FFFFE2FFEDC` with ANSI stdio off) and `ptr:00007FFFFF924B88` on **DSS**, against
      glibc's `ptr:0x55f10ad6f3a8` — so **not one Windows C runtime** emits the `0x` that
      `test/scanstatus2.test:268` rebuilds, while `src/test1.c:78` keys the registry with `%p`. Framed as a
      TEST-HARNESS defect; nothing in the library is implicated.
      ⚠ **The pe64 corpus leg STAYS RED at `scanstatus2-5.1` until upstream acts** — the standing rule forbids
      patching the staged tree or excluding the file, and a filed report does not change that. The redness is now
      named, root-caused, non-DSS, and reported.
      ⓘ **(b) the `fileio.c`/`windirent.h` `_MSC_VER` interlock was RETIRED, not filed** — ✔`cl` compiles that TU
      rc=0 because upstream ships Windows through `Makefile.msc`; only the mingw-on-Windows path breaks, a
      configuration upstream never claims. A reference works ⇒ by bar §A.3b DSS working is REQUIRED.

**ALSO IN BUCKET 1, NOT YET SELECTED** (offer them when the four above are done):
`D-CONFIG-ASM-TEMPLATE-CAPABILITY-NO-LONGER-OPTIONAL-FOR-A-SHARED-SURFACE-IMPORTER` (trigger FIRED) ·
`D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` (⚠ **re-measure first** — it predates the CFI producer landing).
**BUCKET 2 (production errors) IS EMPTY IN THESE TWO FAMILIES** — every asm/AP5 row is trigger-gated, fail-loud
or LOW. The live user-facing one on this branch is `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED` (HIGH: `S*`
diagnostics are never remapped through `#line`).
⛔ **BUCKET 4 — DO NOT PICK, and say so rather than silently skipping:** §B-gated
`D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` + `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL`;
trigger NOT fired `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER`, `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED`,
`D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED`, `D-CSUBSET-BARE-ASM-ACCEPTANCE-IS-UNCONDITIONAL`; SUSPECT
`D-MIR-SEH-FILTER-CLONER-HAS-NO-INLINE-ASM-ARM` (verify or discharge, never patch on suspicion); LOW
`D-ASM-TEMPLATE-IS-LEXED-TWICE`, `D-CSUBSET-ASM-LABEL-ON-SYNTHESIZED-SHIM-SYMBOL`,
`D-ASM-AARCH64-FRAME-OFFSET-BEYOND-SCALED-IMM12`, `D-ASM-AARCH64-FRAME-OFFSET-BEYOND-2GIB`.
ⓘ Two rows a family-grep catches that are NOT defects: `D-OPT-REBUILD-POLICY-NEUTERED-STATE-HOOK` is a 🟢
SHIPPED design record, and `D-TEST-MACOS-LEG-EMSDK-PROFILE-REPLACES-PATH-HIDING-HOMEBREW` is a 🔵 DISCLOSED
pre-existing environment fact.

---

## 0.00000000000000000000 ★★★ CYCLE P18 — AN ANCHOR WITHDRAWN BECAUSE WHAT IT ASKED FOR WOULD HAVE BEEN WRONG

**One row closed, net −1. No code changed.**

`D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED` had been OPEN since 2026-07-29 demanding a guard that every `.sh`
carry a `.ps1` twin. **Operator ruling 2026-08-19:** *"some scripts are posix executed only, and don't have
a .ps1 pair. so we must just enforce the dss cycle and dss code prime skills to always create the pair,
except when the execution is posix only. and this enforcement D-GATE-SCRIPT-PS1-PAIRING-UNCHECKED must not
exist."*

⭐⭐ **THE ROW'S PREMISE IS MEASURABLY FALSE.** It reads an unpaired `.sh` as a missing twin. ✔At closure
**11 of 21 script directories carry no `.ps1`, and every one is correct**: eight are Python — a `.py`
already runs on both hosts, so a twin would be a second implementation of something never split — and two
are POSIX-ONLY BY NATURE (`wsl-leg` runs inside a WSL distro where PowerShell is not the shell;
`profile-compile` drives a POSIX toolchain over a carriage).

★★ **A GATE CANNOT TELL A DELIBERATE POSIX-ONLY SCRIPT FROM A FORGOTTEN TWIN**, so an existence check
would have to be fed an allowlist of eleven exceptions — the convention written twice, in the place least
likely to be read, reddening honest work by default. **The rule is now a judgement the author makes and
writes down**, stated in `dss-cycle/SKILL.md` and in the `dss-code-prime` skill (the conventions
authority): create the twin whenever the capability must reach the Windows leg; omit it — and say so in
the header — when the script is already cross-platform or POSIX-only by nature; and where a pair does
exist, the two must not drift.

ⓘ **The row's second half survives as a rule rather than a gate:** pairing by EXISTENCE is not pairing by
BEHAVIOUR. `scripts_index_guard` covers the sharp edge (a sibling whose `PURPOSE:` contradicts its primary
is refused); the rest is the same-commit discipline the skills state.

## 0.000000000000000000 ★★★ CYCLE P17 — THE REPOSITORY NOW HAS AN INDEX OF ITS OWN SCRIPTS, AND THE GATE STOPPED RUNNING ONE TEST AT A TIME

**Six rows, all born CLOSED, net 0.** Operator-inserted. Two of the six were opened by the INDEPENDENT
AUDITS at the end of the cycle rather than by the work itself — see the audit paragraph below.

**HALF ONE — `tools/` AND `scripts/` BECAME ONE DIRECTORY, WITH AN INDEX.** ✔MEASURED before: 24 tracked
files in `tools/`, 4 script directories in `scripts/`, **408 references across 49 files**, no rule saying
which directory a new script belonged in, and — the actual defect — **no index of any kind**. Eighteen
scripts, named only piecemeal across EIGHT reference files wherever a gate step happened to need one.
★★ That makes the operator's rule unreachable: *"if a tool has a problem, fix before using again, not
workaround an own tool"* cannot be followed if nothing lists the tools. Now: every script lives at
`scripts/<name>/<name>.{sh,ps1,py}` with its siblings and assets inside its own directory; each declares
its purpose ONCE in a `PURPOSE:` line; `scripts/README.md` and `.claude/skills/dss-cycle/references/scripts.md`
are **GENERATED** from those declarations; and `scripts_index_guard` reds when the tree and either index
disagree. ⚠ A hand-written pair would have been two copies of one fact — the failure the whole
deferred-anchor discipline exists to answer — so the index is derived, never authored.

⚠⚠ **THE PART THAT WOULD HAVE FAILED SILENTLY:** **17 repo-root derivations** walked up from a script's
own location and were all one level short after the move. A wrong root does not throw — the guard scans a
smaller tree and reports a clean pass. Each was repaired and each edit asserted to apply exactly once;
✔all six ctest-wired guard twins then re-ran green from their new homes, `check-orphan-tests`'s own 12-arm
self-test included. ★ **Six** `tools/` strings were deliberately NOT rewritten and ALL are named in the row so a
future sweep does not "fix" them (a spec example for a USER's project, a synthetic spawn fixture, an
operator's `.dotnet/tools/pwsh.exe`).

★ **AND THE MOVE FOUND A GUARD THAT ASSERTED NOTHING.** The wsl-invocation rule in
`test_sqlite_harness_legs.cpp` says its coverage is BY DIRECTORY so a new script is governed the day it
lands — and enumerated that directory with a `directory_iterator` that **swallowed its error_code and
checked no count**. Moving the directory would have emptied the range while the test kept passing. It is
now recursive over `scripts/` with a floor, and the retarget WIDENED coverage to four scripts the rule had
never governed (✔none mentions `wsl`, so no new red).

**HALF THREE — THE AUDITS, AND THEY CHANGED THE CYCLE.** Three independent read-only audits ran over the
finished work. They found a HIGH break the migration had caused and hidden (`profile-compile-support.py` built
its gate path from COMPONENTS, so `tools/` appeared nowhere in the file, the sweep could not see it, and
`timed-gate` died on every leg while its diagnostic named a file that was fine); they measured **six** of this
cycle's own numeric claims wrong, including three wearing a ✔MEASURED label; and they got the new
`scripts_index_guard` to report GREEN over six conditions it claimed to refuse. ⚠⚠ **The most important finding
was that the guard's red-on-disable claim in its own row was FALSE** — deleting the floor outright left the
self-test printing *"PROVEN able to fail"*, because the floor arm asserted only an exit code that several
refusals share. The guard was rebuilt: **28 arms**, every red arm asserting the MESSAGE of the refusal it names,
and nine refusals sabotage-tested one at a time.

★★★ **AND THE OPERATOR CORRECTED THE CYCLE'S ANSWER TO ITS OWN WORST FINDING.** Inserting a `# PURPOSE:` line
moved **16** plan citations off their subjects; the rows written to record that then shipped three more wrong
numbers. The plan here was to build a checker that re-resolved citations against `HEAD` — keeping the fragile
reference and detecting its breakage after the fact. The operator's rule instead removes the fragility:
*"we must never document line numbers, we must document method names, comment ids or defined anchors."*
`plan_citations_guard` now refuses a new positional citation, as a ratchet over the **2376** that already exist
(2074 in the registry alone, nearly all in closed historical rows). ✔This cycle burned its own down, **2376 →
2365**, converting all eleven citations it had authored.

**HALF TWO — THE *LOCAL* GATE RAN SERIALLY ON EVERY HOST, BECAUSE NOTHING SUPPLIED A LEVEL.**
⚠ *Local*, and the qualifier is a correction: `.github/workflows/pipeline-pr.yml` (its `ctest --stop-time` step) has passed
`--parallel 4` all along and is untouched by this cycle, so "everywhere" was false. ✔MEASURED:
`local-build.{sh,ps1}` invoked `ctest --output-on-failure` with no `-j`, and `run-gate`'s own usage example
showed the same form, so the form propagated by being copied. The full Windows suite: **899 tests / 2602 s,
one at a time, on a 16C/32T box.** Six example tests: no level **9741 ms**; `CTEST_PARALLEL_LEVEL=8`
**2648 ms**; explicit `-j 8` **2446 ms**; env 8 with an explicit `-j 1` **9669 ms** — so the env var is
honoured *and* a caller's flag still wins, which is what makes this a DEFAULT rather than a policy.
`run-gate` and `local-build` now default it to **8** (operator's number: the Windows and WSL legs run
concurrently here by design, so the default leaves headroom instead of claiming the box). ✔**THE WHOLE-SUITE FIGURE IS MEASURED, NOT EXTRAPOLATED:** through `run-gate` at the new default the
suite ran **900 tests in 803.1 s** against serial **899 / 2602.6 s** — **3.24×**, while a clean WSL build
and three reasoning agents competed for the same box, so it is pessimistic. ⓘ The floor under any
parallelism is `integrated_tests` at **566.56 s** (both figures from this cycle's own P16 serial gate log), a quarter of the serial total in one entry — which is why
the speedup is ~3× and not ~8×.

## 0.0000000000000000 ★★★ READ THIS FIRST — CYCLE P14 (COMPLETE): THE pe64 "MISCOMPILE" DOES NOT EXIST. IT IS AN UPSTREAM SQLITE PORTABILITY BUG, AND THE REASON WE BELIEVED OTHERWISE IS ITS OWN DEFECT.

**✔MEASURED, and it overturns the two cycles above this line.** `D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2`
is **CLOSED / DISCHARGED**: there is no DSS codegen defect at `scanstatus2-5.1`. Do not spend another cycle
hunting one.

**THE ROOT CAUSE.** `testPointerToString` (sqlite `src/test1.c`) registers every handle in a `Tcl_HashTable`
under a key built with **`sprintf(aKey, "ptr:%p", pPtr)`**. `scanstatus2.test`'s test-5.x `trace` proc rebuilds
that handle in Tcl as **`format "ptr:0x%llx" $stmt`**. Those agree only where the C library's `%p` emits
`0x`-prefixed minimal-width lowercase hex — glibc and macOS, not Windows. On Windows the lookup MISSES,
`testStringToPointer` returns 0, and `sqlite3_stmt_scanstatus_v2(NULL, …)` dereferences `[NULL+0x88]`.

**THE FOUR MEASUREMENTS** (each re-runnable; full detail in the registry row):
1. **The discriminating pair, on the SAME crashing binary.** Registry spelling `ptr:00007FFFFF925998` → **rc=0,
   no crash**. Test spelling for the SAME pointer, `ptr:0x7fffff925998` → `wine: Unhandled page fault on read
   access to 0000000000000088 at address 00000001405E0BC2` — address and RIP both identical to the recorded
   fault. One binary, one pointer, two spellings, opposite outcomes.
2. **The `%p` spellings.** DSS pe64 under wine: `ptr:00007FFFFF924B88`. DSS elf64: `ptr:0x386bd348`.
3. **The reference control, on the RIGHT platform.** `x86_64-w64-mingw32-gcc` under wine prints
   `ptr:00007ffffe2ffedc` (default ANSI stdio) and `ptr:00007FFFFE2FFEDC` (`-D__USE_MINGW_ANSI_STDIO=0`) —
   **neither carries `0x`**. Any Windows build of upstream hits this, DSS or not.
4. **The other leg.** The DSS-built **elf64-x86_64** testfixture runs `scanstatus2.test` to **0 errors out of 24
   tests**, 5.1 included — same C, same optimizer, same MIR→LIR.

**⚠⚠ WHY WE BELIEVED IT WAS OURS, AND THIS IS THE PART WORTH CARRYING FORWARD.** The exoneration P14 recorded —
*"the gcc reference … passes 5.1 clean"* — was run against **`reference-testfixture`, a LINUX ELF binary**, to
clear a divergence that exists **only on Windows**. A control built for a different platform than the leg under
test cannot discriminate, and it read as exculpatory. The leg's own same-platform oracle never existed:
`pe64-x86_64/reference-oracle.stderr` says *"the same-platform oracle for leg 'pe64-x86_64' did NOT build
(x86_64-w64-mingw32-gcc exited 1). The leg reports NO ORACLE"*. The harness said so **loudly**; two cycles read
past it. ★ **The rule that falls out: a reference control is not a control until you have checked WHICH TARGET
IT WAS BUILT FOR.**

**⚠ ALSO CORRECTED, so it is not re-quoted from the commits above.** The *"first scanstatus_v2 argument
statically receives a register last written by `mov $0x0,%r12d`"* reading is **FALSE for the shipped binary**.
Aligned (`.pdata`-bounded) disassembly shows arg1 is `mov 0x138(%rsp),%rcx` — a load from its home slot — at all
ten call sites; the store to that slot (`0x1404d0e2d`) **dominates** every one of them (CFG built from the dump,
reachability re-tested with the store's block removed); the `a||b` phi chain writes its carrying register on
every predecessor edge; arg5 correctly occupies the Win64 shadow slot at `[rsp+0x20]`. **That function's codegen
is correct.** The `r15`-as-sixth-argument reading is retracted for the second and last time.

---

## 0.000000000000000 ★★★ CYCLE P16 — THE VERDICT LINE NOW SAYS *WHICH* KIND OF "NO ORACLE" IT MEANS

Two rows closed, **net −1**, and the cheap half of the cycle is the lesson: `D-HARNESS-FAILING-REFERENCE-ORACLE-
COLLAPSES-TO-NO-ORACLE` had been **cited as a closing dependency since 2026-08-18 and never written**. A
`[[wikilink]]` to a non-existent anchor is invisible to every guard here — the anchor-balance instrument counts
ROWS, and a citation is not a row. Worth a sweep.

**THE DEFECT.** `oracle_class_for_leg` keyed the leg's oracle verdict on one fact — did a reference BINARY
survive — so three states with three different next actions printed as one sentence, *"NO ORACLE — no reference
binary was produced or preserved"*: the control **RAN AND FAILED** (its log is on disk, go read it), **no
compiler on this host targets this leg** (an environment limit), and **never attempted**.
⚠⚠ **The fact was never missing — the verdict line threw it away.** `--build-reference-oracle` has always
returned `built`/`build-failed`/`no-reference-compiler` and `attribute_build_failure` has always read it; only
the classifier ignored it. ✔That is the measured cost of a *reporting* defect: **P13 and P14 spent two full
cycles** hunting a DSS miscompile that was upstream, because pe64's verdict claimed it had no oracle when its
oracle had run, failed, and left `reference-oracle.log` on disk.

**THE FIX.** The status vocabulary gets ONE owner (`ORACLE_STATUSES` / `ORACLE_STATUSES_ATTEMPTED`, read by both
the classifier and `attribute_build_failure`, so a second literal list cannot drift); `oracle_class_for_leg`
takes the status and returns `build-failed` / `no-reference-compiler` as classes with their own prose; both
drivers pass `--oracle-status` to `--oracle-report`. ★ It was ALREADY passed to `--attribute-build` — which is
exactly why the bare option name was **not** a usable pin, and why the new assertions match the whole argument as
each driver spells it. ★★ Safety property pinned: **no status can conjure a control out of a missing binary** (a
`built` status with nothing on disk is still `absent`). An unrecognised status **RAISES** rather than degrading
to the pessimistic class.

**AND THE WINDIRENT ROW CLOSED WITH IT**, because its central claim is measured false: *"cannot be compiled by
ANY GNU-on-Windows compiler, DSS included"* — ✔DSS compiles that TU clean, and ✔`cl` compiles it rc=0. Two
references and DSS build it; only mingw-on-Windows does not, and upstream ships Windows via `Makefile.msc`. Under
bar §A.3b that is a NON-DSS CONFOUND, now rendered honestly as `build-failed` rather than "no control existed".

**GATE:** harness self-test **1962 → 1975, 0 failed**; `test-confound-scope.sh` **152/0**;
`test-confound-scope.ps1` **111/0** (both files' own assertion-accounting guards caught the additions — 150→152,
109→111, SKIP 48→49); **red-on-disable PROVEN fail-closed** (mutant differed by hash, lacked the witness, still
parsed, verified on disk before the reader ran, went RED, and the restore was verified byte-identical by sha256).

**NEXT — OPERATOR-SCHEDULED (2026-08-19), in this order:**
1. **The remaining residue above**: `D-HARNESS-FAILING-REFERENCE-ORACLE-COLLAPSES-TO-NO-ORACLE` — write the row
   (or retire the citation) and thread the oracle status into `oracle_class_for_leg` so *"the control ran and
   failed"* stops rendering as *"there is no control"*. Small, self-contained, and it is the last live piece of
   the pe64-attribution story.
2. **Operator conversation, then a cycle: grab the important anchors from the AP5/AP6 commit + the asm feature
   (this PR) pending deferrals.** Operator's stated ORDER: **pending feature completion → production errors →
   harness/test errors → others.**
3. **Operator decision still owed: report upstream to sqlite?** (a) the `%p` vs `format "ptr:0x%llx"` handle
   mismatch (`test/test1.c` + `test/scanstatus2.test`); (b) `fileio.c`/`windirent.h`'s `_MSC_VER` interlock.
   Both outward-facing, so neither was filed.
4. Then the standing loop: TLS-dylib corpus arm (P12 residual); asm remainder; C1 diagnostic coordinates
   (`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, HIGH).

⚠ **Housekeeping seen but NOT done this cycle** (no anchor — it is the already-open
`D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED`'s neighbourhood): ✔MEASURED, **five** stale lane worktrees
survive — `.claude/worktrees/{agent-a33bf76aa1baf2e04, agent-aa5ee5ab409b3c519, dss-no-repo-ancestry-cwd}` plus the
SIBLING-DIRECTORY ones `C:/Source/DailySoftware/{dss-lane-m, dss-wt-bitwise, dss-wt-movzw}` (detached HEADs).
⚠ `agent-aa5ee5ab409b3c519` is on disk with its own `build/` but is **absent from `git worktree list`** — an
orphaned copy, which is the worst of the set: it answers repo-wide greps and nothing tracks it. Both
`.claude/worktrees` copies were hit while searching this cycle.
---

## 0.00000000000 ★★★ READ THIS FIRST — CYCLE P13: THE CROSS-HOST MATRIX RAN — AND FOUND A MISCOMPILE

**The queue re-derivation paid for itself again**: the C7 "provider conversions" the handoff queued
were ALREADY DONE — the pinned-archive-everywhere conversion landed 2026-08-10 in `3e86a187`
(PR #48) and three rows sat stale-open for 9 days (`D-HARNESS-UBUNTU-PORTS-PROVIDER-NOT-
GENERALISED-TO-PINNED-ARCHIVE`, `D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-ONE-DRIVER` —
both closed this cycle; the "PS1 dispatch arm" sub-row never existed under the name I queued).
What remained was the ROW'S OWN STANDARD: *"the closing test is per-cell and by EXECUTION"*.

**The matrix, run by execution on four hosts** (sqlite harness, full driver per host):
- **Windows** × {elf64-arm64, elf64-x86_64}: built (after the discovery hazard below).
- **WSL** × {elf64-x86_64, pe64-x86_64, macho64-arm64, macho64-x86_64}: all built; elf64-x86_64 +
  pe64 corpus-VERIFIED (2 verified, 0 build-input-missing).
- **arm64 VPS** × elf64-arm64: built + corpus VERIFIED natively.
- **macOS** × {elf64-x86_64, pe64-x86_64}: built (runs structurally skipped by runOn, as designed).
⇒ **`D-HARNESS-CROSS-HOST-ANY-TARGET`'s BUILD half: every declared leg built on every host, by
execution.** (The row's close records the RUN matrix's documented restrictions.)

**⚠ THE DISCOVERY HAZARD, measured:** the first Windows attempt "failed" with pre-P10 loader
errors — the driver's compiler discovery (newest RELEASE tree, mtime-wins, build-type read from
each tree's CMakeCache) picked `build/rel`, a 2026-08-18 21:01 pre-grammar binary; the repo's
only current tree was Debug (excluded by design). **Not a harness defect** — the policy did
exactly what it documents; the fix was building a current `build/rel`. A stale Release tree that
SILENTLY answers discovery is now a standing trap: `cmake --build build/rel` after any
harness-relevant change, or clear the stale trees.

**★★★ THE FINDING — `D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2` (OPEN, HIGH, silent-miscompile class), operator-directed root cause, three facts pinned:**
1. **DETERMINISTIC**: standalone `scanstatus2.test` crashes at `scanstatus2-5.1`, same RIP, 30/30.
2. **WINE EXONERATED**: the same WSL-built testfixture.exe on REAL Windows exits 0xC0000005 at
   the same test — no wine in the path.
3. **THE INSTRUCTION**: the callee (RVA [0x5E0B0F,0x5E1952)) homes SIX arguments — rcx/rdx/r8/r9,
   stack arg5 — **and reads its SIXTH from `r15`, a callee-saved register no x86-64 ABI passes
   arguments in**, then derefs `[r15+0x88]`. The caller (the 5,534-byte Tcl command dispatcher
   [0x4D098E,0x4D1F2C)) passes five args and holds r15 as an int counter (=0). Shape: a
   caller/callee ARGUMENT-CONVENTION disagreement (K&R prototype-vs-definition arg-count mismatch
   is the classic source shape) or a >4-arg pe64 codegen bug.
⚠ The pe64 corpus has passed on WINDOWS-built fixtures historically — the reproducer must decide
whether the trigger keys on the WSL cross-build or a path the Windows build didn't take.

**NEXT — P14, nothing above it:** root-cause and FIX
`D-HARNESS-PE64-CORPUS-WINE-ABORT-SCANSTATUS2`. Start from the pinned RVAs: identify the TU
(transpile the staged `tclsqlite.c`-family TUs, match the caller's `mov %esi,%eax` + 5-arg call
shape), build the minimal reproducer (a 6-arg call through a mismatched prototype), fix in the
shared calling-convention/prologue code — this is a FIX, no gate applies — then rerun the pe64
corpus leg to green. Then the loop continues: the TLS-dylib corpus arm (P12's stated residual),
asm remainder, C1 coordinates.

---

## 0.0000000000 ★★★ READ THIS FIRST — CYCLE P12 (COMPLETE): THE MAC-AWAKE WINDOW — FOUR ROWS CLOSED ON REAL HARDWARE

The operator turned the Mac on (*"mac is on. you can try it"*), which made the availability-gated
window the pick. **Every closure below was MEASURED on the Mac (arm64, macOS 26.5.2) before
anything was declared**, and the window's queue was RE-DERIVED at pick time — half of it had
CLOSED UNDERNEATH the list (C6 done TF-C109; C7's named blockers done TF-C117/C121/C124):

1. **`D-LK-MACHO-X8664-DYLIB-RUNTIME` ✅** — the Rosetta run leg: DSS-built x86_64 `.dylib` +
   client, `arch -x86_64` → **exit 42**. Detail recorded in the row: macOS strips `DYLD_*` env
   for these processes (measured), so the client links with the carriage's documented import-name
   override — the clang equivalent of linking by path.
2. **`D-CODEGEN-MACHO-ARM64-X29-ALLOCATED-WITH-NO-FRAME-RECORD` ✅ DISCHARGED** — the crash
   reporter walks a DSS arm64 image's mid-function fault **fully symbolicated**
   (`leaf←middle←outer←main`), identical to the clang control. The ABI inference was refuted on
   the exact surface the row named; x29 stays allocatable, no config change (a register saved
   from a non-defect).
3. **`D-LK1-MACHO-X8664-DARWIN-DATA-SECTION` ✅** — its config half shipped 2026-08-05 (the row
   was STALE — the exact class `D-FORMAT-MACHO-X86_64-EXEC-DECLARES-NO-DATA-SECTIONS` warned
   about); the Rosetta run leg landed now: rodata/data/bss TU → **exit 42**.
4. **`D-LK3-DYLIB-TLS-MODEL` ✅** — witnessed → declared → run end-to-end: (a) the clang probe
   measured the row's exact unknown (a thread alive BEFORE a dlopen sees a FRESH, INITIALIZED
   TLV instance); (b) the arm64 dylib format opts in (three `__thread_*` rows from the exec
   sibling + `tlsAccess` + `supportedDataSections`; x86_64 sibling gets its absent-rationale);
   (c) the walker belt removed — its own trigger ("until a Mac witness exists") fired; (d) the
   rejection pin replaced by `ThreadLocalDylibLinksAndCarriesTLV` (strict: 48 B of descriptors,
   `__tlv_bootstrap` in the bind stream); (e) a DSS `_Thread_local` dylib + client, run NATIVELY
   → **exit 42**.

Gates: Windows 898/898 · Mac full ctest green (both on the final tree) · WSL + arm64 VPS legs
(this commit). Anchor delta: **closed 4, opened 0, net −4**. Independent code audit: CLEAN on
correctness; its MEDIUM finding (no in-suite corpus arm builds a `_Thread_local` dylib — the
two-file client shape is needed) is recorded as the STATED RESIDUAL in the D-LK3 row and queued
below; its LOW stale-citation finding folded same-cycle; MH_HAS_TLV_DESCRIPTORS covered
structurally at the shared flag site (both polarities pinned exec-side).

⚠ **The Mac window is STILL OPEN and the queue for it is NOT empty** — remaining Mac-gated work,
next window: the C7 provider conversions (`D-HARNESS-LIBRARY-ACQUISITION-BUILT-FOR-ONE-LEG-IN-
ONE-DRIVER` PARTIALLY CLOSED: generalize `ubuntu-ports` onto pinned-archive + declare routes for
`elf64-x86_64`/`pe64-x86_64` + the `.ps1` dispatch arm for elf64-arm64 — the four red BUILD-matrix
cells) — then validate the macOS×{elf64-x86_64, pe64-x86_64} cells ON the Mac. `D-LK3-DYLIB-WEAK-
EXPORT`'s validation also wants the Mac (implementation is format-side).

**NEXT — the loop continues:** (1) the C7 provider conversions above while the Mac is up;
(1b) the TLS-dylib CORPUS ARM (audit residual): a two-file `_Thread_local` dylib + client
example/integrated arm so a `lib`-profile regression reds in-tree (`test_ffi_resolve_library`'s
two-TU pattern);
(2) if the window closes: asm remainder (`D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` label
half — the asm-goto BLOCK binding design; `D-ASM-TEMPLATE-IS-LEXED-TWICE` LOW;
`D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` — re-measure, it predates the CFI producer landing);
(3) C1 diagnostic coordinates (`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, HIGH);
(4) §B pairs stay decisions (`D-ASM-RIP-RELATIVE…` + `D-ASM-ADDRESS-OPERAND…`;
`D-MIR-SEH-FILTER-CLONER` SUSPECT — verify or discharge). sqlite matrix re-run still queued for
the next codegen-changing cycle (P12 changes the DYLIB code path — a `lib`-profile corpus leg
would re-run it; the CLI/units corpus is unaffected).

---

## 0.000000000 ★★★ READ THIS FIRST — CYCLE P11 (COMPLETE) + THE STANDING BACKLOG LOOP

**The loop mandate (operator, 2026-08-19, verbatim):** *"/loop the /dss-cycle until all opened are
fixed. all closed by the best long term implementation with no workarounds."* The queue is the
operator's prioritized backlog (asm+miscompiles+errors → ap5/ap6 → tools → the rest), **verified
against the registry before P11 picked** — and that verification changed the top of the queue:

- **The operator's A1 and B1 cycles were already GONE**: `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`
  ✅ 2026-08-17, `D-DEPS-DEPENDENCY-CANNOT-DECLINE-A-TARGET` ✅ 2026-08-15 (operator decision),
  plus 4 more closed rows named in the backlog (P0016-detail, CRLF-divergence, PKG-config-tree,
  BUILD-LAYOUT) — all confirmed by measurement, not recalled.
- ⚠ **One correction to the answer I gave the operator:** I recommended
  `D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD` as the new top priority reading it OPEN HIGH — it has been
  **✅ CLOSED since 2026-06-03**; its closure record sat in the WRONG TABLE CELL (see below), which
  is why the balance gate and every list derived from it counted it open. Measured before P11
  picked; the recommendation is withdrawn.

**P11's deliverable — `D-ASM-TEMPLATE-DIAGNOSTIC-DOES-NOT-NAME-THE-C-STATEMENT` ✅ CLOSED:**
an inline-asm template diagnostic now names the **C statement as its PRIMARY locus** with the
template locus demoted to a related note. ✔Both references measured first (the row demanded it):
clang 18 (`gt.c:3:13: error: …` + `<inline asm>:1:2: note: instantiated into assembly here`) and
gcc 13.3 (`gt.c:3: Error: …`) **both lead with the C statement, neither has template-primary** —
so DSS's old template-primary shape was itself the divergence. Mechanism: `MirAsmDescriptor`
carries the statement locus (populated at HIR→MIR from the optional source map; sourceMap-less
callers degrade to the old render, nothing fabricated); `DiagnosticReporter::reanchorFrom`
(the `remapBuffers` post-admission precedent; demotes iff `buffer.valid()`) applied by an RAII
guard over the whole `expandInlineAsm` body and around `lowerInlineAsmGoto`'s refusal, so no
refusal site (or future one) can miss it; the text-only wrapper refusals gain the C primary
(they had no locus at all). Witness: `AsmTemplateDiagnosticNamesTheEmbeddingCStatement` — both
arms, exact rendered strings, red-on-disable demonstrated (guard deleted → new test RED, parent
test GREEN). Design audit (independent): 3 CONCUR + 1 objection adopted (demote discriminator =
buffer validity, not span emptiness — a zero-width span at a real buffer is a real locus).

**The registry sweep that fell out — `D-PLANS-REGISTRY-CLOSURE-MARK-IN-WRONG-CELL` ✅ born-closed:**
a closure record can sit in the Closing-work cell while the status cell (the one the balance gate
reads) carries only the description — the row then counts OPEN forever. Full-registry pass found
**8 rows with a cell-3 ✅ lead: 2 genuinely closed-but-miscounted, both repaired**
(`D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD` 2026-06-03, `D-LK10-ENTRY-RESOLVE-ENTRY-FN-IDX` 2026-06-02);
2 genuinely OPEN rows whose cell-3 ✅ is a stale over-claimed closure (left as live work, cell 2
authoritative); 4 🟢 design-record rows that stay counted OPEN by the gate's only-✅-closes
contract (reclassifying them is an operator semantics decision — **flagged in the P11 report**).

**Anchor delta: closed 3, opened 0, net −3** (balance gate OK; 707 → 704 registry-side).
Gates: Win 898/898 baseline + full battery, WSL + arm64 VPS legs (this commit).

**NEXT — the loop continues, this order (operator bands):**
1. **asm remainder**: `D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` (narrowed to the `%l` label
   half — real design work: a binding that names a BLOCK, the asm-goto refusal names the blocker)
   · `D-ASM-TEMPLATE-IS-LEXED-TWICE` (LOW; ownership precondition ✅ landed; its consumer needs
   designing) · `D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` (vocabulary rows; check trigger) ·
   `D-CSUBSET-ASM-LABEL-ON-SYNTHESIZED-SHIM-SYMBOL` (LOW).
2. **the Mac-awake window, C5+C6+C7 TOGETHER** (all gated on Mac availability):
   `D-LK-MACHO-X8664-DYLIB-RUNTIME` · `D-LK3-DYLIB-TLS-MODEL` · `D-LK3-DYLIB-WEAK-EXPORT` ·
   `D-HARNESS-CROSS-HOST-ANY-TARGET` (macho leg inputs) · `D-TEST-MACOS-HOST-SPAWNS-FOREIGN-BINARY`
   (over-claimed close, genuinely open) · `D-CODEGEN-MACHO-ARM64-X29` (SUSPECT — verify or
   discharge). The Mac is usually OFF — never wake it; run the window when the operator has it on.
3. **production errors**: C1 diagnostic coordinates
   (`D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, HIGH, unconditional) → symbol tables ×5 →
   dynamic linking ×2 → arch identity ×2 → diagnostic quality ×2 → `D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD`.
4. **§B-gated, bring as decisions, do not build**: `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` +
   `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` (the §5.4 operator-decision pair) ·
   `D-MIR-SEH-FILTER-CLONER` (SUSPECT — verify or discharge, don't patch on suspicion).
5. **Still queued behind the loop**: the full sqlite matrix re-run (operator: *"we'll re run
   everything once our compile is fast enough"* — it is; P11 changes diagnostics only, not
   codegen), then FC18.

---

## 0.00000000 ★★★ READ THIS FIRST — CYCLE P10 (COMPLETE): FIRST-CLASS CONFIG-DRIVEN LTO

Operator mandate, verbatim: *"address it properly, long term solution, no workarounds, 100% config
driven. first class LTO implementation"* (`D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE`, ✅ CLOSED — the row
carries the full measurement table and the two §B rulings).

**Landed in two operator-steered steps, one behavior change each:**
1. **`2557a717` pre-step** — the P9 self-audit's hardening pins (production candidate-builder value
   pin with a mutant-proven red arm; front-half concurrency witness; `.sh`/`.ps1` case parity).
2. **`a0370fde` step 1 — the schedule GRAMMAR**: `passes` became a closed tree (leaf / `repeat` /
   `fixpoint`), the engine its interpreter, flat docs desugar at load; budgets fail loud at load.
   ✔Byte-identical to P9 (`1d117b1a…`), compile 41.7s.
3. **This commit, step 2 — the TOPOLOGY**: `PipelineStage` slots in the driver (Unit at
   `buildCuMir`; Program at every final-module site — merged, N==1, archive member), the document
   owns what runs (`unitPipeline` key; absent = both stages same doc, what `debug` ships).
   `resolveCompileConfigFromPipelineName` + the examples runner's real-channel switch.

**The shipped split (B′, decided by the operator-amended rule — direct measures, no proxies):**
`release-unit` = `[ConstFold, Mem2Reg, SimplifyCfg, Dce, Inlining]×2` per CU, full release at the
program stage. Against the incumbent: **exe −5.6%** (6,904,848 vs 7,314,848), **compile time
neutral** (38.4s median vs 38.7s), **runtime FASTER** (0.453s vs 0.470s median, 7 repeats —
`scripts/sqlite-runtime-bench/sqlite-runtime-bench.py`, the standing runtime-differential instrument). The control arm
(unit key removed) measured **byte-identical to pre-P10**, proving the restructure behavior-neutral.
⚠ The FIRST rule (inline counts + exe-vs-A′) picked A′ and its cheapness premise was refuted
(+60% compile); the operator amended the rule toward direct measures BEFORE the runtime column was
measured — recorded in the row as the pre-registration discipline working.

**§B rulings this cycle:** (a) strip-order — unit stage CONTAINS `Inlining` (operator; per-TU -O2
shape); (b) the A/B rule amendment itself. Both recorded in the D-OPT7 row.

**⚠ OWED NEXT CYCLE (nothing blocks, all small):** the stage trace line (`opt: stage=…`) is
parsed by agg-trace (verified: 103 unit / 1 program over the B′ log) but no TEST pins its format;
the audit's nodePath label fix landed same-cycle. Debug-config column measured and clean: the kit
compiles in **24.8s** at `--config=debug` (the new N==1/archive program-stage calls run
Identity+prune+strip — no give-back).


---

## 0.0000000 ★★★ READ THIS FIRST — CYCLE P9 (COMPLETE): COMPILE TIME — PROFILED ON FOUR LEGS, FOUR MEASURED CAUSES FIXED, OUTPUT BYTE-IDENTICAL

Operator instruction that opened the cycle, verbatim: *"now we need a comprehensive profiling
across the WHOLE pipeline compiler and with ALL available legs (windows, linux arm and x86, macos).
The compiled object will be the sqlite cli. We'll use D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX
anchor but the goal here is to make a first class compiling time (and, of course, the code must be
correct!)"*. Later, verbatim: *"even 2 minutes is bad... gcc and msvc timing on sqlite cli would be
less then a minute... and if rederiveStructCfMarkers is slow, lets fix it"*.

### ✅ WHAT LANDED — every cause below was measured first, then fixed, then pinned

1. **`rederiveStructCfMarkers` was O(functions × module blocks).** Fixed three ways, one per row:
   the module self-loop index is computed ONCE per module; the back-edge sweep is scoped to the
   function's own candidate range (`mirNaturalLoops(…, candidateSources)`, fail-loud ascending
   contract, completeness = order ∪ {module self-loops}); and the post-dominator scratch
   (`MirPostDomScratch`) reuses the eight whole-module buffers via two self-recorded touched lists,
   one `computePostDomInto` core keeping fresh and scratch byte-identical BY CONSTRUCTION.
   ✔29.0 s → **1.2 s** across the 320 whole-module calls of one sqlite release compile
   (5.3–6.6 s → 0.19–0.22 s per call). Rows: [[D-OPT-POSTDOM-SCRATCH-REUSE]] ✅ (the gated trigger
   FIRED at 28.3 s vs its ~10 s bar), [[D-OPT-NATURAL-LOOPS-MODULE-WIDE-SCAN]] ✅ born-closed,
   [[D-MIR-STRUCTCF-UNREACHABLE-BLOCK-CLAIMED]] ✅ born-closed (spec text reconciled to pinned
   behaviour), and [[D-MIR-STRUCTCF-DERIVATION-REACHES-PAST-THE-FUNCTION]] 🟠 OPEN — the derivation's
   cross-function reach is CANON, pinned, and narrowing it is a deliberate future decision, not a
   tidy-up.
2. **Every pass reserved a module-sized rewrite map per function.** Per-function `reserveHint`;
   the natural experiment that sized it: ConstFold ~500 ms vs 3,100–3,900 ms for the
   per-function-rebuilding passes. [[D-OPT-REBUILD-REWRITE-MAP-RESERVES-THE-WHOLE-MODULE]] ✅.
3. **The front half was strictly serial and the `--time` report lied about it.** `buildCus` now
   runs the pool with index-order slot collection (determinism from CU index, never completion
   order), a `std::latch` join, fail-loud on a disengaged slot, n≤1 inline; `kMaxAutoWorkers`
   16→32 with RSS measured at both (5.0 GB @16, 8.6 GB @32 — the raise is a memory decision, on
   the record). Phase timing is TWO-CLOCK (`cpuNanoseconds` Σ-thread vs `wallNanoseconds` interval
   union, `peakConcurrency`, `liveScopeCount`) and an invariant violation FAILS the run — the old
   `[other] 0ms` row was a negative remainder clamped to zero. `std::localtime` →
   `localtime_s`/`localtime_r` (thread safety under the now-parallel front half).
   [[D-PERF-4-CU-PARALLELISM]] carries the addendum.
4. **The harness timed a Debug compiler against Release twins.** `build-and-test.ps1` now READS
   the compiler's build type from its tree's `CMakeCache.txt`, refuses non-Release under
   `SKIP_DSS_BUILD=1`, offers a three-state `DSS_ALLOW_NONRELEASE_COMPILER` (typos refused) that
   opts out of the refusal, never the statement. [[D-HARNESS-PS1-TIMES-A-DEBUG-COMPILER-WHILE-THE-SH-TWIN-TIMES-A-RELEASE-ONE]]
   ✅ born-closed, its selector probed by AST-extracting the real function text.
   `scripts/profile-compile/profile-compile*.sh` + `-support.py` ship as the cross-leg profiling kit — ONE script,
   no `.ps1` twin, BY DECISION (a profiler whose value is "the host is the only variable" must
   not have a per-host implementation). ⚠ Two Windows-leg defects in the kit were found and fixed
   by RUNNING it: `os.path.join` backslashes eaten by bash, and bare-name `bash` resolved by
   CreateProcess to an extensionless PATH shadow that `shutil.which` never saw — the gate now
   passes the absolute shell and forward-slash paths.

### ✅ ACCEPTANCE — "the code must be correct!" ✔ALL GREEN, witness on disk

- **Byte-identity vs the PRE-P9 compiler**: pristine `d62405ba` worktree, Release, built this cycle
  (`C:\Source\DailySoftware\dss-p9-baseline`, deleted after) vs the P9 merged tree — same kit, same
  target, **image trees both sha256 `1d117b1aafce2e2c1f52b789fdf49f71bb3fe97cab3f4a39a1d2f46bb4f3a813`**
  (path-keyed hash over every emitted file; the sqlite3 binary itself `932cbdaf…` on both).
- **≥20× repeat determinism**: **20/20** parallel repeats of the P9 compiler, every image tree
  byte-identical (`ACCEPT-OK … repeats=20`, captured at `build/perf/accept-p9-result.log`).
- **Cross-host byte-identity**: the WSL-built P9 compiler emits the SAME `932cbdaf…` sqlite3 binary
  for the same kit+target — the determinism property holds across hosts, not just runs.
- **Post-fix timing (the point of the cycle)**, 103-TU sqlite CLI kit, `--config=release`:
  **Windows median 40.7 s** (20 repeats, 38.6–45.4 s) vs pre-fix 3m32.1 s; **WSL 33.6 s** vs
  pre-fix 1m40.8 s. The Windows-vs-WSL residual collapsed **2.1× → 1.2×** — the quadratic and
  allocation-churn work was exactly what Windows' allocator amplified. gcc yardstick stays
  21.5 s `-j1` / 4.8 s `-j32` on WSL.
- **sqlite re-probe**: the emitted CLI, FILE db, release config — CRUD `sum(x)=42` ✔.
- **3-leg gate**: Windows **898/898** · WSL **898/898** · arm64 VPS **898/898** — all three
  through the run-gate witness (`'100% tests passed'`).

### ✅ THE PRE-FIX PROFILE — settled measurements, re-quotable as the BEFORE record

**★★★ THE ANCHOR'S HEADLINE FIGURE WAS AN ARTEFACT OF THE HARNESS, NOT A PROPERTY OF WINDOWS.**
`D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX` compared a **Debug** compiler against a
**Release** one. ✔MEASURED: `build/real-examples/win-probe.log:64` shows the Windows run used
`build\dbg\bin\dss\dss-code-prime.exe`; that tree is `CMAKE_BUILD_TYPE=Debug` with `-g`, and its
`build.ninja` contains **zero** `-O` flags and **zero** `NDEBUG`. Meanwhile `build-and-test.sh:2767`
*unconditionally* builds `build/rel` with `-DCMAKE_BUILD_TYPE=Release`, while `build-and-test.ps1:2721`
takes the **newest existing** binary from five roots regardless of build type — and on a developer box
`build/dbg` is always newest. ⇒ anchored + fixed as
[[D-HARNESS-PS1-TIMES-A-DEBUG-COMPILER-WHILE-THE-SH-TWIN-TIMES-A-RELEASE-ONE]].
⚠ The trap that hid it: the harness's `Config` parameter says `release` and means **the artifact's
optimizer pipeline**; the **compiler binary's own build type** was never stated anywhere. Both read as
"release" in the log and only one was ever controlled.

**✅ THE FOUR-LEG PROFILE.** Identical sources (SQLite CLI, 103 TUs, copied — NOT re-staged, because
the harness pulls upstream every run and a re-staged subject is a different subject), identical target
`x86_64:elf64-x86_64-linux-exec`, Release compiler built on each host, each host reading **its own**
config tree:

| host | cores | wall | optimize | front half | semantic | everything else |
|---|---|---|---|---|---|---|
| WSL x86_64 | 32 | **1m40.8s** | 1m03.6s | 29.6s | 16.7s | 37s |
| arm64 VPS | 4 | **3m18.1s** | 1m50.7s | 66.8s | 12.2s | 87s |
| Windows x86_64 | 32 | **3m32.1s** | 2m24.2s | 41.3s | 21.6s | 68s |
| macOS arm64 | 10 | **5m10.0s** | 4m42.9s | 27.4s | 5.2s | **27s** |

★★ **READ THE macOS ROW BEFORE CONCLUDING ANYTHING ABOUT HOSTS.** It is the SLOWEST overall and has
the FASTEST front end of the four — its semantic phase is **5.2 s against Windows' 21.6 s**. The whole
of its deficit is `optimize`. Across the four hosts `optimize` varies **4.4×** while every other phase
varies under 3×. That is the signature of allocator behaviour under high churn, not of computation —
and the churn has a named source (below).
⚠ Windows-vs-WSL on the SAME hardware is **2.1×**, real but a fraction of the anchor's 8×.

**✅ THE YARDSTICK.** ✔MEASURED on WSL, the SAME 103 TUs, gcc 13 `-O2`, on a **monotonic** clock:
**`-j1` 21.5 s · `-j32` 4.8 s** — against DSS's 1m40.8 s on that host. So DSS is **4.7× serial gcc**
and **21× parallel gcc**. ⚠ The first attempt at this number used `date +%s%N` (CLOCK_REALTIME) and
reported a compile that took **minus 11.7 seconds** — see [[project_wsl2_clock_realtime_broken_2026_08_01]].
The compiler's own `--time` uses `steady_clock` and was never affected.

**✅ WHERE THE TIME GOES — four independent causes, each measured, none of them host-specific:**
1. **The merged whole-program optimize is 107.7 s of passes + 6.3 s of marker rederivation, on ONE
   thread, while 31 cores idle.** ✔Process sampling showed a ~2-minute stretch at exactly 1.0
   CPU-second per wall-second with `threads=1`. Four iterations, **never converged**, and the cost per
   iteration is FLAT — 25.6 / 26.3 / 27.6 / 28.2 s. It gets *slower*, not cheaper.
2. **Every pass clones the whole module whether or not it changes anything.** Every one of the nine
   passes is `for (i<nf) { policy.analyze(f); MirFunctionRebuilder rb{mir,builder,policy};
   rb.rebuildFunction(f); }` — ~3,900 functions rebuilt per pass per iteration. `Cse rebuild=9.94s vs
   analyze=0.73s`; `Licm rebuild=9.68s`. ★ Trace lines include `pass=Mem2Reg done 3454ms mutated=0` and
   `pass=Licm done 6129ms mutated=0` — **19.0 s of the 107.7 s is spent by passes that provably changed
   nothing.** This is the allocation churn that explains the 4.4× host spread.
3. **`rederiveStructCfMarkers` cost 28.3 s over 319 calls** — more than every per-CU pass combined.
   ✔MECHANISM FOUND BY READING: `deriveStructCfMarkers` (`mir_struct_markers.cpp:44`) opens with
   `std::vector<StructCfMarker> out(mir.blockCount(), ...)` — `Mir::blockCount()` is the **WHOLE
   MODULE's** block count — and the whole-module driver calls it **once per function**, while
   `applyDerived` reads back only that function's slots. O(functions × module blocks): quadratic.
4. **The front half is strictly serial.** preprocess → splice → tokenize → expand → parse →
   resolve-imports runs one TU at a time at `program.cpp:3206`; sampling showed 1.0× throughout. And
   `resolveCuPoolWidth` (`program.cpp:358-371`) caps the back half at `kMaxAutoWorkers = 16`, so a
   32-core host uses half the machine — though observed back-half concurrency was only ~3.7×, well
   under even that cap, so worker count is **not** the whole story there and the bump must not be
   claimed as its fix.

**✅ THE `--time` REPORT WAS LYING, AND IT HID EXACTLY THIS WORK.** Its `[other]` row printed **`0ms`
on all four legs** — not "nothing unaccounted" but `program.cpp:2374-2376` **clamping a negative
remainder to zero**. `PhaseTimers` sums self-time across every thread with relaxed atomics while the
back half runs 16 workers, so attributed time routinely exceeds process wall time. The header still
asserted the opposite (`phase_timers.hpp:29-31`: *"The pipeline itself is synchronous"*), stale since
`D-PERF-4`. ★ The per-pass numbers above had to be reconstructed from an env-gated `fprintf` that
prints and discards — duplicated in **five** files.

### ⚙️ THE LANES' FOLD — where the four lanes' work lives now

All four lanes merged into the working tree and were reviewed; their build trees
(`build/lane-{markers,opt,driver,harness}{,-rel}`) are CLEARED once the gate is green. The
merged tree was accepted as ONE subject: the byte-identity + repeat acceptance below was run
against the merged compiler, not per-lane binaries. ⚠ Two unattributable byte-identical sqlite
binaries found in `build/perf/{lane-before,scratch}` during fold review were NOT used as
acceptance evidence — the baseline below is a pristine `d62405ba` worktree built this cycle.

### ⚖️ NEXT CYCLE'S MANDATE — `D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE` (operator-promoted)

I recommended leaving it (deleting the redundant per-CU optimize buys ~5-10 s of a 213 s build and can
move codegen). **The operator overrode that**, verbatim: *"address it properly, long term solution, no
workarounds, 100% config driven. first class LTO implementation"*.
★★★ **MY SIZING MEASURED THE WRONG THING** — it priced *deleting the second call*, when the
deliverable is *the topology*. Today both call sites resolve **the same pipeline document**, so
`release.pipeline.json`'s 9 passes × 4 iterations run over every CU and then again over the merged
program; real toolchains run a per-TU pipeline and a link-time pipeline that are DIFFERENT. The
topology is hardcoded in the driver, and that — not the duplicated work — is the defect.
⚠ **THAT ROW'S OWN PREMISE IS UNDER CHALLENGE**: it says the per-CU pass *"changes no output"*, but
`Inlining`'s legality gate is a hard size cutoff (`inlineThreshold`, 50 MIR instructions), so per-CU
`ConstFold`/`Dce`/`Mem2Reg` can shrink a callee **under** that threshold before the merged pass sees
it. Default stage contents are therefore a MEASUREMENT, not an analogy. Full detail is in the row.

---

## 0.000000 ★★★ READ THIS FIRST — CYCLE P8: PATH IDENTITY IS NOW A TYPE, AND THE REBASE IS DONE

**✅ THE REBASE IS COMPLETE.** ✔MEASURED: HEAD `41a320ad` (Cycle P7) sits on `origin/main`'s
`fe031376` (AP6 #53); `backup..HEAD` = **555 files**, exactly AP6's own count, and the ONLY file
changed that AP6 never touched is `src/asm/asm_template_to_lir.cpp` (a deliberate port of AP6's
`CfClass::Switch` refusal into our relocated dispatch). Backups: `backup/pre-rebase-p7-8ecb8e8d`,
`backup/pre-rebase-main-0f47896f`, `backup/c23-burndown-3-pre-rebase-2026-08-17`.
⛔ **NOT PUSHED — operator instruction, verbatim: *"when it's time to commit, do it, but don't
push."*** That instruction is still live.

### ⚖️ THE sqlite MATRIX — CANCELLED MID-FLIGHT BY OPERATOR INSTRUCTION. WHAT IS AND IS NOT MEASURED

Operator, 2026-08-18: *"ok. let's cancel this, commit + push, we'll re run everything
once our compile is fast enough"*. ⚠ **The BUILD half is a real result and the RUN
half is mostly UNMEASURED. Do not read the build numbers as a matrix verdict.**

✅ **BUILD — measured, and it is the strongest statement this run supports:**
**three hosts each built ALL FIVE target legs** (`elf64-x86_64`, `elf64-arm64`,
`pe64-x86_64`, `macho64-arm64`, `macho64-x86_64`) from the full upstream sqlite
source set. Windows 5/5 · WSL 5/5 · arm64 VPS 5/5. ★ That is the
build-ANY-target-on-ANY-host requirement holding on three hosts at once.
⚠ **macOS never reached step 7** — it was at 6/9 (per-leg tcl/zlib resolution) when
cancelled, so it built NOTHING this run and its 5/5 is **not** claimed.

✅ **RUN — what actually executed, all of it from the WSL host:**
- `elf64-x86_64` corpus **GREEN** — 4 errors / **394,693** tests, all 4 known
  non-DSS confounds (`sessionnoact-4.3`, `walsetlk-2.1.6`, `walsetlk-2.2.14`,
  `zipfile-25.0`).
- `elf64-arm64` corpus **GREEN** — 7 errors / **394,697** tests, all known
  confounds.
- `pe64-x86_64` CLI smoke **14/14**; its corpus ran under Wine with 4 aborts and
  4 resumes, and 1 failure excused ONLY because the leg is emulated.
- arm64 VPS: `elf64-arm64` CLI smoke **14/14**; the other three legs' smokes were
  CLASSIFIED skips naming the host OS and the absent launcher, never silent.

⛔ **NOT MEASURED, and each is a hole, not a pass:**
- **No host produced a step-9 VERDICT BLOCK.** WSL's was destroyed by
  [[D-HARNESS-ABORT-SUMMARY-CRASHES-ON-THE-RUN-THAT-NEEDS-IT]]; the other three
  were cancelled before reaching it.
- **The Windows corpus never ran at all** — it was still in step 7b (per-leg CLI
  builds, ~11 min each) after ~2 hours, having spent ~89 min on the testfixtures.
- **The native arm64 corpus never ran.** ★★ THE REASON IS WORTH KEEPING: the VPS
  driver runs legs in catalogue order, so it spent **59+ minutes emulating x86_64
  under `qemu-x86_64`** — duplicating a leg WSL had already run NATIVELY green —
  while the one result only that machine can give sat queued behind it. ⚠ On a
  re-run, drive that host with `DSS_LEGS=elf64-arm64`.
- **macOS: no build, no run, no CLI smoke** this cycle. Its UNITS are green
  (898/898) and that is a separate, complete measurement.

### ⛔ TWO RE-RUNS ARE OWED AND ARE **DEFERRED BY OPERATOR INSTRUCTION** — DO NOT RUN THEM

Operator, 2026-08-18, verbatim: *"don't rerun qemu now please. we need to enhance
compile time before it, we'll do it later"*. ⚠ **This is a deliberate sequencing
decision, not an oversight, and a future reader must not discharge it as tidy-up.**

The two owed items, stated precisely so nobody re-derives them wrongly:
1. **arm64 VPS units re-run.** Its 897/898 was measured on a tree synced BEFORE the
   last three fixes landed; the sole failure was `anchor_registry_guard`, the
   registry cell-width violation now fixed. ⚠ On POSIX the only product-code
   delta since that sync (`make_preferred()`) is a NO-OP, so the compiler
   behaviour it measured is the shipped behaviour — but the trees are not
   byte-identical and the claim is stated at that strength, not stronger.
2. **WSL sqlite re-run.** Its run produced every corpus result and then died in
   step 9/9 (see `D-HARNESS-ABORT-SUMMARY-CRASHES-ON-THE-RUN-THAT-NEEDS-IT`), so
   there is **no verdict block** for it. The per-leg numbers below were read from
   the lines that printed before the crash and are sound; the aggregate verdict
   was never computed.

★ THE BLOCKER TO CLEAR FIRST IS [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]]
— the reason the operator wants compile time addressed before paying for another
full matrix is in that row's numbers: ~18 min per leg on the Windows host against
~2m20s on WSL and ~5 min on a small ARM VPS.

### ★★★ THE FINDING: EVERY PATH-IDENTITY KEY IN THE COMPILER WAS 8.3-BLIND ON OUR OWN TOOLCHAIN

✔**MEASURED** with a standalone probe against `c++.exe (MinGW-W64 x86_64-ucrt-posix-seh) 13.2.0` —
the compiler `build/dbg` actually uses:

```
input            : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1
exists           : yes
weakly_canonical : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1   ec: <none>
canonical        : C:\Users\rafae\AppData\Local\Temp\DSS-SC~1   ec: <none>
```

libstdc++ resolves `.`/`..` and symlinks and has **no concept of an 8.3 alias**, so two spellings of
ONE directory survived as TWO keys. ★★ **The MSVC STL happens to normalize them, which is why the
2026-08-17 CI fix looked like it worked: the property was never HELD, only accidentally satisfied by
one toolchain.** The consequences were live — the preprocessor's `#pragma once` re-entry guard was a
`vector<fs::path>` searched with `std::find` (a LEXICAL compare), so a header reached through both
spellings would be preprocessed TWICE into one TU; and the source-dedup keys would admit one file
twice, producing a duplicate CU and a duplicate-symbol link error naming no manifest.

★★★ **OPERATOR RULING 2026-08-18 — option (A) WITH TWO UPGRADES.** The test that decided it:
*"the only one whose invariant a check script can hold."* Boundary normalization (option B) was
rejected on its own analysis — an unbounded ingress set where a miss is silent *"is not an invariant,
it is a habit"* — and §4 of the ruling **forbids doing both**: a second normalizer is a second owner
of one fact, and the one that runs first wins silently.

- **Upgrade 1 — PATH IDENTITY IS A TYPE, NOT A FUNCTION RESULT.** `core::PathIdentity`
  (`src/core/substrate/path_identity.{hpp,cpp}`) has exactly one constructor, `of()`; the 14
  containers are keyed on it, so a raw `fs::path` **does not compile** as a key. Converting OUT is
  free; construction IN is the one gate.
- **Upgrade 2 — THE LINT DEFINES A COMPLEMENT.** `scripts/check-path-identity/check-path-identity.py`: rule 1 refuses
  `#include <filesystem>` in `src/` outside a 50-file allowlist (so a NEW file doing path work is a
  decision on the record); rule 2 is the enumeration underneath. Comment/string-stripped so it fires
  on CALLS not mentions, with a 4-control `--selftest`.

★★ **THE UPGRADE PAID FOR ITSELF IMMEDIATELY, AND THIS IS THE DURABLE LESSON: the type found FOUR
sites the originating grep was structurally incapable of seeing, because none of them called
`weakly_canonical` at all** — `dependency_resolver.cpp`'s artifact `seen` (raw `generic_string()`),
`shipped_lib_descriptor.cpp`'s `claimedPaths` (a THIRD normalizer), and the `visited` closure sets in
`import_resolver.cpp` and `preprocessor.cpp` (×2). **13 sites became 14.**

⚠ **ONE SITE WAS DELIBERATELY NOT CONVERTED** — `core/types/config_path_walk.cpp` resolves the
running executable's symlink to derive a directory to WALK; its result is never compared, never a
key, never printed as an identity. It carries an explicit, reasoned exemption in the lint.

### ⚠⚠ TWO TRAPS INSIDE THE FIX — BOTH MEASURED, ONE BEFORE AND ONE AFTER

1. ✔**BEFORE the code was written:** `GetLongPathNameW` **fails wholesale when any component is
   absent** — `DSS-SC~1` expands, `DSS-SC~1\not\yet\built.o` comes back **UNCHANGED**. The obvious
   one-call composition therefore no-ops on exactly the paths that do not exist yet (every output
   artifact) while looking correct on every input path that happens to exist. The implementation
   expands the longest EXISTING prefix and re-attaches the remainder verbatim.
2. ✔**AFTER, caught by the suite:** unifying the identity key on `generic_string()` was right, but
   leaving the conversion OUT generic too handed callers a forward-slashed Windows path. Three
   native-probe tests died with `'""C:' is not recognized as an internal command` — the probe builds
   a `.bat` path from a canonicalized base plus a natively-joined suffix, and **`cmd.exe` cannot run
   a forward-slashed path while every Win32 API accepts one happily**. ⇒ `string()` is the identity
   and stays generic; `path()` returns PREFERRED separators.

### ★ THE TEST THAT WAS RIGHT WHEN I ASSUMED IT WAS MIS-AIMED

`MatchesTheProductsOwnCanonicalizer` scanned `dependency_resolver.cpp`'s body for `weakly_canonical`.
Routing the product through `PathIdentity` emptied that body, so the pin failed — saying exactly the
true thing. **Its subject moved; it was not wrong.** It is replaced by
`FixtureHasNotGrownItsOwnCanonicalizer` (the fixture now CALLS the product, so "the two agree" would
be x==x — a test that passes both ways). ⚠ Its positive control was mutating `weakly_canonical`, a
token the fixture no longer contains, so it would have silently asserted NOTHING; re-pointed to the
token the pin now searches for.

### ★★ TWO HARNESS DEFECTS FIXED IN PASSING, BOTH OF THE SAME SHAPE

- **`mustDifferFromBaseline` was read by ONE of the two example runners.** ✔MEASURED **455 of 609**
  manifests declare it across **548 arms**; `integrated_tests/runner.cpp` never learned the key, so
  every one of those assertions was invisible on the CLI-subprocess side. Implemented (parse + allow
  + **read**) — whitelisting alone would have re-created, inside the fix, the exact defect the closed
  key set exists to prevent.
- **The macOS leg could not be staged at all.** ✔MEASURED: `printf 'X' \| scripts/ssh-macos/ssh-macos.sh cat`
  returns **nothing** — the Mac's login profile CONSUMES STDIN, so tar-over-ssh arrives truncated.
  The VPS survived the identical path only because its profile is quiet. Both `.sh` carriages gained
  a `--rsync` transport that never touches stdin; ✔verified by execution, not by exit code.

⚠ **AND THE MISTAKE I KEPT REPEATING, RECORDED BECAUSE IT COST FOUR ROUND TRIPS:** a `\| tail` after
a build or a gate replaces the status that matters with `tail`'s. It produced a "green" Windows leg
over a red ctest, a "successful" build that had failed on a brace error, and two "completed" legs
that had died on a path. **Take rc DIRECTLY; use `scripts/run-gate/run-gate.sh`, which exists for this.**

## 0.000001 ★★★ CYCLE P7 — pe64 GETS A POSIX LAYER, AND AN IDENTITY CLAIM BECOMES CHECKABLE

**Every claim below is ✔MEASURED unless labelled otherwise.**

### ★★★ THE HEADLINE, AND IT INVERTS WHAT THE CYCLE OPENED WITH

The cycle opened believing it had caused a **105-diagnostic pe64 regression** by deleting `_MSC_VER`.
✔MEASURED against the leg's OWN same-platform reference compiler (`x86_64-w64-mingw32-gcc`, gcc
13.2.0, driven by the 46 `-D`/`-I` flags lifted verbatim from the harness's own
`out/pe64-x86_64/reference-oracle.log`):

| TU | DSS | mingw-w64 gcc, same flags | owner |
|---|---|---|---|
| `bld/shell.c` (CLI) | 24 | **0** | DSS |
| `src/test_fs.c` | 38 | **0** | DSS |
| `src/test_thread.c` | 1 | **0** | DSS |
| `ext/misc/fileio.c` | 66 | **31** | **UPSTREAM** |

★★ **`ext/misc/fileio.c` cannot be built for any Windows target by any GNU-on-Windows compiler.**
`fileio.c:86` gates `#if !defined(_WIN32) && !defined(WIN32)` with **no `__MINGW32__` escape**, so
every Windows target takes `#include "windirent.h"`; `windirent.h:22` opens
`#if defined(_WIN32) && defined(_MSC_VER)`, so under any non-MSVC toolchain the shim body is elided
and `<windows.h>` never arrives — while `fileio.c` uses the Win32 vocabulary unconditionally.
Upstream ships Windows via `Makefile.msc`, so the configuration is never exercised there.
⇒ [[D-UPSTREAM-SQLITE-FILEIO-WINDIRENT-IS-MSVC-ONLY]].
★ **The identity fix did not CREATE that; it UNMASKED it.** pe64 had been compiling the TU only by
claiming an MSVC identity DSS does not have. Agreeing with gcc is the conformance outcome.
⚠ **AND `fileio.c` IS STILL CHARGED TO DSS**, because the new attribution machinery found a real DSS
gap inside it on its first run — `_wchmod` — which is exactly the false-amnesty a coarse rule would
have granted. Fixed this cycle; the TU's residue is now empty of DSS-attributable names.

### ★★★ WHAT LANDED — pe64 NOW HAS A POSIX LAYER, MODELLED ON WHAT THE REFERENCE ACTUALLY IS

`<unistd.h>` on pe is a **RE-EXPORT**, not a symbol dump, because that is what mingw's own header is:
it `#include`s `<io.h>`, `<process.h>`, `<getopt.h>` and declares only eight names itself. So
`unistd.json` gained pe via **format-gated `includes` edges** — new vocabulary this cycle
([[D-FFI-DESCRIPTOR-INCLUDES-EDGE-GATE]]) — plus its own constants.
★ **`getpid` is deliberately NOT in `unistd.json`.** It arrives on pe only over the pe-gated edge to
`process.h`, which makes it the witness that the surface checker walks the ACTIVE CLOSURE rather than
the named descriptor. **Do not "simplify" it into a direct declaration — that silently deletes the
witness.**

**DSS now ships bodies, not just declarations** — `runtime/platform/src/unistd.c` (139 lines, 7
realized bodies), the second witness for [[D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF]] after
`dirent.c`. Each one exists because a `linkName` would have been WRONG, not merely absent:
* `sleep`/`usleep` — ucrtbase exports `_sleep`, which takes **milliseconds** where POSIX takes
  **seconds**. A `linkName` onto it is a silent 1000× error. Red-on-disable proves the body: the
  mutant that treats the argument as ms reddens with a **289 ms** wall against the good run's 1289 ms.
* `ftruncate`/`ftruncate64`/`truncate`/`truncate64` — mingw realizes `ftruncate` as a header INLINE
  over `_chsize`, so no export of that name exists to bind to.
* `swab` — `_swab` is a real export, but DSS declared `swab` on **no** format, so it was closed on
  elf and macho too.

Also closed, each against the oracle: `dup`/`dup2`; `_fileno`/`_pclose` moved to `stdio.json` where
the reference has them; `open`/`close`/`read`/`fstat` converted from **macros to `linkName` symbol
rows** (the witness takes `&open`, impossible if they were macros); the `F_OK` family on `<io.h>`;
`S_ISBLK`/`S_ISSOCK` per-format (`S_IFBLK` was not merely absent on pe — it shipped **24576** flat
where mingw declares **12288**, a value NEITHER reference declares); `_wchmod` via an EDGE, because
mingw's `<sys/stat.h>` line 14 *is* `#include <io.h>`.

★★ **AND A LATENT SILENT MISCOMPILE, FOUND ONLY BECAUSE THE GAP HID THE GAP.** `clock` was declared
flat `fn() -> i64`, but `clock_t` on pe is `long` = **4 bytes**: reading a 64-bit return takes the
**undefined upper half of RAX**. It had never fired because `clock_t` had no pe arm at all, so
`clock()` died at its declaration. Fixing the declaration would have exposed the miscompile; fixing
both is the cycle's second `_sleep`-shaped defect — links clean, loads clean, wrong answer. The
width is now double-authored (`i64` elf/macho, `i32 "long"` pe) and pinned by a RUNTIME red-on-disable
(revert to flat `i64` ⇒ exit 2). `CLOCKS_PER_SEC`/`CLK_TCK` were declared on **no** format — which is
what made the facility unusable rather than merely awkward — and six pe `<time.h>` macros became
symbol rows.

### ★★★ `impliedSurface` — AN IDENTITY MACRO'S PRESENCE IS NOW A CHECKED CONSEQUENCE

[[D-LANG-PREDEFINED-MACRO-REQUIRES-REALIZED-SURFACE]]. Every `predefinedMacros` row in every document
declares one of three states; there is no bare `null` and no defaulted absence. The claim names
**SYMBOLS**, never just a header — header granularity is nearly vacuous once re-export exists, since
`<unistd.h>` resolves on pe the moment the `io.h` edge fires even if the only name that arrives is
`getpid`.

★★★ **THE REFUSAL HAS BEEN OBSERVED FIRING TWICE, AND ONE WAS AN ACCIDENT.**
1. ✔THROUGH `ctest` — the only channel that proves the mutant was READ, because `findShippedConfig`
   walks the CWD unless `DSS_CONFIG_ROOT` is set and only `dss_add_test` sets it. Control-green →
   mutant-red → restore-green, mutant sha-different:
   `error[C_UnbackedPredefinedMacro] predefined macro '__MINGW32__' (<lang>.lang.json
   /preprocess/predefinedMacros[21]) requires 'unistd.h' to declare 'sysconf' on object format 'pe',
   and the shipped surface reachable from that header does not.` — macro, CONFIG ROW, header, missing
   SYMBOL and format all render.
2. ★★ **UNPROMPTED, IN THE LIVE TREE.** A lane moved `_fileno` from `io.json` to `stdio.json`; a
   claim two files away went stale the same instant and the build refused. Nobody was testing for it.
   That is the mechanism's entire purpose, arrived at by accident, which is the strongest evidence
   available that it is not a formality.

⚠ **THE FIVE NULLS THAT PROVE WHY THE SHAPE HAD TO BE TAGGED.** The first cut was `null`-or-claim.
✔MEASURED against the real corpus: 80 of 84 rows were `null` — and FIVE of those were
surface-implying platform identities written as `null` **while populating the mechanism built to
prevent exactly that** (the two OS-selection rows against a 97-symbol/64-typedef/89-constant/16-struct
surface, and the three platform-vendor rows against the Darwin cluster). Operator's design bar: *the
question is not whether those five were wrong, it is WHAT FINDS THE SIXTH.* The obvious rule — force a
claim only on FORMAT-GATED macros — ✔MEASURED to reach **19 of 84** and to EXEMPT the four
compiler-identity rows, which are ungated and are the subtlest class there is. Hence: no bare `null`
anywhere, a closed `reason` tag (`erases-to-nothing` / `arch-property` / `standard-defined`), and a
third state **`not-expressible`** for a macro that DOES imply a surface this predicate cannot state.
★ The closed tag is what makes copy-paste decay REVIEWABLE — an inherited `null` is invisible in a
diff; an inherited TAG that is wrong for the new macro is not.

⚠ **THE KEY IS `impliedSurface`, NOT `requires`, AND THE RENAME WAS AN OPERATOR REVERSAL OF THEIR OWN
NAME.** ✔MEASURED: `requires` already carried TWO unrelated meanings — the document-level grammar-HOLES
contract (`asm.lang.json`) and the sqlite harness's confound environment-probe list (`legs.json`,
pinned at `tests/harness/test_sqlite_harness_legs.cpp:1584`). *"Different scopes, no parse ambiguity"
is a correct statement about the PARSER and the wrong test for a config key."* Renamed while the
surface was 84 rows and nothing pinned it.

### ✅ TWO LOAD-TIME INVARIANTS, AND THE GATE THAT UNCOVERED TWO OLDER DEFECTS

(i) **EDGE FIRES ⇒ CHILD AVAILABLE** and (ii) **NO EMPTY SURFACE ON A SERVED FORMAT**, corpus-wide and
format-INDEPENDENT (an arm no current target selects is exactly the arm that rots). ✔MEASURED over the
whole existing shipped set BEFORE any new row was added — 49 descriptors × {elf,pe,macho}, **0
diagnostics** — so neither is introduced on top of a live violation. An EMPTY served-format span is
refused: a sweep that cannot fail is not a sweep.

★★ **Turning the gate on exposed two PRE-EXISTING defects with nothing to do with the feature:**
an unavailable parent still recorded its **entire** `includes` closure, so the semantic tier injected
every sibling's surface for an `#include` it had just rejected — one diagnostic away from a silent
wrong-surface injection; and a closure sibling unavailable on the active format raised a diagnostic
naming a header the user never wrote while the preprocessor tier silently skipped the same case, which
is the tier drift the FC15c single-funnel design exists to prevent.

### ✅ THE HARNESS CAN NOW SAY WHOSE FAILURE IT IS

[[D-HARNESS-BUILD-FAILURE-HAS-NO-PER-TU-ATTRIBUTION]]. `poisoned` was decided by one predicate —
`grep -qE 'error\['` over the compile log — with no notion of whose error it was. ★★ **And the control
already existed; the REPORTING threw it away**: a reference build that RAN AND FAILED printed
`NO ORACLE`, identical to one that could not be attempted, so *"the reference agrees with us"* and
*"there is no reference"* rendered as one sentence. Now a third `CONFOUND_MATCH_KINDS` entry
(`build-tu`) beside `unit` and `abort-file`, same provenance discipline, same lint.
★★★ **A ROW ALONE EXCUSES NOTHING** — amnesty needs this run's oracle to have errored in THAT TU, an
active row naming it, AND every identifier-bearing DSS error subject in that TU to have been named by
the reference too. ⚠ Stated limit with its size: **cascade diagnostics naming no identifier are
unattributable — 44 of the 105 on the witness leg** ([[D-HARNESS-BUILD-ATTRIBUTION-BLIND-TO-CASCADE-DIAGNOSTICS]]);
and the CLI artifact has no oracle at all ([[D-HARNESS-CLI-ARTIFACT-HAS-NO-ATTRIBUTION-ORACLE]]).

Also landed: **run FIDELITY** — the resolver was computing `arch_ok` and discarding it, collapsing
`foreign-kernel` and `emulated` into one `launched` mode. ✔MEASURED on this project's own hardware:
`macho64-x86_64` on darwin/arm64 is `emulated` (Rosetta: foreign ISA, NATIVE kernel) while
`elf64-arm64` on windows/arm64 is `foreign-kernel`. `DSS_RUN_FIDELITY` selects on it in **both**
drivers. ⚠ Follow-on: `scope: emulated` and `fidelity: emulated` now disagree
([[D-HARNESS-CONFOUND-SCOPE-EMULATED-COLLIDES-WITH-RUN-FIDELITY-EMULATED]]).

### ★★★ THREE CONFORMANCE FIXES, ALL THE SAME SHAPE: **DSS DISAGREED WITH EVERY REFERENCE**

The rule is that reference compilers are the spec **BIDIRECTIONALLY** — DSS may not reject what they
accept, and may not silently accept what they diagnose. All three below were found by MEASURING both
directions rather than by chasing a complaint, and one of them exists only because the sweep was done.

**1. `P0014` incompatible macro redefinition was FATAL.** ✔MEASURED one TU per shape, mingw
`-std=c2x -pedantic`, cross-checked with MSVC `cl /Zs` (C4005): parameter spelling, replacement text,
arity, object-vs-function-like and variadic-vs-not **all warn at rc=0, none is fatal, and in every case
the SECOND definition is in effect at runtime** (verified by BUILDING AND RUNNING each shape, not by
reading the diagnostic). Only `-Werror` stops a build, which is a choice about warnings. DSS errored
AND retained the OLD definition. ★ **The value half is the one that could silently miscompile** — a
severity-only change would have left DSS agreeing about the diagnostic and disagreeing about the
program, so the early `return` had to go with it. Live consumer: sqlite `shell.c.in` defines
`S_ISLNK(mode) (0)` at line 141, BEFORE its `#include <sys/stat.h>` at 148.
⚠ **A LANE HAD ALREADY WORKED AROUND THIS AND SAID SO IN ITS OWN `$comment`** — it spelled our
descriptor's parameter `mode` to match shell.c, calling it *"a LOTTERY … Fixing P0014's severity in the
preprocessor is what actually closes the class"*. That fix landed, so the arm is back to `m`, matching
every other macro in the array, and the rationale now records that the spelling is **no longer
load-bearing** so nobody reintroduces it. ✔Re-measured: the shell.c gate order gives rc=0 with one
`warning[P0014]` and zero errors; `ext/misc/fileio.c` stays at 0 compile errors.

**2. The sweep found the OPPOSITE defect, which had no reporter.** `MacroDef::text` joined replacement
tokens with a single space UNCONDITIONALLY, so C 6.10.3p2's *presence, not amount* rule was erased:
`40+2` and `40 + 2` compared EQUAL and DSS said nothing where both references warn. Fixed with a
parallel `spacing` bitmap — kept a separate field rather than encoded into `text` with a sentinel byte
because **no byte is safe**: a string-literal token's text is its source SPELLING, and a source file may
legally contain any byte inside one. ★★ **This is why the shapes were swept individually.** The
over-strict half was reported by a consumer; this half had nobody to report it, and generalising from
the complaint would have shipped a fix that was still half wrong.

**3. A shipped OPAQUE tag could not be completed by the TU.** ✔MEASURED: completing `DIR` or `FILE`
was `rc=1 F_ShippedTypeIdentityConflict` on DSS and `rc=0 clean` on mingw; forward-declare-only, both
reverse-direction orders, and an unrelated-tag control were already clean. Root cause: **the descriptor
vocabulary had no spelling for "opaque"**, so an empty named struct stood in — and `type_interner.hpp`
says in its own words that `struct E {}` is *"a LEGAL COMPLETE zero-field struct (size 0)"*. The
descriptors' prose called those types OPAQUE and INCOMPLETE while the engine recorded COMPLETE: the
comment-holds-the-full-fact-while-the-code-uses-half pattern. ★ The incomplete machinery already
existed (`forwardComposite` / `isIncompleteComposite`); what was missing was a spelling and a
narrowing. The spelling follows the codec's OWN precedent — a bare keyword after the name, like
` packed` — and is TERMINAL, no braces. **50 spellings migrated across 3 files** (`FILE` alone is 46),
all together, because one leftover would mint a second COMPLETE `FILE` and split the identity
`sCtx.xCloser = pclose` is compared by. ⚠ NARROWED, NOT DELETED: two COMPLETE declarations that
disagree are still refused.

### ⚠ TWO SUSPICIONS OF MINE THAT MEASUREMENT KILLED — recorded because the near-misses were the useful part

* **"The opaque narrowing went too far."** A complete-vs-complete `struct dirent` case compiled clean,
  which looked like a hole the fix had just opened. ✔Disabling the arm and rebuilding showed it STILL
  compiled — so the case never reached the conflict path — and the same run confirmed the red-on-disable.
  The reason is [[D-FFI-DIRENT-API-DECLARED-OVER-VOID-NOT-ITS-OWN-STRUCTS]]: `readdir` is declared
  `fn(ptr<void>) -> ptr<void>` and NO shipped signature references the tag, so there is nothing to adopt.
  **No miscompile** — and a new row for the laxness that POSIX would not have.
* **"A registry row's cells are broken."** `D-LANG-PE64-HAS-NO-POSIX-DIRECTORY-API` read as six cells
  with its Closing-work column reduced to a bare backslash, and `check-anchor-registry.sh` passed anyway.
  ✔It has exactly 5 UNESCAPED pipes, its embedded C `||` is correctly written `\|\|`, and it matches its
  table's own header. **My reader split on every pipe including escaped ones**, and a scan I wrote
  assuming 4 columns everywhere produced 45 false positives — the file holds ~293 tables whose widths
  differ BY DESIGN. ★ A row's arity is only meaningful against its OWN table header. The guard was right.

### ★★★ INSTRUMENT LESSONS THAT OUTLIVE THIS CYCLE

* **A CROSS-REFERENCE SATISFIES `check-anchor-registry.sh`.** ✔MEASURED: two anchors reported
  UNREGISTERED, then reported OK after four *unrelated* rows were appended whose prose merely cites
  them — each still appearing exactly ONCE in the whole file, as that citation. The guard is meant to
  prove a deferral is RECORDED (trigger, closing work); a passing mention records none of it.
  [[D-GATE-ANCHOR-REGISTRY-GUARD-ACCEPTS-A-CROSS-REFERENCE-AS-A-REGISTRATION]].
* **A STRAY FILE NAMED AFTER A LANGUAGE BECOMES A SECOND LANGUAGE DOCUMENT.** `c-subset.lang.json.orig`
  and `.rej` both trip it; `zz.lang.json.bak` does not — the match is the language-NAME prefix, not the
  suffix. The message names neither file and reports a downstream extension collision instead. ⚠ Live
  tooling hazard: any script writing `<lang>.lang.json.tmp-*` beside the original and dying before its
  rename leaves the tree unbuildable with a message pointing elsewhere.
  [[D-CONFIG-STRAY-FILE-NAMED-AFTER-A-LANGUAGE-LOADS-AS-A-SECOND-DOCUMENT]].
* **A TEST PIN THAT MATCHES A WHOLE ROW VERBATIM BREAKS ON FIELDS IT SAYS NOTHING ABOUT.** Two pins
  reproduced `_WIN32`'s entire entry to locate it; both broke when `impliedSurface` landed. Re-anchored
  to the row's name plus the field each test is actually about. Not a weakening — the verbatim match
  only ever guaranteed the substitution happened.
* **ENCODE BEFORE YOU OPEN FOR WRITING.** A config-editing script opened a 433 KB `.lang.json` for
  writing and *then* hit a `UnicodeEncodeError` on a surrogate pair — truncating it to **0 bytes**.
  Recovered from the git index. Every editing script here now encodes and JSON-parses the payload
  first and `os.replace`s a temp file last.
* **A TEXT QUERY CANNOT ANSWER A CONTAINMENT QUESTION.** Adding the mandatory field to inline C++
  fixtures took three attempts: a POSITIONAL rule ("walk back to the nearest `predefinedMacros`")
  rewrote `"target":{"name":"X"}` across a raw-string boundary; a CONTENT rule (`"name"` + `"kind"`)
  rewrote 20 `relocations` entries that legitimately carry both. Only bracket-matching the array and
  verifying every occurrence fell inside it was correct. Same family as the anchor-count instrument
  that has been wrong twice.

---

## 0.0000 ★★★ CYCLE P6 — THE ASM BLOCKER BURN-DOWN, THEN THE IMPLEMENTATION HALF OF A TOOLCHAIN

**Operator argument: *"I WANT NO FAILURES, NO BLOCKERS, ADDRESS EVERYTHING."*** Lanes on disjoint
file sets. ✔**GATE, ALL THREE LEGS ON THE FINAL TREE: Windows 887/887 · WSL x86_64 887/887 · arm64
887/887** — the arm64 leg on the **REAL VPS hardware, not qemu**, each via `scripts/run-gate/run-gate.sh` with
its tool-emitted `100% tests passed` witness. Everything below is MEASURED by the lane that reports it.

### ★★★ THE HEADLINE: DSS NOW SHIPS THE IMPLEMENTATION HALF OF A TOOLCHAIN
`D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF` — operator §B ruling, **realized**. A shipped-header
descriptor may declare, per object format, that a symbol's body is PROVIDED by a shipped source unit
rather than IMPORTED: `"realization": { "pe": { "source": "runtime/platform/src/dirent.c" } }`.
✔**PREMISE MEASURED BEFORE ANY CODE:** a Windows host compiled a 2-CU program for pe64 and it **RAN,
exit 42**; a Linux host produced pe64 + elf64-aarch64 + macho64-arm64 and the aarch64 one **RAN under
qemu, exit 42** — the graph compiles a shipped unit FOR THE TARGET, cross, on every host.
★★★ **THE STRUCTURAL WIN IS PROVEN, NOT CLAIMED:** rename `d_name` in the descriptor's pe
`struct dirent` and `runtime/platform/src/dirent.c` **FAILS TO COMPILE** — the ABI agreement is
checked by the compiler, because the implementation `#include`s the declaration the descriptor
generates. ★ **COMPILE-ALWAYS ≠ LINK-ALWAYS**, measured both ways: `hello.exe` on pe64 carried
`_wfindfirst64i32`/`MultiByteToWideChar` before the split, **0 and 12288→3072 bytes** after, while the
consumer still links them and exits 42.

### ⚠ TWO THINGS THAT ARE **NOT** DELIVERED — do not read the cycle as though they were
1. **THE RUNTIME OBJECT CACHE HAS NO PRODUCTION CALLER.** ✔MEASURED: `resolveArchiveSiblingFormat`,
   `computeRuntimeObjectKey`, `lookupRuntimeObject`, `storeRuntimeObject` are referenced nowhere in
   `src/` outside their own TU. The invalidation property is proven in isolation; **every build still
   compiles the runtime unit from source every time.** Wiring it into the driver is the next step.
2. **THERE IS NO SHIPPED WARM CACHE.** Lane PK correctly declined to install an empty
   `dist/release/` — an `install(DIRECTORY)` at a non-existent tree creates an empty destination and
   reports success, reading for months as though the cache were shipping. ⇒ the *"users never pay the
   cold-build cost"* benefit that partly motivated choosing this option over shipping prebuilt objects
   **does not exist today**, and with two roots (shipped read-only + per-user writable) nothing yet
   bridges them.

### ✅ CLOSED THIS CYCLE
- **`.cfi_*` UNWIND PRODUCER, BOTH PORTS** (`D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`). gdb 15.1 walks
  a **3-frame stack and STOPS** out of a DSS ELF exec with **no frame pointers**; the matched control
  (same file, every `.cfi_*` deleted) runs off into a **stack address presented as a return address**.
  arm64's FDE is **rule-for-rule and delta-for-delta identical to `aarch64-linux-gnu-as` 2.42's**.
  ★★★ `.cfi_escape 0x10,0x06,0x02` **in this repo's own corpus file was a LIVE SILENT CORRUPTION** —
  it declares a 2-byte expression, supplies zero bytes, and makes GNU `as` itself eat the following
  `.cfi_restore 6` out of the unwind program at rc=0 with no diagnostic. gas validates nothing here.
  ⛔ pe64 is a MEASURED BOUNDARY: Win64 `UNWIND_INFO` needs `SizeOfProlog` and the GNU `.cfi_*` family
  has **no prologue-end verb** (checked over gcc's complete emitted set, nine spellings) — that is what
  `.seh_endprologue` is for. Refused by name.
- **`"+r"` TIED OPERANDS RUN** (`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE`) — exit 42, debug and release.
  The tie is a **location identity**, not new machinery: bind both halves to one `LirReg` and x86's
  two-address `add` composes for free while arm64's three-address `add` needs nothing.
- **bare `asm`** (`D-CSUBSET-INLINE-ASM-SPELLING`) — and the omission was **never neutral**: DSS was
  **ACCEPTING `int asm = 42;`**, which no reference compiler accepts in GNU mode. An invented
  extension, not a missing feature.
- **positional `%lN` refused at the semantic tier** so the two tiers agree · **template diagnostics
  render their source line** (and the row **under-counted** — every lowering-refusal was equally
  unrenderable) · **the same defect fixed in the FFI header reader**, found by looking for the class ·
  **the CLI runner builds 313 optimizer arms, 277 artifacts byte-differing** · **the two-runners rule
  got a machine check** (25 keys, zero difference) · **the anchor guard now scans `tests/` +
  `integrated_tests/`** (837 citations had never been checked) · the `got ` fragment · the S0067
  docblock · the LIR fixture that could not see an `__asm__` at all.

### ★★★ THREE INSTRUMENT LESSONS THAT OUTLIVE THIS CYCLE
1. **A VACUOUS ASM PIN CAN SURVIVE *BOTH* ARMS — `release` is not a rescue.** A **single `"+r"`**
   example stayed GREEN over a live mutant at debug AND release: the read half's `mov` targets a dead
   vreg, the result vreg's range starts right after, they never overlap, and the linear scan hands the
   result that register already holding the right value. Two tied operands → red on both.
2. **CLAUSE 5 HAS A MIRROR — prove the RESTORED bytes reached the process.** A harness restored the
   source without rebuilding, so its "good" column measured the MUTANT. **Both columns agreed
   perfectly**, which reads exactly like a stable measurement, and produced a false verdict that
   briefly looked like a real bug. Now in `references/the-bar.md` §A.5.
3. **A SAFE PORT CAN MASK A DANGEROUS ONE.** A `for (p : {kX86, kArm})` loop with an `ASSERT` aborted
   at the x86_64 half — where the mutant fails LOUD — and never reached aarch64, where the same mutant
   resolves **silently** to physical x30. All 17 port loops now run through a `void` callable.

### ⚠ AND ONE ABOUT THE ORCHESTRATION ITSELF
`D-BUILD-LANES-SHARE-ONE-CONFIG-TREE-AND-ONE-WORKING-TREE`: a per-lane `build-<lane>` dir isolates
OBJECT CODE and nothing else. **Config is runtime data read from the shared source tree**, so one
lane's `.lang.json` edit is instantly live for every other lane's already-built binary — one lane's
"baseline 874/874" was measured across a config edit and **was not a baseline of anything**. A partial
build also relinks the shared DLL. ⇒ per-lane git WORKTREES, or stage config into the build dir.
The orchestrator launched six lanes into one tree; the method outran the isolation it assumed.

### ★★★ A GUARD WHOSE PASS WAS MANUFACTURED BY ITS OWN BUG REPORT — and the fix repeated the defect twice

`D-GATE-ANCHOR-CITATION-RESOLVES-VIA-ITS-OWN-BUG-REPORT` ✅ CLOSED. Citation resolution in
`check-anchor-registry.sh` is **substring-anywhere over `.plans/`**, and that tolerance is
**load-bearing, not sloppiness** — ✔MEASURED: of **1181** unique citations, **874** resolve to a
registry row key and **307 only via prose**, of which **160 are line-wrap fragments** of one real
name (`…-NOT`, `…-NOT-ADDRESSABLE`, `…-NOT-ADDRESSABLE-AT-AN` are ONE anchor split across comment
lines). ⇒ The cost of the tolerance is that **it cannot tell a LIVE name from a DEAD one.**
✔The proof: `D-ASM-INDIRECT-BRANCH-SUCCESSOR-SET-UNDERIVABLE` was cited in
`tests/asm/test_asm_text_to_lir.cpp:1528` after a **one-word rename** to `…-UNSTATED`, and the ONLY
text in `.plans/` supplying its resolution was **the row written to report it as stale**. It had also
never been checked at all before this cycle widened the guard to `tests/` — so it went from
UNSCANNED straight to FALSELY-RESOLVED, validated at no point.

**★★★ THE TRANSFERABLE PART IS THE FIX'S OWN TWO FALSE POSITIVES, each the previous rule's failure
mode:** (1) matching the words `RETIRED ID` in a status cell red on the row whose prose merely says
*"as a retired id"*; (2) matching the unspaced token `RETIRED-ID` then red on **the row that
DOCUMENTS the token** — the row explaining the rule was classified BY the rule, the same
self-reference the check exists to kill, one level up, within minutes. ⇒ **A marker that is merely
PRESENT can always be tripped by writing about it.** The marker is now **POSITIONAL** — the status
cell must OPEN with the retirement (`^ *\**✅ \*\*CLOSED <date> — RETIRED-ID`), where prose never
sits, which is the rule the anchor-balance instrument already used (CLOSED iff the cell BEGINS with
`✅`). Comparison against citations is **EQUALITY, not substring**, since a retired id may be a
PREFIX of a live one. Exit **4**, fail-closed at 0 marked rows, ✔red-on-disable measured BOTH
directions (plant a retired id ⇒ exit 4 naming the file:line; remove ⇒ green — the clause-5 mirror).
⚠ **NOT fixed, and anchored rather than glossed:** `D-GATE-ANCHOR-DECLARATION-SITE-UNDEFINED-ACROSS-PLAN-FORMS`
(OPEN). The general tightening — resolve only against a plan **declaration site** — reds **147**
citations, and spot-checks show most are legitimate plan deliverable ids declared in shapes a
first-cell table matcher cannot see. **Triage the 147 before tightening; a gate that reds 147 without
a per-name verdict is a wall, not a gate.** ⚠ An earlier pass this cycle cried *"46 dangling
citations"* from a regex broader than the guard's own (1404 vs 1181, catching `D-32-BIT-WORD`);
re-measured with the guard's EXACT regex and include-set, **0 resolve nowhere. The alarm was the
instrument.**

### ★★★ A PLATFORM SPLIT THAT WAS A DEFECT IN DISGUISE — and the GREEN platform was the liar
The WSL leg red one test the Windows leg passed: the new sigil detector's
`AnOmittedRoleIsRefusedAndAnExplicitNullIsHonoured`. Root-caused rather than retried, and it was not
a toolchain quirk.
✔MEASURED: **the two shipped dialects genuinely give OPPOSITE answers to `symbolicNameClose: null`**,
because the role's bound KIND has different provenance in each. `asm-x86_64-att` binds
`PlaceholderNameClose`, which **no other lexeme declares** — so the synthesized row was its only
declaration, null left the binding dangling, and the load FAILS (loud). `asm-arm64-gas` binds
`BracketClose`, which its **global `"]"` row already declares for the memory form `[x0, #8]` — so the
kind stays interned, the load SUCCEEDS, **and the template goes on accepting `%[name]` through the
mode's global fallback.** ⇒ **the language declared the form ABSENT and one dialect kept parsing it.**
An accept-and-do-nothing — the worse of the two outcomes — and it was the GREEN platform hiding it.
★★★ **THE MECHANISM OF THE DISGUISE, and it is the durable lesson:** the pin asked
`dialects.front()` — ONE member of a **discovered** set — and `directory_iterator` is **sorted on
NTFS, hash-ordered on ext4**. ✔The enumerations are the measurement: NTFS put arm64 first (the silent
accept ⇒ green), ext4 put att first (the loud path ⇒ red). ⇒ **A genuine two-outcome defect was
collapsed into a green/red platform split, which reads as an environment problem and invites entirely
the wrong investigation.** ⚠ **Discovering a set and sampling one member is STRICTLY WORSE than
hard-coding one, because it LOOKS exhaustive** — this file discovered its dialects deliberately so a
third would be covered automatically, then threw that away at the last step.
⇒ `D-TEST-PIN-SAMPLES-ONE-MEMBER-OF-A-DISCOVERED-SET` (OPEN). ⚠ A grep cannot sweep it: 24 files under
`tests/` use `directory_iterator`, and a naive `.front()`/`[0]` scan returns **391 hits across 18
files**, nearly all ordinary indexing. It is a data-flow question, so a mechanical sweep would be a
wall, not a gate.
★★ **THE FIX IS AT THE LOADER'S INTERSECTION, NOT IN EITHER DOCUMENT.** Dropping the shape or its
reference would let a LANGUAGE's null silently delete a production from ANOTHER document's grammar —
a second owner of the grammar, i.e. the closed row's own defect one tier up. Four cases now closed and
decidable from the two documents alone: declared+bound → row; declared+unbound → LOAD ERROR;
**null+bound → LOAD ERROR (new)**; null+unbound → HONOURED, and the scan stops recognizing the form.
A binding exists exactly when the entry's closure spells the hole, so the verdict can never again
depend on what else is interned, in what order, or on which filesystem.
✔**BOTH PLATFORMS 881/881, 0 FAILED.** ★ The two halves were measured NON-REDUNDANT: the test fix
alone reds BOTH platforms; the loader clause is what makes both green.

### ✅ MACH-O TOO — WITNESSED BY APPLE'S OWN CRASH REPORTER, and it found a REAL capability gap
The operator brought macOS up mid-cycle, which FIRED the trigger on
`D-LINK-MACHO-IMAGE-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS` — the row the ELF lane had **correctly
refused to close blind**, because the bar wants execution and the Mac was off. ✅ Now closed on real
Apple Silicon (macOS 26.5.2): **Apple's own crash reporter**, unwinding a genuinely faulting stack and
symbolicating from the image nlist, went from `#0 sym_83 / #1 sym_88 / #2 sym_92` to
`#0 static_helper / #1 global_helper / #2 main` — **every offset identical**, so name-only — with the
trampoline correctly still synthetic. The functional change is **2 lines**, routing both image sites
through the *same* shared `imageName` the ELF lane added; a format-selecting ternary was DELETED.
⚠ **HONEST LIMIT, stated rather than blurred:** lldb could not attach — the Mac reports *Developer
mode is currently disabled*, and enabling it is a `sudo` security change a lane must not make. lldb
witnessed only statically. **The live-stack witness is the crash reporter, not lldb.**
★ The dylib control was run with a probe built by **Apple's own clang**: `_lib_static_helper` present
in the nlist, `dlsym` for it **NULL**, `dlsym("dss_lib_entry")` resolves and CALLS. The image tier
moved; the ABI surface did not.

### ✅ …AND IT WAS CLOSED THE SAME DAY — APPLE'S `ld` NOW ACCEPTS A DSS MACH-O IMAGE
`D-LINK-MACHO-LINKEDIT-SYMTAB-MISALIGNED` ✅. Same `ld` invocation, before and after, on real Apple
Silicon (macOS 26.5.2, **ld-1267**): arm64 dylib as a link input **rc=1 → rc=0**, and the linked
client then **RAN**; arm64 exec via `-bundle_loader` rc=1 → rc=0; x86_64 dylib rc=1 → rc=0. ✔`symoff`
**50076 → 50080** — the anchor's own number, reproduced INDEPENDENTLY.
★★ **ROOT CAUSE: THE INVARIANT LIVED IN THE PRODUCERS, WHICH IS WHY IT HELD FOR THREE BLOBS AND NOT
THE FOURTH.** The rebase, bind and export-trie builders each pad their OWN tail "so the next payload
starts aligned". The indirect symbol table has no such step — it is `count * 4` — so an **ODD**
indirect count (any module with a DATA extern, i.e. every `#include <stdio.h>` TU) left the nlist at
4 mod 8. ⇒ **a producer padding its own tail asserts NOTHING about the next blob's start.** The rule
moved to the consumer, and the whole cursor chain was audited — including the ones already correct,
three of which were correct only *accidentally*, via those producer pads.
★★★ **THE MIRROR CAUGHT ITSELF, and this is the most transferable result of the cycle.** The lane's
first restore used `shutil.copy2`, which **preserves mtime** — so ninja said *no work to do*, the
binary never relinked, and the "restored" ctest was still rc=8. **The restored bytes had never
reached the process.** Re-run with an explicit `utime`: sha matches, binary relinked, rc=0. ⇒ clause
5's mirror is not ceremony — **a restore can silently fail to reach the process exactly as a mutant
can**, and without the mirror this would have read as "the fix is load-bearing" when nothing had
changed at all.
★★ **M3 caught a gap in the test's own first draft:** an offset-only pin would PASS a fix that
aligned the CURSORS while the BYTES landed 4 earlier — a correct `symoff` published over the WRONG
table, which is precisely the silent miscompile the row is about. The test now reads the nlist back
THROUGH the published offsets. ★ Each cell asserts its own reachability (`nIndirect % 2 == 1`),
because an EVEN count leaves the packed cursor aligned BY LUCK and every assertion would hold
pre-fix. ⚠ And the reloc kind is resolved BY NAME: `RelocationKind{4}` is `abs64` on arm64 but
`tls-tpoff32` on x86_64, so the copied literal had silently made the x86_64 cells test NOTHING.
★ Codesign re-verified rather than assumed (`--strict --deep`, rc=0 both tiers, plus the CodeDirectory
arithmetic read directly); the `.o` tier proven untouched TWICE — Apple `cc` links and runs it, and
the emitted `.o` is **byte-identical** across the fix.
⚠ **ONE HONEST ASYMMETRY:** the x86_64 exec was ACCEPTED as a `-bundle_loader` while misaligned where
arm64 was refused, and `nm -m` proves that link really did read the misaligned table. **Apple's
refusal is not uniform across (arch, link-role) — 3 of 4 pre-fix probes refused, 1 not.** One
accepting tool proves nothing.

### ★★★ AND THE BIGGER NEWS: APPLE'S `ld` CANNOT LINK AGAINST A DSS MACH-O IMAGE AT ALL
`D-LINK-MACHO-LINKEDIT-SYMTAB-MISALIGNED` 🔴 OPEN, HIGH. The dynamic image packs `symtabOff` hard
against the preceding `__LINKEDIT` blob with no alignment step; `nlist_64` carries an 8-byte `n_value`
and Apple requires 8-byte alignment. ✔MEASURED: `symoff` = **50076 (≡ 4 mod 8)** on both the exec and
the dylib, **before and after** the name fix ⇒ pre-existing and orthogonal.
★★ **THE FIRST READING WAS WRONG AND WAS CORRECTED RATHER THAN QUIETLY REWRITTEN.** `dyld_info`
refusing the image looked like one fussy inspector — until **Apple's own `ld`** was asked to link
against the dylib and refused in the same words, `ld: mis-aligned LINKEDIT content 'symbol table'`, on
all four probes. ⇒ **a DSS-built Mach-O dylib cannot be consumed as a LINK INPUT by the Apple
toolchain.** Control that makes it ours: the same `dyld_info` reads `/usr/lib/libSystem.B.dylib` fine.
★ Blast radius bounded by measurement: the MH_OBJECT writer lays out its own LINKEDIT and is
UNAFFECTED — a DSS `.o` linked by Apple `cc` against an Apple-compiled `main` gave rc=0 and RAN, so
the c139–c142 relocatable arc is intact; dyld still LOADS and RUNS these images. ⇒ **an inspector
disagreeing with you is a hypothesis; the reference toolchain refusing you is the finding.**

### ✅ DSS BACKTRACES NOW NAME REAL FUNCTIONS — and the row's own stated blocker was FALSE
`D-LINK-ELF-EXEC-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS` ✅. gdb over a DSS ELF exec went from
`#0 sym_84 / #1 sym_89 / #2 sym_93` to `#0 static_helper / #1 global_helper / #2 main`, at
**BYTE-IDENTICAL addresses** — name-only — on x86_64 native and aarch64 under qemu's gdbstub. Naming
`main` also restores gdb's stop-at-main policy. **Three code lines**, via a new
`ObjectSymbolNames::imageName` that sits in the SAME owner as `definedName` and differs in exactly one
clause (no `isExternallyVisible` gate).
★★ **THE TIERS LEGITIMATELY DISAGREE, and that is now written down rather than incidental:** a `.o`'s
names ARE a foreign linker's resolution keys, so a real-named `static` collides across TUs; a FINAL
IMAGE is never re-linked, so its `.symtab` resolves NOTHING and is read only by debuggers — where the
real name is the wanted answer, and is what gcc emits. ★ **The `.so` proves the tiers stayed
separate:** in ONE binary `static_helper` is in `.symtab` and correctly ABSENT from `.dynsym`.
★★ **BOTH image builders were broken and the LIVE one was the DYNAMIC one** — every shipped exec/PIE
spells `processExit` as a by-name import of `exit`, so every real executable routes through
`encodeElfExecDynamic`; the static ET_EXEC arm needs zero externs. ⇒ **a pin on the "minimal ET_EXEC"
alone would have tested the DEAD arm.**
★★★ **THE ROW'S OWN STATED BLOCKER WAS MEASURABLY FALSE.** The code comment justified the synthetic
name by claiming entry resolution matched `entryPoint` against the reconstructed `.symtab` name.
✔MEASURED: `resolveEntryFnIdx` RE-DERIVES `<prefix><SymbolId>` from the id and **never opens the
symtab** — all four entry tests stay green, including one with `entryPoint = "sym_42"`, and every
shipped exec declares `entryPoint: ""` anyway. **A comment recording a coupling the code did not have
is what kept this defect alive for its whole life.** Same family as the `hwtime.h` blocker and the
inverted `__text` alignment finding: *the blocker was in the prose, not the code.*
✔3 mutants, 5 clauses + mirror; **M2's line count identical (4687/4687)** with a differing hash; M1/M2
are EXACT COMPLEMENTS so neither arm can mask the other; all 17 cells run through a `void` callable.
★★ **A RESTORE THAT CAN REVERT MORE THAN THE MUTANT IS NOT A RESTORE.** The lane's first driver
restored with `git checkout -- <file>`, which silently reverted **the uncommitted fix itself**, so the
mirror read rc=1. ⇒ snapshot the working file and restore FROM THE SNAPSHOT; never use a VCS-relative
restore while the subject is uncommitted.
⇒ Three follow-ons anchored, all measured against **gcc on the same source**: statics emit
`STB_GLOBAL` where gcc emits `LOCAL` (deliberately not bundled — it moves statics into a local-first
prefix and falsifies a hardcoded `firstNonLocal = 2`, which would have hidden a symtab-layout change
inside a change whose whole proof was byte-identical addresses); an image `.symtab` carries **no data
symbols at all**, so a debugger can name every frame but not one variable; and the Mach-O sibling,
**correctly left unfixed** — the fix is two lines and the shared `imageName` already exists, but the
bar requires witnessing by EXECUTION and the Mac is off, so landing it would be the speculative build
§A.2 forbids.
✅ And a fourth, **fixed on the spot**: `D-TEST-BUDGET-THREADING-PRIVATE-REPO-ROOT-WALK-FAILS-OUT-OF-SOURCE`
— the last private `repoRoot()` in the tree, walking from cwd only, so the SAME binary passed inside
the repo and failed from an out-of-repo build dir. **Third instance this cycle of the
already-fixed-next-door pattern:** `test_header_name_matching.cpp:471` sits two files away carrying
the identical fix and the identical written reason. ⇒ **a consolidation is finished when nothing
private survives, and only a SCAN tells you which of those you have.**

### ✅ THE TWO `asm goto` / PARAMETER-OUTPUT ROWS CLOSED — and a POLICY OWNED A FACT IT COULD NOT KNOW
`D-OPT-ASM-GOTO-WITH-OUTPUTS-ABORTS-THE-MIR-REBUILDER` ✅ + `D-CSUBSET-ASM-OUTPUT-ON-A-PARAMETER-NOT-ADDRESS-TAKEN` ✅.
★★★ ROOT CAUSE of the first: `MirRebuildPolicy::recordTerminatorInRewrite()` let a **pass declare**
that nothing downstream reads a terminator as an operand — and `Dce` and `SimplifyCfg` both declared
it. **The declaration is FALSE:** a `ReturnPiece`'s single operand ANCHORS IT TO ITS PRODUCER, and for
an `asm goto` WITH OUTPUTS that producer IS the block's terminator. ⇒ **whether an old id is
referenced is spelled out in the MIR's OPERAND LISTS the rebuilder is already walking — it is not a
fact a pass can own**, so the hook was **DELETED** rather than re-answered. Neither `false` reason
survived contact: Dce's was cost (one hash insert per block), SimplifyCfg's a hypothetical about a
folded `CondBr` whose recorded entry is in fact ACCURATE *and* unreachable. Withholding it converted a
speculative wrong-shape read into a guaranteed `std::abort`.
★★ **A SECOND CLONER, FOUND BY PROBING RATHER THAN BY READING:** with the substrate fixed, the same
shape inside an inlined `static` callee STILL exited 127 — `Inlining` splices callee bodies with its
OWN walk and its OWN `local` map, which a shared-substrate fix does not reach. ⇒ *fixing the substrate
does not fix the code that declined to use it.*
✔28 CLI compiles (7 shapes × 2 configs × 2 formats) all rc=1 on the same honest refusal, ZERO aborts
(pre-fix release rc was 127). 3 mutants, each rc **0 → 8 → 0**, mtimes moved BOTH directions; two had
**IDENTICAL line counts** (661=661, 1655=1655) — exactly what a line-count criterion misses. Under
every mutant the four PRE-EXISTING carriage tests stayed GREEN; only the new anchor pins caught it.
★★ **THE LANE CORRECTED ITS OWN ROW'S CONTROL, and that is the third instance of the shared-tree
defect** — the row claimed `"+r"` on a parameter "compiles and exits 42"; re-measured at pristine HEAD
with `DSS_CONFIG_ROOT` pinned to a clean worktree it is REFUSED, because the original figure had been
taken against a build carrying **another lane's uncommitted `mir_to_lir.cpp`**. ⇒ **a shared tree's
damage outlives the session: a wrong build makes a wrong number, and the number gets written into a
row as ✔MEASURED.** Pin `DSS_CONFIG_ROOT` for any figure that will be WRITTEN DOWN.
⚠ **STILL UNMEASURED, and the lane retracted the claim rather than let it stand:** whether `"+r"` on a
parameter now COMPILES with Lane L's `tieAsmReadWriteOperands` in scope. It measured parity only in
its own worktree at pristine HEAD. **Re-measure against the merged tree before treating
`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE` as reachable.** ✅ **THE PROBE RAN — THE COMPOSITION HOLDS, ✔MEASURED 2026-08-17 on the merged
tree:** a tied `"+r"` output bound to a **non-address-taken parameter** now COMPILES **and RUNS, exit
42, at BOTH `debug` and `release`** on pe64-x86_64 (`static int bump(int v){ __asm__("addl $20, %0" :
"+r"(v)); return v; }` over a `volatile` seed of 22). So Lane L's `tieAsmReadWriteOperands` and Lane
M's address-taken marking compose, and [[D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE]] is already ✅ CLOSED by
Lane L. ★ **The lane was RIGHT to retract rather than assert it** — it had measured only its own
worktree at pristine HEAD, where the refusal genuinely fires; the claim happened to be true, and it
still could not have known that. **A correct guess withdrawn is worth more than a correct guess
kept**, because the next one would not have been true.
⚠ **AND ITS GATE DID NOT TERMINATE — reported as unmeasured, which is the standard to copy.** An
earlier run of its did reach 875/875 and is **VOID**: it ran `ninja` mid-ctest, producing ~400
spurious `***Exception` lines and one invented failure (`double_to_unsigned`, green on a clean re-run).
⇒ **an overlapped ctest run can invent a failure, so it can equally mask one — void in BOTH
directions.** Second mechanism from the same incident: **killing the wrapper does not kill `ctest`**,
which kept running and writing. Both now recorded in `references/gate-and-cross-plan.md`.

### ★★★ A DEFECT FOUND TWICE BY HAND IS EVIDENCE OF A POPULATION, NOT OF A PAIR — 2 became 61
`D-TEST-SEMANTIC-FIXTURE-ABORTS-THE-WHOLE-BINARY` ✅ CLOSED: the semantic fixture did
`ADD_FAILURE()` then **`std::abort()`**, which kills the whole test PROCESS — every sibling test in
that executable loses its verdict. ✔MEASURED, not hypothetical: a config-mutating pin drove
`loadShipped` to a **legitimate** refusal and the binary died `0xc0000409` mid-suite, **taking nine
passing tests' results with it** and reporting an exception code instead of the load error. ⇒ **the
arm that was working correctly is the one that destroyed the evidence.**
★★ **The project had already made this exact fix one layer down and written down why**
(`tests/test_support/repo_root.hpp:59` — *"`std::abort()` kills the whole test BINARY … `repoRoot()`
throws instead"*), and it did not propagate. **A recorded lesson is not a fixed defect: nothing makes
a call site read a neighbour's comment.**
⇒ So it got a guard: **`scripts/check-no-abort-in-tests/check-no-abort-in-tests.py`**, wired into the gate battery, 14 stripper
self-tests + a line-number-preservation test. ★★ It strips comments/strings/raw-strings BEFORE
matching because **a bare token grep reds on the very file that documents the fix** — the same
merely-present-marker shape as the retired-id check above.
★★★ **THE SCOPE IS THE RESULT: the row named 2 sites; the first scan found 61 LIVE SITES ACROSS 29
FILES** — the identical `ADD_FAILURE(); std::abort();` idiom copy-pasted (worst:
`tests/hir/test_hir_lowering_c_subset.cpp`, **11**). Shipped as a **RATCHET** (per-file ceilings that
may only come DOWN; a new site reds, and a *fixed* site also reds until its ceiling drops, because
unclaimed headroom is where the next regression hides). ⚠ **`INVENTORY` is deliberately NOT
`ALLOWLIST`** — an allowlist claims *aborting here is right*; these 61 claim only *unfixed debt
predating the guard*. Merging them would launder 61 unexamined sites as 61 proofs. `ALLOWLIST` is
empty and that is measured. ✔4 arms, all discriminate. **`D-TEST-ABORT-IN-A-FIXTURE-HAS-NO-GUARD`
stays OPEN until the inventory is empty — a guard existing is not the debt being paid.** ⛔ The
61-site sweep was NOT attempted this cycle, and the reason is recorded rather than implied: three
lanes held `test_mir_lowering_c_subset.cpp` (5), `lowered_lir_fixture.hpp` (3) and the semantic tree,
and sweeping files under concurrent edit is how a fold loses somebody's work.

### ⚠ THE RELEASE-ARM GAP IS **184**, MEASURED — and it is the §B, not a shrug
✔MEASURED 2026-08-17: of **577 runnable** manifests (24 more are diagnostic-only), **393 carry a
shipped `release` arm**, **86 carry arms but no release arm**, and **98 carry no arms at all** ⇒
**184 runnable examples witness the front end and codegen and say NOTHING about the optimizer.** The
project's own rule in `examples/README.md` already mandates the arm; these manifests simply predate
it. ⇒ `D-EXAMPLES-OPTIMIZER-WITNESS-IS-A-HAND-LISTED-PASS-SUBSET`.
★ **Why it was NOT bundled into this cycle, and this is an attribution argument rather than a
capacity one:** 184 new arms is a large new validation surface, and any red would be indistinguishable
from a red caused by this cycle's asm/CFI/config work. **Control the variables** — it deserves its own
cycle and its own investigation budget, because a release-only failure among those 184 is exactly the
class this project most wants to find (`D-OPT-VARIADIC-RELEASE-MISCOMPILE` shipped that way).

### ★★ OPERATOR INSTRUCTION 2026-08-17 — ONE BUILD ROOT, AND LANE BUILDS GET CLEARED
Verbatim: *"EVERY build must be inside build directory (we can have multiple subdirectories for
distinct builds), but only 1 root build. Also, lane builds MUST be cleared once everything is fine,
so we keep the storage good and also organize build directories."* ⇒ written up as
**`.claude/skills/dss-cycle/references/build-layout.md`** and wired into that skill's file map (read
at step 5 before creating a build tree, and at step 11 before reporting a cycle complete).
★★★ **The argument is not tidiness — a flat `build-*` namespace has ALREADY shipped a bug here, this
cycle:** an rsync exclude written unanchored as `build*` silently matched
`src/program/build_scripts.cpp`, so the WSL leg configured against a tree missing a changed `.cpp`.
With ONE root the exclude is `/build/` — one anchored path, no glob, nothing for a SOURCE file named
`build_*` to collide with ⇒ **the class disappears instead of being re-fixed in each rsync, script,
doc, `.gitignore`, and the `dss-state` driver's auto-pick.** ✔MEASURED storage half: **11 root build
trees / 54.3 GiB**, seven of them one-cycle lane builds at ~5.6 GiB each — **27 GiB reclaimed on the
spot**, plus 16 GiB from two folded worktrees (`dss-lane-l`, verified by CONTAINMENT of its
contribution, not by byte-identity — later edits legitimately stack on top).
⚠ **NOT MIGRATED YET, and it is not a `mv`:** `build-dbg` is named in **47** files and `build-rel` in
**16** (scripts, CI, docs, `dss-state`'s driver) ⇒ `D-BUILD-LAYOUT-FLAT-ROOT-BUILD-DIRS-NOT-MIGRATED`,
to be sequenced for a QUIET tree because it edits the very scripts the gate runs. A missed reference
fails in the worst available way: a script that silently configures a new EMPTY build tree and reports
a pass over a scan of almost nothing. ⛔ Two worktrees (`dss-wt-bitwise`, `dss-wt-movzw`, ~25.7 GiB)
hold **118 and 124 uncommitted paths** at old commits and were deliberately NOT deleted — that is an
operator call, not a cleanup.

### ⚠ `examples/README.md` WAS STALE IN THREE PLACES, AND ONE WAS INVERTED INTO HARMFUL ADVICE
✔RE-MEASURED 2026-08-17: the optimizer-arm figures read **460/652/374/278**; the truth is
**477 of 600 manifests / 669 arms / 391 release / 278 inline**. Three of four stale — and `278`
survived only because **every arm added in between was a `release` arm**, which is exactly the
coincidence that makes a stale count look verified. The prose claimed `optimizedPipelines` and
`expectedStdout` are read by the in-process runner **ONLY**; this cycle taught the CLI runner both,
so the paragraph understated coverage. ★★ **Worst of the three: rule 3 told authors NOT to give a
project-mode example a release arm**, reasoning from that same false premise. ✔MEASURED at both
sites, the prohibition is dead: `--config` is a GLOBAL CLI flag (so the CLI runner applies an arm to
a `--project` build) and the in-process pipeline override is `Program` state read by the delegated
`compileFiles`/`compileUnits` (so it survives `compileProject`). ⇒ A project-mode example without a
release arm now witnesses the optimizer in **NEITHER** runner. **The corpus's ONE project-mode
example had 0 arms and its own `$comment` named its own trigger — *"add the release arm the day the
CLI runner grows `--config`"*. That day arrived this cycle; the arm is in.** ⇒ **A manifest comment
that states its own firing condition is a scheduled task, and nothing was scheduled to read it.**

---

## 0.000 ★★★ P5c — A SILENT MISCOMPILE IN SHIPPED EXTENDED ASM: EVERY UNPINNED INPUT READ AN UNDEFINED REGISTER

**✔MEASURED 2026-08-17 on the compiler that had just passed 873/873.**
`__asm__("movl %1, %0" : "=r"(r) : "r"(a))` with `a == 42` compiled **rc=0** and the program
**returned 0** — on **BOTH** `pe64-x86_64` and `elf64-x86_64`, at **BOTH** debug and release.
The disassembly named it outright: the input's load defined the register the OUTPUT had been
allocated, and the template's `%1` read one nothing ever wrote — `mov 0x0(%r14),%r14d` (load `a`),
then `mov %r15d,%r14d` (the template), with r15 untouched since the prologue.

**Cause (`expandInlineAsm`, `src/lir/lowering/mir_to_lir.cpp`):** the materialisation loop opened
`if (!ins[j].pinned) continue;`, so only PINNED inputs were moved into their bound register.
`bindAsmOperand` mints a FRESH vreg for an unpinned operand and, for an INPUT, **nothing else ever
writes it.**
★★ **THE FALSE SYMMETRY IS THE LESSON.** The capture loop twelve lines below skips unpinned OUTPUTS
for a correct reason its own comment states — *the template wrote that vreg, so a copy would be dead*
— and the input loop reads as its mirror image. It is not one: an output is written by the TEMPLATE,
an input must be written by the LOWERING.
★ **A second defect hid underneath and only surfaced once the first was fixed:** `needMoves` gated
resolution of `MnemonicSlot::Mov` on *any operand pinned*, so with unpinned inputs alone `movOp`
stayed disengaged and the corrected loop dereferenced an empty optional
(`LirBuilder::addInst: Invalid opcode`). One omission concealed both halves.

**✔MEASURED FIXED:** the four probes (`movl $42,%0` / `movl %1,%0` / two-input `"=&r"` / arm64
`add %0,%1,%2` on `long`) all exit **42**, debug AND release, on pe64 native, WSL elf64-x86_64 and
qemu-aarch64. Anchor `D-LIR-ASM-UNPINNED-INPUT-NEVER-MATERIALISED`, born ✅ CLOSED (balance net 0).

### ★★★ WHY 873 GREEN TESTS SAW NOTHING — the durable finding
The corpus's **three** inline-asm examples declared **ZERO input operands between them**:
`c_inline_asm` is the EMPTY template; `c_inline_asm_extended` is register-PINNED OUTPUTS (`rdtsc`)
on x86_64 and a pure CLOBBER list on aarch64. ⇒ **A FEATURE'S COVERAGE IS AS WIDE AS THE OPERAND
SHAPES ITS TESTS NAME, NEVER AS THE NUMBER OF TESTS.** Counting examples said inline asm was well
covered; counting SHAPES said inputs had never once run.
⚠ It also re-reads the cycle that shipped it: `hwtime.h` compiling was a TRUE result *because*
`rdtsc` has outputs and no inputs — the motivating construct could not have caught this.

### ★★ AND THE PIN ITSELF NEARLY SHIPPED A FALSE CLAIM
New example `examples/c-subset/c_inline_asm_operands`. I first wrote that it *"discriminates at both
arms, because a register nothing wrote is undefined at every optimization level"* — reasoning that
sounds airtight and is **WRONG**. ✔MEASURED with the mutant restored and the example reduced to the
call-shaped two-input helper alone: **baseline exited 42 (GREEN — the mutant SURVIVED)**, release
exited 1. Adding a single-input shape lowered **directly in `main`** made the same mutant fail the
BASELINE arm outright. ⇒ a pin that survives because an undefined register HAPPENED to hold the right
value is not a pin, and which shape gets that luck cannot be read off the source. The example
carries **both** shapes and must not be "simplified" to one.
⚠ The original claim came from a standalone probe of a DIFFERENT program shape — a property measured
on one subject and asserted about another.

### 0.001 ✅ THE TWO OPERATOR-QUEUED TASKS (§0.00) ARE DONE IN THIS SAME CYCLE

**TASK 1 — asm output store-back through `emitScalarStore`.** ✔**IT WAS A LIVE VERIFIER FAILURE, not
only a quality gap** — the fixture was written FIRST to settle exactly that, as §0.00 instructed:
`_Atomic int g; __asm__("movl $42, %0" : "=r"(g));` exited **rc=1** with
`error[I_AtomicAccessNotLowered] … plain 'store' to an _Atomic-qualified pointee`. Valid C, refused.
The `volatile` half was the SILENT one: it compiled, ran, and simply lost the flag.
Both sites now call `emitScalarStore(st, pieceTypeFor(k), kids[k])`; the second pass stays a second
pass (the producer→`ReturnPiece` adjacency window must still close first).
Pin `MirLoweringCSubset.AsmOutputStoreBackGoesThroughTheScalarFunnel` — volatile arm, `_Atomic` arm
(AtomicStore == 1 **and** plain Store == 0), and a **plain control** proving the lowering does not
stamp volatile on everything. ✔RED-ON-DISABLE: reverting one call reds all three arms, control green.
Anchor `D-MIR-ASM-OUTPUT-STORE-BACK-BYPASSES-THE-SCALAR-FUNNEL`, born ✅ CLOSED.

**TASK 2 — pin `isExtended` + `tiedOutput` through the MIR rebuild.** Sentinel sets both to
NON-DEFAULT values (`tiedOutput` names output **1**, so a drop re-defaulting to `0` stays visible);
`expectSentinel` asserts engagement with `ASSERT_TRUE(...has_value())` before the value, since
`.value()` on a dropped optional aborts instead of naming the site. ⚠ Adding an input made the
descriptor's input count non-zero, and `MirBuilder::checkAsmOperandAlignment_` **aborts** on a
descriptor/operand mismatch — so all three fixtures plant the asm through one `plantSentinelAsm`
helper instead of three call sites that can drift. ✔RED-ON-DISABLE: a field-by-field copy at rebuild
site 1 omitting exactly those two fields reds both new assertions; reverted.
Anchor `D-TEST-MIR-ASM-DESCRIPTOR-NEW-FIELDS-UNPINNED-THROUGH-REBUILD`, born ✅ CLOSED.

### 0.002 GATE STATE — ✅ ALL THREE LEGS GREEN ON THE FULL DIFF
✔ **Windows ctest 874/874, rc=0** · ✔ **WSL x86_64 + qemu-arm64 874/874, rc=0 under
`DSS_STRICT_ARM_VERDICTS=1`** · ✔ **`integrated_tests` (the CLI-subprocess runner) passed, and the new
example is genuinely IN it** — verified by name in `ctest -V`, not inferred from the glob, because a
capability that reaches one runner while its sibling shrugs is a silent harness bug.
Anchor balance **net 0** (three rows, all born ✅ CLOSED); registry guard OK (1047 src anchors
resolve); line-endings OK.
⚠ **An instrument bug worth carrying:** the WSL sync used `rsync --exclude 'build*'`, which is
UNANCHORED and therefore matched `src/program/build_scripts.cpp` — the mirror failed to configure with
*"Cannot find source file"*, which reads like a missing-file bug in the tree and is not one. Anchor
excludes with a leading slash (`/build*`).

### 0.003 ✅ RESOLVED 2026-08-17 — `--scanstatus` IS ON. IT WAS NOT A FORK.
I framed this as a three-way operator decision and was wrong to: one measurement collapsed it.
✔MEASURED at HEAD, the verbatim upstream construct (`__inline__ sqlite_uint64 sqlite3Hwtime(void)`
containing `__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi))`, plus a caller) **compiles at
`--config=release` on BOTH `pe64-x86_64` and `elf64-x86_64`, and the pe64 artifact runs, exit 42**;
at `debug` it fails `K_SymbolUndefined`, which is the conformant answer.
★ **The fact that dissolved the fork: `build-and-test.sh` sets `DSS_CONFIG="${DSS_CONFIG:-release}"`
and its own comment calls that load-bearing rather than a speed choice** — so the configuration this
harness actually exercises is the one in which `hwtime.h` links. There was no trade-off to bring;
asking would have billed the operator for a question a two-minute probe answers.
⇒ `--scanstatus` added to `configureFlags`, `SQLITE_ENABLE_STMT_SCANSTATUS` added to
`requiredDefines` (the fail-loud link that proves the define reached the compiler), and the stale
`$scanstatusComment` rewritten with the original kept as the record. `harness/test_sqlite_harness_legs`
passes with the new declaration. ⚠ **NOT YET PROVEN BY A FULL LEG RUN** — that is the cycle's sqlite
re-probe and it is still owed; until it runs, this is "declared and self-tested", not "green".
⚠ No `capabilityWitnesses` row was added and the omission is deliberate: a witness needs exactly one
decisive gate in its preamble, vetting that needs the upstream tree (cloned at run time, not
vendored), and an unvetted witness is how `mem5` and `analyze3` went red for reasons their capability
could not fix.

<details><summary>The 2026-08-17 brief that framed it as a decision, kept as the record</summary>

#### ⚠ A LOOSE END P5b LEFT, SURFACED NOT SILENTLY DECIDED — `--scanstatus` IS STILL OFF
`real-examples/c/sqlite/legs.json`'s `$scanstatusComment` instructs, in its own words, to *"re-add
the flag in the change that closes it"*. ✔MEASURED: **every blocker that comment names is now
closed** — `__volatile__`, the `:` operand lists, the asm TEXT, and (P5b) the `__inline__`-only
external-definition gap, after which `hwtime.h` compiles and runs at release on pe64,
elf64-x86_64 and macho64-x86_64. **The flag was not re-added, and the comment still concludes it
stays off for the `__inline__` reason it no longer has.**
⚠ **WHY THIS IS A DECISION AND NOT A CHORE:** `hwtime.h` still fails at **debug**, and that failure
is CONFORMANT — gcc 13.3.0, clang 18.1.3 and clang 19 all fail to link a called inline definition at
`-O0` and succeed from `-O1`. So re-adding `--scanstatus` buys sqlite's own `build(Default)`
configuration and its two corpus files, at the cost of a debug leg that goes red for a reason the
reference compilers share. ⇒ **Decide it explicitly (re-add release-only / re-add and accept the
debug red / keep off and correct the stale comment) — do not let it drift by inheriting a paragraph
that is now false.** 🧠 Not attempted this cycle: it is scope the operator has not asked for, and
guessing the answer would bake a policy into a config comment.

</details>

**STILL OWED on this cycle:** re-run both gate legs, commit + push.
🧠 **sqlite re-probe judged not proportionate here and the reasoning is stated rather than assumed:**
the whole diff touches the inline-asm lowering path plus one test, and `--scanstatus` is OFF, so no
sqlite TU in this configuration contains an `__asm__` at all. The in-suite
`harness/test_sqlite_harness_legs` runs as part of the 874. ⚠ If `--scanstatus` is ever re-enabled
(§0.003) that reasoning expires immediately — the asm path becomes live in sqlite and a full re-probe
becomes mandatory.
📄 PRs **#50, #51 and #52 are all MERGED**; this branch was cut clean from main. ⚠ The public-repo bot
rebases/squash-merges, so `4969e9e2` / `e5b60f6c` / `e42ae5a5` (the asm cycles) are **NOT ancestors of
HEAD** — their *content* is in main, their SHAs are not reachable. Do not `git show` them and conclude
the work is missing.

---

## 0. ★★★ P5 IS DELIVERED — GNU EXTENDED INLINE ASM COMPILES, LINKS AND RUNS

**✔MEASURED 2026-08-14/15.** `examples/c-subset/c_inline_asm_extended` exits **42** on **pe64 native,
WSL elf64-x86_64 and qemu-aarch64**, in **debug AND `--config=release`**, registered in **BOTH**
examples runners (in-process `tests/examples/` + CLI-subprocess `integrated_tests/`).
★★★ **The negative miscompile pin DISCRIMINATES:** eight values from `dssOp()` **CALLS** held live
across an `__asm__` clobbering `x21`–`x28`; delete the clobber list, rest byte-identical ⇒ **release
exits 1 instead of 42.** ⚠ Debug does NOT discriminate (locals are memory-resident pre-mem2reg) —
**the `release` arm is load-bearing, not decorative.**
✔ Conformance census genuinely AGREES with all four gnu oracles; `@acknowledged-gap` deleted.
⚠⚠ **AND EARLIER IN THE SAME CYCLE THAT CENSUS READ A SILENT MISCOMPILE AS CONFORMANCE** — with
capture landed but HIR→MIR still emitting a barrier, a clobber-bearing `__asm__` compiled rc=0 clean
and `objdump` showed it emitting **ZERO instructions**. *An oracle that only checks accept/reject
cannot tell "works" from "does nothing".*

**Anchors CLOSED this cycle (9):** `D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED` ·
`D-CSUBSET-INLINE-ASM-OPERANDS` · `D-CSUBSET-INLINE-ASM-GOTO` · `D-CSUBSET-INLINE-ASM-TEXT` ·
`D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED` · `D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE` ·
`D-ASM-ARM64-SETCC-W-FORM-UNDECLARED` · `D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE` ·
`D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED` (a **FALSE CLOSE** corrected — it had been witnessed
only on `eq`/`ne`, the two spellings where the substrate and gas vocabularies coincide).

### 0.00 ✅ OPERATOR-QUEUED CYCLE 2026-08-15 — BOTH TASKS DONE 2026-08-17 (see §0.001). Brief kept below.
⚠ **Sequencing, measured (now historical — the lane landed):** the in-flight lane owns `src/mir/lowering/hir_to_mir.cpp`, `src/mir/mir_asm_descriptor.{hpp,cpp}` and `tests/mir/**`, **and it is the lane ADDING the two fields task 2 pins**. Task 1 collides outright; task 2's subject does not exist until it lands. **Dispatch both only after that lane reports.**

**TASK 1 — route asm output store-back through `emitScalarStore` (a REAL silent defect).**
`Lowerer::lowerInlineAsm` stores each inline-asm OUTPUT back through its lvalue address with a bare
`mir.addInst(MirOpcode::Store, st)` at **TWO** sites — the `asm goto` successor-head path
(`std::array<MirInstId, 2> st{rp, outAddrs[k]};`) and the non-terminator path
(`std::array<MirInstId, 2> st{pieceVals[k], outAddrs[k]};`). Both **bypass `Lowerer::emitScalarStore`**
(same file, ~`:594`), the documented funnel that (a) routes an `_Atomic`-qualified lvalue to
`MirOpcode::AtomicStore` with `kAtomicOrderSeqCst` and (b) stamps the c21 `MirInstFlags::Volatile`
from `volatileFlagFor(node) | volatileFlagForType(accessedTy)`.
⇒ **Two silent consequences:** `volatile int x; __asm__("…" : "=r"(x));` loses the Volatile flag on
the write-back, so the optimizer may elide or reorder a store the source marked volatile; and
`_Atomic int x; …` emits a **plain Store on an atomic object**. ★ `emitScalarLoad`'s own docblock says
a missed funnel site is caught **LOUD** by the MIR verifier's atomic belt (`I_AtomicAccessNotLowered`),
so this is **likely a LIVE verifier failure rather than only a quality gap — confirm which by writing
the fixture FIRST.**
⚠ **The two halves of one operand are already asymmetric:** the READ half of a `"+r"` operand (added
2026-08-15) already goes through `emitScalarLoad`.
**Fix:** call `emitScalarStore(st, pieceTypeFor(k), kids[k])` at both sites instead of `addInst`.
⚠ **Keep the ordering** — the non-terminator path emits its stores in a SECOND pass, deliberately
after the producer→`ReturnPiece` adjacency window closes.
**Tests** in `tests/mir/test_mir_lowering_c_subset.cpp` (its `lowerCSubset` harness now threads both
the target schema and `hir->inlineAsmPool`, so a descriptor-carrying `__asm__` reaches MIR): assert
the volatile fixture's store-back carries the Volatile flag and the `_Atomic` fixture lowers to
`AtomicStore`. Red-on-disable by reverting each call individually and re-running through `ctest`.

**TASK 2 — pin the two new `MirAsmDescriptor` fields through the MIR rebuild (test hardening).**
`MirAsmDescriptor` gained `bool isExtended` (the BASIC/EXTENDED surface, carried because `:::` with
every section empty is **unreconstructable** downstream) and `std::optional<std::uint32_t> tiedOutput`
on `MirAsmOperand` (set on the synthesized INPUT entry carrying a `"+r"` operand's read half, naming
the output it shares a location with). `tests/opt/test_inline_asm_rebuild_carriage.cpp` pins that the
descriptor survives the rebuild passes, but its `sentinelDescriptor()`/`expectSentinel()` pair sets and
checks templateText, isVolatile, outputs, inputs, clobbers and the two clobber flags — **not the two
new fields**.
ⓘ **Today this cannot drop anything**: every rebuild site passes `src.asmDescriptor(id)` **WHOLE by
value**, so new members ride along. The exposure is a **future refactor to a field-by-field copy** —
precisely the silent-drop class `mir_asm_descriptor.hpp`'s own docblock guards (*"A POOL INDEX IS NOT
SELF-CARRYING, AND THAT IS THE WHOLE HAZARD"*).
**Fix:** extend `sentinelDescriptor()` to set `isExtended = true` and give one input a `tiedOutput`;
extend `expectSentinel()` to assert both. **Red-on-disable by making ONE rebuild site copy the
descriptor field-by-field, omitting the two new fields**, confirming the pin reds through `ctest`, then
reverting.

### 0.0 ✘ STALE — ALL FOUR OF THESE CLOSED IN CYCLE P5b (`4095c13b`). KEPT ONLY AS THE RECORD.
⛔ **Do not plan off this list.** ✔MEASURED 2026-08-17 that items 1–3 are done and item 4 shipped
(`hwtime.h` compiles and runs at release). The **current** open asm set is:
`D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER` — **narrowed to the LABEL half only**; `%N`, `%%` and
aarch64 outputs all measured working, `%l[name]` still fail-loud refused ·
`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE` (`"+r"` carriage lands, `bindAsmOperand` still refuses it) ·
`D-CSUBSET-INLINE-ASM-POSITIONAL-LABEL-REF-ACCEPTED-WITH-NO-GRAMMAR` ·
`D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER` ·
`D-ASM-TEMPLATE-DIAGNOSTICS-RENDER-WITHOUT-SOURCE-CONTEXT` · `D-CSUBSET-INLINE-ASM-SPELLING` (bare
`asm`) · `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` ·
`D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` · `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED` ·
`D-ASM-ARM64-GAS-SURFACE-INCOMPLETE` · `D-TEST-INTEGRATED-RUNNER-HAS-NO-OPTIMIZATION-ARM-CONCEPT`
(BLOCKING trigger). ⚠ Still trigger-gated and **meant to stay open**:
`D-ASM-TARGET-DECLARES-NO-BYTE-ORDER`, `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED`,
`D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED`.

<details><summary>The 2026-08-15 mandate, kept as the record</summary>

#### ⚠ THE NEXT CYCLE'S MANDATE — four blockers, operator-scheduled 2026-08-15
Balance ends **+6 by an explicit §B decision** (*"close everything already closed, commit, then
another cycle for the new blockers — I WANT ASM FULLY 100% DELIVERED"*). Not drift: this cycle
FOUND more than it fixed because it was a deep investigation. **Close these next:**
1. ★ **`D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER`** (HIGH) — the big one. No dialect declares
   `%N`/`%[name]`/`%l[label]`, nor a shape for the declared `PercentEscape`. Blocks `%%eax`, EVERY
   aarch64 asm OUTPUT (so `mrs %0, cntvct_el0` is unspellable), and `asm goto` WITH labels.
   ⚠ **NOT a sibling-alt addition** — `detectAmbiguousAlternatives` refuses two alts sharing a FIRST
   token and `%` is already `RegisterSigil`/`TypeSigil` (EXERCISED, two tests watch it refuse). Use
   the shipped `lexerModeTokens` per-mode override + an `assembly.templateLexerMode` key.
2. **`D-LIR-EARLYCLOBBER-FLAG-UNSETTABLE-AFTER-EMISSION`** (HIGH) — **one method**: a
   `LirBuilder::setInstFlags` sibling to `setInstRegConstraints`. Both ends already exist.
3. **`D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE`** — `"+r"`; replace the `requires2Address` bool with an
   operand INDEX at four literal-`0` sites. Core fix; every two-address target benefits.
4. **`D-CSUBSET-INLINE-FUNCTION-NO-EXTERNAL-DEFINITION-EMITTED`** — what ACTUALLY blocks sqlite
   `hwtime.h`. ⚠ **NOT an asm defect**: the asm half is byte-proven (`0f 31 rdtsc`) and builds on all
   five legs. gcc **`-O0` fails identically**, so DSS's debug behaviour is CONFORMANT and the residue
   is our release inliner. Sole blocker on re-enabling `--scanstatus`.
Also open: `D-LIR-VERIFY-VREG-CLASS-RULE-ASSUMES-A-ONE-TO-ONE-LIR-TO-MIR-MAP` and
`D-LIR-PER-INSTRUCTION-OUTPUTS-NOT-ENFORCED-SUBSET-OF-CLOBBERED`; plus trigger-gated
`D-ASM-SYSTEM-REGISTER-AS-ENCODED-DATA-UNMODELLED` which is MEANT to stay open.

</details>

<details><summary>§0 as written mid-cycle, kept as the record</summary>

## 0.9 ⚠⚠ (SUPERSEDED) A CYCLE IS IN FLIGHT AND NOTHING BELOW §1 IS COMMITTED

**Cycle: inline-asm P5 — embedded `__asm__` in C.** Started 2026-08-14, **UNCOMMITTED**.
✔MEASURED 2026-08-14 (second session, after a context exhaustion): **41 modified + 4 untracked**
files, **+4722 / −238**. **The tree BUILDS: `cmake --build build-dbg` rc=0, 520 steps.**
If you are picking this up cold: the work is real but unlanded. **Run the baseline yourself.**

**Baseline** ✔MEASURED at `d4c2836b` before any edit: build-dbg 592 steps rc=0 · ctest
**860/860** · anchors **982 OPEN** (registry 657 + plans 325).
**Re-measured mid-flight on the dirty tree:** ctest **862 / 863**, rc **8**. Sole failure =
`anchor_registry_guard`, EXPECTED (see "OWED" below). Anchor balance still **982 → 982, net 0**.
⚠ **Never pipe `scripts/run-gate/run-gate.sh` into `tail`** — the harness then reports the PIPE's rc. It said
"exit code 0" over that rc=8 run. The script logged `rc : 8` correctly; the pipe hid it. Read the log.

### 0.1 ★★★ OPERATOR RULINGS TAKEN 2026-08-14 — full text in [plan 29](29-inline-asm-plan%20-%20tbd.md) §4.4/§4.5/§4.6. DO NOT RE-LITIGATE.
1. **Asm outputs are SSA values carried as pieces — REUSING `ReturnPiece` ITSELF. ZERO new
   value-producing opcodes.** ⛔ No `AsmOutputPiece`. `InlineAsm` takes `Call`'s ROW SHAPE — but
   ✔MEASURED `{0,N}`, **not** Call's `{1,N}` (Call's minimum operand is the callee; an asm block may
   have zero inputs). ⛔ Rejected: outputs as memory operands — it makes the C local address-taken,
   degrading SROA and the shipped LICM on exactly the hot paths asm exists for.
   ★ THE ONE-FACT TEST: two opcodes would encode ONE fact, so every consumer carries both arms
   forever and the next multi-result producer mints a third. **A CONSTRUCT-private verb breaks
   agnosticism exactly as a LANGUAGE-private one does.**
2. **Promote "where do my result pieces live" to a PRODUCER-DECLARED property** (Call → the cc's
   return regs, unchanged; InlineAsm → the constraint-bound regs). Key on DERIVABILITY, never on
   producer identity. ★ **HARD GATE: with the property landed and no asm in the source, x86_64 +
   arm64 + pe64 output must be BYTE-IDENTICAL. If not, STOP — do NOT re-baseline goldens.**
3. **Rename `ReturnPiece`/`ret_piece` → `ResultPiece`/`result_piece`, ONE mechanical commit** kept
   separate from the semantic work. Reaches the shipped target JSONs + `lir_text` goldens; the churn
   is at its global minimum before the P5 corpus exists and rises monotonically after.
4. **`asm goto` WITH outputs LANDS THIS CYCLE.** ⚠⚠ The operator ruled this AGAINST their own
   written refusal argument — I asked because the brief's §6 said "does NOT land" while the button
   said "build it". **The button stands.** ⇒ build the edge-placement rule: pieces at the head of
   every successor block, splitting critical edges. [[D-CSUBSET-INLINE-ASM-GOTO]] **CLOSES**; no
   sibling row. (The refusal argument is preserved in plan 29 §4.5 so a later cycle does not "fix"
   this back — its premise is real: `lir_callconv.cpp:2255`/`:2522-2529` require a piece to
   IMMEDIATELY follow its producer and a terminator has nothing after it.)
5. **`%N` binding is STRUCTURAL** — a dialect-declared placeholder resolving to the operand's VREG at
   expansion. ⇒ **no post-regalloc binding pass is needed at all** (an earlier phrasing said
   otherwise and was wrong). ⛔ Rejected: rendering operand text and re-lexing — the renderer would
   restate sigils the dialect token table already declares for parsing.

### 0.2 ★★★ THE §8 CONTINGENCY FIRED — `ReturnPiece`'s payload carries TWO facts, only ONE stored
The ruling pre-authorised the fix, so this is NOT a new fork. ✔MEASURED: `addReturnPiece(call,
ordinal, pieceType, …)` (`mir.hpp:534`) takes **no class parameter**; `hir_to_mir.cpp:7604-7614` runs
separate `gprRet`/`fprRet` counters, picks the ordinal from the piece's class, then **DISCARDS the
class**, re-encoding it as a TYPE (`:7272-7287`) — its own comment (`:7271`) says *"The piece's
register CLASS follows from the type."* Consumption is `returnReg(schema, cc, rpRes.regClass(),
payload, …)` where **the class selects the register POOL and the ordinal indexes it**
(`lir_callconv.cpp:1520`). ⇒ `"=x"` on an integer would index the **wrong pool, SILENTLY**, GPR being
the else-branch default. **Split the payload so the class is carried explicitly. Core fix, no new opcode.**

⚠ **LATENT SHIPPED BUG found in the same read — fix it, do not walk past it:** `cc.returnVrs` exists
(`target_schema.hpp:558`) but `returnReg` **never reads it** — `lir_callconv.cpp:1520` is a two-way
`(cls == FPR) ? returnFprs : returnGprs`, so **a VR-class piece silently takes the GPR branch.**

### 0.3 ✔MEASURED — the four constraint forms vs the core (full table: plan 29 §4.4.4)
| form | verdict |
|---|---|
| `"=r"` / `"=x"` | ✅ **EXISTS, no core change.** `newVReg(LirRegClass)` (`lir.hpp:216`) is the ONLY creation API and **40+ shipped sites already pass a class independent of the MIR type**. ⚠ Tripwire: `checkVregClassMatchesMirType` (`lir_verifier.cpp:397-436`) needs a 4th skip arm for `"=x"` — and it matters NOW because this cycle wires the verifiers into production. |
| `"+r"` | 🟠 **CORE GAP.** `requires2Address` is a per-opcode **bool** hardwired to *result == operand[0]*, `0` a literal at FOUR sites (`lir_2addr_legalize.cpp:133-149`, `:151-156`, `:179-185`, `lir_regalloc.cpp:1026-1039`). Only ONE result slot exists (`lir_node.hpp:340`). ⇒ replace the bool with an operand INDEX defaulting to 0; every two-address target benefits. |
| `"=&r"` | 🔴 **ABSENT — nothing exists** in `src/` or either shipped target. ★ Carrier settled by measurement: `LirInst::flags` is a `uint8_t` with only `0x01/0x02/0x04` used ⇒ **5 free bits** — and `flags` is threaded through **all 8** rebuild sites, so it survives rebuilds BY CONSTRUCTION where the `_pad2` handle explicitly does not. |
| `"=m"` | 🟠 **PARTIAL.** Memory as an OPERAND is fully modelled; memory as a **RESULT cannot be expressed** (`LirInst::result` is typed `LirReg`). ⇒ an `"=m"` output must lower to the store-class shape (`result: none` + operand list). |

### 0.4 ✅ ALL THREE KILLED LANES WERE RESUMED AND COMPLETED (2026-08-14, session 2)
| lane | outcome |
|---|---|
| **Lane 1 — LIR wiring** | ✅ DONE — §0.4.1. Found **2 shipped defects** (`verifyLir`'s false rule; the 2addr abort). |
| **Lane G — config/grammar** | ✅ DONE — 46 + 22 tests green. **Settled the blocking gcc/clang measurement (§0.4.2)** and found `D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE`. |
| **Lane T — template→LIR** | ✅ DONE — **715/715** on the affected surface (`asm/ lir/ mir/ hir/ program/ link/ conformance/ examples/`). `examples/asm` **12/12 before AND after** the extraction. **CLOSED the zero-operand blocker** as "SHAPE 0". |
| **Lane V — diagnostics** | ✅ DONE — `0xE065`..`0xE06B`, all unsuppressable. |
| **Lane S — positional selectors** | 🟠 IN FLIGHT — spec is [plan 29 §4.7](29-inline-asm-plan%20-%20tbd.md). |

#### 0.4.1b ✔MEASURED — FULL-SUITE STATE AFTER LANES 1/G/T/V
**863 / 864.** Affected surface (`asm/ lir/ mir/ hir/ program/ link/ conformance/ examples/`)
**715/715**; complement (`lsp/ analysis/ core/ harness/` + shuffled + guards) **148/149**.
`examples/asm` **12/12 before AND after** Lane T's extraction, fixtures unchanged.
**The single red is `core/test_unsuppressable_codes` → `EveryMemberHasAnEmitSiteOrIsMarkedRetired`**
— Lane V's seven `S_InlineAsm*` codes (`0xE065`..`0xE06B`) are in the closed table with no emit site
in `src/` yet. ✔Attribution proven, not assumed: all seven are ABSENT at HEAD, and the asm lane's
four files contain **zero** `S_InlineAsm*` references. **It goes green when the front-end lane lands
its emit sites IN THE SAME COMMIT** — the ordinary mid-flight shape, not a defect.

⚠ **COORDINATION POINT — two of the seven overlap the engine tier and MUST NOT DRIFT:**
`S_InlineAsmPlaceholderOutOfRange` (0xE06A) is the **semantic-tier twin** of the engine's
`AnUnboundOperandIsRefusedNamingTheBoundSet`. The front end will validate `%3` against the operand
list before the template ever reaches `lowerAsmTemplateToLirRun`, so **the engine refusal is a SECOND
line of defence, not the primary one** — correct, and worth keeping, but the two key on *different
facts*: the engine's on *"the caller bound no such spelling"*, the front end's on *the index*. Keep
both; do not let them drift into disagreeing about what "out of range" means.
`S_InlineAsmPlaceholderInBasicTemplate` (0xE06B) confirms the basic-vs-extended `%` discriminator is
owned at the **front-end/lexer** tier — which is where §0.4.2's measurement says it belongs, and why
the engine correctly needs no assumption either way.

#### 0.4.2 ★★★ THE §2a TABLE WAS WRONG — SETTLED, AND IT CHANGES THE MANDATORY PIN
✔RE-MEASURED twice, base64-fed sources, `gcc 13.3.0` + `clang 18.1.3` (unsuffixed `clang` ABSENT):
a bare `%eax` in an **EXTENDED** template is an **ERROR on BOTH** (gcc: *"operand number missing
after %-letter"*; clang: *"invalid % escape"*). **Any colon makes it extended.** `%%eax` works.
⇒ in an extended template `%`+letter is a **MODIFIER, not a sigil**, so the template surface and the
`.s` surface **genuinely differ lexically**. ⚠⚠ **THE NEGATIVE MISCOMPILE PIN MUST BE WRITTEN
`%%eax`** — written the §2a way it would not compile under the reference compilers at all.

#### 0.4.3 ⚠ TWO CORRECTIONS TO ORCHESTRATOR BRIEFS — both were MY error, recorded so they are not repeated
- **A token-declaration-ORDER pin cannot exist.** ✔MEASURED: `longestMatch` probes down from the
  longest declared lexeme, so 2-byte `%%` beats `%` **by LENGTH in any row order**; and
  `nlohmann::json`'s default object is a `std::map`, so **row order does not survive the parse**.
  The requested pin would have asserted **nothing**. What shipped pins the only mutation that CAN
  change the outcome — **deleting the `%%` row** — and fails if there is no row to erase.
- **The placeholder CANNOT be a sibling alt.** `%` is already `RegisterSigil`/`TypeSigil` and
  `detectAmbiguousAlternatives` refuses two sibling alts sharing a FIRST token — **exercised**, not
  read (two in-process tests watch the loader refuse it). ⇒ use the **shipped `lexerModeTokens`**
  per-mode override + an `assembly.templateLexerMode` key. Reuse, not invention.

#### 0.4.4 ⚠ FIVE STALE LINE-NUMBER CITATIONS created by Lane T's extraction — FIX BEFORE COMMIT
Three point **past EOF**. `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` `:1616`→`asm_text_to_lir.cpp:1224` ·
`D-ASM-INTERIOR-LABELS-NOT-ADDRESSABLE-AT-AN-OFFSET` `:2827`→`asm_template_to_lir.cpp:1529` ·
`D-ASM-DATA-SYMBOL-ABSENT-FROM-SYMTAB` `:272`→`asm_text_to_lir.cpp:50` ·
`D-ASM-ZERO-OPERAND-PLAIN-INSTRUCTION-UNLOWERABLE` `:2580`→`asm_template_to_lir.cpp:1298` **(and that
row is now CLOSEABLE)** · `_handoff.md` `:3360`→`asm_text_to_lir.cpp:1561`.

### 0.4.0 (historical) the three lanes as they stood when the first session died
Resume by re-reading each file, NOT by assuming the lane finished. Their transcripts are on disk.
| lane | owned paths | observed state |
|---|---|---|
| **Lane 1 — LIR wiring** | as below | ✅ **DONE** — see §0.4.1 |
| **Lane G — config/grammar** | `asm.lang.json`, both dialects, `assembly_config.hpp`, `grammar_schema_json.cpp`, `semantic_config.hpp`, `tests/core/test_language_references.cpp`, `tests/asm/test_asm_shipped_dialects.cpp` | all three `.lang.json` MODIFIED; added `operandRule`/`memoryClobber`/`conditionCodeClobber` as **REQUIRED** members of `semantics.inlineAsm` ⇒ **this RED is Lane G's own and it was mid-fix:** `LanguageReferenceRefusals.InlineAsmGateBaseLoadsClean` (`test_language_references.cpp:949`) — the in-test host fixture at ~`:936` still declares a now-PARTIAL `inlineAsm` object, which the all-or-nothing loader correctly refuses |
| **Lane T — template→LIR** | `src/asm/asm_{text,template}_to_lir.*`, `src/asm/CMakeLists.txt`, `tests/asm/test_asm_template_to_lir.cpp` | `asm_template_to_lir.hpp` CREATED (untracked), `asm_text_to_lir.cpp` MODIFIED, **`.cpp` NOT yet created**. Task: extract the per-instruction core (`emitInstruction` ~:1845, `decodeOperand` ~:1960, `buildLirInst` ~:2478, variant election ~:3015) behind a caller-supplied `LirBuilder` + operand-resolution callback. ★ MUST be behaviour-preserving; the 12 `examples/asm/` fixtures are the guard, green before AND after |

✅ **Lane V — diagnostics: DONE.** `0xE065`..`0xE06B` (`S0065`..`S006B`), all seven unsuppressable,
array 148→155. Values measured free two independent ways; band is gapless with **four** retired-but-
reserved values (`0xE015`, `0xE04E`, `0xE04F`, `0xE052`) that must never be reused. Red-on-disable
proven with the mutant shown to have been read (binary mtime).
⚠ **Its ONE red is a cross-lane ORDERING dependency, not a defect:**
`UnsuppressableCodes.EveryMemberHasAnEmitSiteOrIsMarkedRetired` fails with exactly those seven codes
because nothing emits them yet — **it goes green when the consumer lanes land in the SAME commit.**
Fallback if they slip: the shipped `D_DependencyGit*` precedent (drop the rows, restore 148, flip the
pin to its negative form). ⚠ `0xE06B`'s "does not diverge from the reference toolchain" half is
**INFERRED** (it rests on gas rejecting the emitted `%0`) and is labelled as such in-code — measure it
before the consumer lands, or narrow/withdraw the code.

#### 0.4.1 ✅ LANE 1 COMPLETE — what landed, and TWO shipped defects it found
**Landed:** the per-instruction handle rides all four rebuild passes; `copyLiteralPool` shim
**deleted** (zero refs left in `src/`); `verifyLirRebuild` wired after each of the four passes
(`compile_pipeline.cpp:1021, 1049, 1061, 1109`) plus `verifyLirPostRegalloc` after rewrite and
callconv, **always on, no debug gate**; the XFAIL replaced by per-pass positive pins **plus three
ORDERING pins** (the handle landed on the *right* instruction — which no reference count can see).
Red-on-disable done in **two** mutation classes per pass (delete the carry; move it to the end of the
iteration — the realistic misplacement), mutant-read proven by **both the `.obj` AND the `.dll`
mtimes advancing**, the DLL being what the pin's process actually loads.
**Cost ✔MEASURED: +3.3%** compile time (1162 ms vs 1125 ms on a 135 KB input; spread ~2%, so ~2× noise).

★★ **THE SURPRISE, and it is the interesting part:** the four passes are NOT uniformly 1:1, and
callconv has a **third class** — ~11 arms *materialize* a virtual op into a **different** opcode
(`arg`→mov/frame_load, `alloca`/`va_*`→lea, `frame_*`→class-routed memory op, `ret_piece`→consumed),
and several have **no correspondent at all** (`maybeMov` emits **zero** instructions when regalloc
already picked the source register). ⇒ *"carry one handle per source instruction"* is not even
well-defined there. Those arms deliberately carry nothing **and that is fail-loud**: the pool is
copied unconditionally, so a dropped handle leaves an unreferenced entry and `verifyLirRebuild`
reports `L_SideStructureReferenceLost` **naming the pass**.
Per-pass correspondent: 2addr → the **second** (the operation); wide-call-args → the **last** (the
Call); rewrite → the **middle**; callconv → varies by arm.

⚠⚠ **DEFECT 1 — `verifyLir` is DELIBERATELY NOT WIRED, because the rule is FALSE about the LIR this
compiler builds.** Wiring it reds **~200 `examples/` tests**, all on Rule 1
(`checkMemOperandPairing`) and no other rule: the rule demands every `load`/`store`/`lea` end with
`MemBase`+`MemOffset`, but `mir_to_lir.cpp:3809` emits `load result, [SymbolRef]` for a global (and
`lea result, [SymbolRef]`) — a legitimate symbol-addressed mode with **no base/offset pair**.
★ It survived because **it had only ever run on hand-built test modules** — the same
"pin that never met its subject's real input" family this project keeps catching. `lir_verifier.cpp`
was not Lane 1's to edit. **Anchor + fix; until then `verifyLir` cannot run in production.**

⚠⚠ **DEFECT 2 — `legalizeTwoAddress` ignores `emitTerminator`'s failure**: it never sets
`allFunctionsLegalized = false`, so the block is left unterminated and `finish()` **aborts the
process** instead of failing loud. Same *"a refusal that crashes is not a refusal"* class as
`D-LIR-TEXT-PARSE-UNSEALED-BLOCK-ABORT`. **Anchor + fix.**

★ **No collision with the queued Lane R:** Lane 1 touched `lir_rewrite.cpp` lines **674–687,
696–698, 700–702, 946–951** only; the `implicitForbidden` region at **541–554 is byte-identical to
HEAD** with unchanged line numbers.
✔ `ctest` in `build-lane-lir`: **860/863** (the 3 reds being the guard + the two concurrent lanes'
in-flight state). `build-dbg` never built into.

#### 0.4.5 ✅ LANE S (positional operand selectors) COMPLETE — plan 29 §4.7 shipped
17 new tests, `asm` suite 38/38. **One key minted (`operandSelectors`) + one struct; no new role,
verb, kind or diagnostic code.** Six red-on-disable mutants, each proven read by subject-binary mtime,
all through `ctest`. ★ Two of them mattered more than expected: mutant **B** was **too coarse** — two
tests stayed green because it made all twelve `cset` rows match, tripping the double-match guard
instead; the lane **refused that as evidence** and added finer mutants B2/C that actually
discriminate. And under mutant A the pin's own anti-vacuous `ASSERT_TRUE(row.contains(...))` fired,
**reporting itself BROKEN rather than passing vacuously** — the guard-on-the-guard working.
✔ Load-time ambiguity refusal (§4.7.1) **EXERCISED**, not read, with the complement pinned (twelve
`cset` rows sharing one spelling load clean) so it cannot degenerate into "all duplicates refused".
✔ Witnessed by EXECUTION: `aarch64-linux-gnu-objdump` reads DSS's own ELF back as
`mrs x0, cntvct_el0` / `cset x0, ls`, and it runs under qemu-aarch64 **exit 0, debug AND release**,
negative control exit 255.
⚠ **The cross-front-end pin's C half is NOT reachable — measured by EXERCISING it**, not read:
`__asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r"(v))` → `error[S0062]`, and `cntvct` has **one**
`src/` hit, a comment. What landed instead asserts the `.s` walker and `lowerAsmTemplateToLirRun`
elect the SAME opcode row and emit byte-identical output. **P5's exit criterion still needs the C
gate opened by the front-end lane.**
★★★ **AND IT CAUGHT A FALSE CLOSE — the highest-value find of the cycle. See §0.4.6.**

#### 0.4.6 ★★★ A SHIPPED `✅ CLOSED` ANCHOR WAS FALSE, AND THE FAILURE MODE IS GENERAL
[[D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED]] was marked **✅ CLOSED 2026-08-13**. ✔MEASURED
2026-08-14: `condCodeOfOperand` resolves a bare-name operand against `kTargetCondCodeTable`, which is
keyed on **SUBSTRATE** names (`slt`, `sle`, `ult`…) while gas writes `lt`, `le`, `lo`, `ls`, `hi`,
`hs`, `cc`, `cs`. It is correct on exactly **`eq` and `ne` — which are the two spellings the row cites
as its evidence** — and WRONG IN BOTH DIRECTIONS on the other ten (refuses `cset x0, lt` which gas
accepts; accepts `cset x0, slt` which gas rejects). ⇒ the close covered the ENGINE half on its two
easy cases; the DIALECT half stayed open and the ten hard spellings were never exercised.
★★★ **THE TRANSFERABLE LESSON, now in the row: A CLOSE WHOSE WITNESS SET IS THE SUBSET WHERE TWO
VOCABULARIES AGREE HAS TESTED THE COINCIDENCE, NOT THE MAPPING. Pick witnesses where they DISAGREE.**
The row is corrected in place — re-closed by the twelve selector rows, each byte-pinned to gas's
measured word, with the superseded claim kept in a `<details>` block rather than deleted.

#### 0.4.7 ⚠ A LANE CORRECTLY REFUSED AN OPERATOR INSTRUCTION — recorded so it is not "fixed" back
The ruling said: if gas is case-insensitive, the selector match must be too. ✔MEASURED: gas **is**
fully case-insensitive — but DSS's mnemonic match is exact and **already** breaks `MOV X0, X1`
dialect-wide. A case-insensitive selector beside a case-sensitive mnemonic leaves one half of a row
loose and the other strict, and `MRS X0, CNTVCT_EL0` would **still** fail at the mnemonic ⇒ it buys
nothing and **hides the real gap behind a half-fix.** The lane anchored the true, dialect-wide gap
instead: [[D-ASM-DIALECT-MNEMONIC-MATCH-IS-CASE-SENSITIVE]].

### 0.5 ⚠ STILL OWED BEFORE COMMIT
⚠⚠ **ANCHOR BALANCE IS `+8` AND THE GATE CORRECTLY FAILS.** ✔MEASURED 2026-08-14:
`anchor-registry: OK (1033 src anchors all resolve to plans)` but
`anchor-balance: FAIL — this cycle leaves 8 more row(s) OPEN than it found.`
**DO NOT COMMIT AND DO NOT WIDEN THE GATE.** The cycle has opened rows and closed one
(`D-ASM-ARM64-CONDITION-AS-OPERAND-UNMODELLED`, re-closed properly). The four big closures —
`D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED`, `D-CSUBSET-INLINE-ASM-OPERANDS`,
`D-CSUBSET-INLINE-ASM-GOTO`, and the embedded half of `D-CSUBSET-INLINE-ASM-TEXT` — all depend on the
front end, MIR carriage and the expansion, **none of which exist yet**. The balance goes negative
when they land; until then this gate failing is the truth, not an obstacle.
✅ **The `anchor_registry_guard` red is CLEARED** — ✔MEASURED 2026-08-14:
`anchor-registry: OK (1030 src anchors all resolve to plans)`, 3816 rows in 40 files, 0 cell-width
violations. Four rows were written: `D-LIR-PER-INST-REG-CONSTRAINTS` (the guard's actual cause),
`D-LIR-VERIFY-MEM-OPERAND-PAIRING-RULE-IS-FALSE`, `D-LIR-2ADDR-IGNORES-EMIT-TERMINATOR-FAILURE`, and
`D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED`.
⚠ **All four are OPEN, so the anchor-BALANCE gate is now +4 and WILL FAIL until the cycle's closures
land** (`D-LANG-GNU-EXTENDED-INLINE-ASM-UNSUPPORTED`, `D-CSUBSET-INLINE-ASM-OPERANDS`,
`D-CSUBSET-INLINE-ASM-GOTO`, the embedded half of `D-CSUBSET-INLINE-ASM-TEXT`). That is expected
mid-flight, not a defect — but **do not commit while it is positive**.

★★★ **NEW BLOCKER, and it is a §B the operator has NOT yet answered:
`D-ASM-ARM64-SYSTEM-REGISTER-AS-OPERAND-UNMODELLED` BLOCKS THE arm64 HALF OF P5's OWN EXIT
CRITERION.** The dialect lane correctly DECLINED to ship a `cntvct` row: gas spells it
`mrs <Xd>, cntvct_el0` — the system register is an **OPERAND** — while the target opcode is
**ZERO-operand** with the counter in the fixed word, so a `{"spelling":"mrs","opcodes":["cntvct"]}`
row would hand the lowering one leftover operand against `maxOperands: 0` and **every line using it
would fail loud**. ⚠ This NARROWS rather than contradicts the earlier refutation: that refutation is
still right about the ENCODING side (`cntvct` needs zero new slot vocabulary), but it never reached
the DIALECT side. P5's exit criterion is *"`hwtime.h` compiles"* and hwtime.h's arm64 arm **is** the
`mrs`. The two candidate shapes and a recommendation are in the row; **ask before building.**
- `D-TEST-SCHEMA-MUTATION-HELPER-FAILS-OPEN` — cited in `tests/test_support/CMakeLists.txt:42`, no
  row. (The guard scans `src/`+`examples/`+`real-examples/` only, so it does NOT fire — §A.7 still requires it.)
- Rows born ✅ CLOSED: the mutation-helper fail-open · the vacuous `VaListStrategyKeys.
  AKeyValidForTheDeclaredStrategyIsAccepted` pin · the `[[nodiscard]]` explicit discard at
  `asm_text_to_lir.cpp:3360` · **the `run-gate.sh`-piping trap above.**
- **NO ROW EXISTS** for the per-target **operand-modifier width-view facet** that `S0067` (`%w0`/`%k0`)
  refuses on — plan 29 never mentions it. Decide: build it, or a real trigger-gated row.
- The `returnVrs` blindness (§0.2) · the `"+r"` core gap (§0.3) · the `checkVregClassMatchesMirType`
  4th skip arm.
- **`--scanstatus` must be re-added to `real-examples/c/sqlite/legs.json`** (its `$scanstatusComment`
  at `:43` says so explicitly) **in the same change that closes the gap**; `requiredDefines` then
  proves it took. Buys 2 test files (`scanstatus`, `scanstatus2`).

### 0.6 REMAINING WORK, in dependency order
1. Finish Lanes 1 / G / T (above).
2. **Rename lane** (§0.1.3) — mechanical, blocked on Lane 1 (`lir_callconv.cpp`).
3. **Lane R — regalloc chokepoint + earlyclobber.** Blocked on Lane 1 (`lir_rewrite.cpp`). ★★ The
   `inputs ∪ clobbered` union is **hand-rolled at THREE sites** (`lir_regalloc.cpp:483`, `:1095`,
   `lir_rewrite.cpp:542`), ALL per-opcode-only ⇒ **regalloc cannot see the per-instruction pool at
   all**; carrying handles faithfully and then ignoring them is the exact silent miscompile P5 exists
   to prevent, and it passes any test that only checks the handle survived. §A.5 ⇒ ONE accessor
   (`effectiveForbiddenOrdinals`), three callers, and the closing test must exercise **each site
   individually** (one of them with a SPILLED operand — `lir_rewrite.cpp:538` names the miscompile it
   prevents: *"a spilled idiv DIVISOR reloads into rax … 121 not 160"*).
   ⚠ Earlyclobber: the fix recorded in an earlier handoff ("place the def at the FIRST expanded
   instruction") **DOES NOT WORK** — ✔MEASURED, for a single-instruction template the input's use at
   `earlyPos` and the def at `latePos` are already disjoint (`lir_liveness.cpp:143-144`, `:352`,
   `:360`; `expireActive` frees at `range.end <= currentStart`, `lir_regalloc.cpp:574-588`). The
   discriminating variable is **early slot vs late slot**, not which instruction. The multi-instruction
   case is already safe. **The test MUST be the single-instruction case with a matched plain-`"=r"`
   control demonstrating SHARING** — otherwise it passes on an allocator that never shares.
4. **Lane M — MIR carriage**: `InlineAsm` `{0,N}`, the `ResultPiece` payload split (§0.2), the
   producer-declared piece source (§0.1.2) — ✔MEASURED it needs a **CONSUMER** in `returnReg`, not a
   new carrier: `LirRegConstraintPool` already carries per-inst `outputNames`/`outputOrdinals` with
   zero consumers — `returnVrs` fix, `asm goto` CFG + edge placement + critical-edge splitting.
   ⚠ MIR is rebuilt by **three** live verbatim-copy sites that carry `instPayload` but re-add no side
   pool (`opt/passes/mir_rebuild_helper.cpp:428`, `inlining.cpp:617`, `inlining.cpp:1041`; LICM is
   excluded by construction at `licm.cpp:93`). ★ Structural fix, not a checklist: add the asm opcodes
   to `MirBuilder::addInst`'s dedicated-builder refusal list (`mir.cpp:637-647`) so a forgotten copy
   site **aborts loudly** instead of dropping the clobber list.
5. **Lane F — front end**: `InlineAsmFacts` captures **zero expression NodeIds** today
   (`semantic_analyzer.cpp:9498-9536`) — that is the hole. Gates at `:10950`/`:10990`/`:11013`+`:11028`/
   `:11042`. ⚠ **There is NO typed-view layer** — `docs/tree-model.md:124` records the 08.55 cleanup
   deleted it because role-position helpers drift silently; extend `gatherInlineAsmFacts`, do not mint
   a view. Locate the operand's value expression by **RuleId**, never by child position.
6. **Lane X — MIR→LIR expansion + corpus + goldens.** ★ Every register-pinned OUTPUT must ALSO enter
   the instruction's clobber set: `outputs ⊆ clobbered` is **loader-enforced only for the per-opcode
   path** (`target_schema_json.cpp:3235-3259`) and regalloc's forbidden set deliberately omits outputs
   (`lir_regalloc.cpp:452-466`) — the second is safe ONLY because of the first, and **the
   per-instruction path has no loader**. Attach the handle to **EVERY** instruction the block emits,
   not just the first (`collectImplicitClobberPositions` records one forbidden position per instruction).
   ⚠ **Test-design trap:** `__asm__("nop")` "compiles AND RUNS" is VACUOUS — a `nop` changes nothing
   observable, so an expander emitting ZERO instructions passes it, and no cleverer basic template
   fixes it (basic asm's register effects are invisible to the compiler by design). ⇒ the witness must
   be TWO-PART: a byte/structural pin that the instruction was emitted, PLUS the runtime arm.
   ⇒ **P5 cannot land without its negative miscompile pin**: a value obtained from a **CALL** (not
   computed inline — both reference compilers schedule around an inline computation and every arm
   looks identical), held live across an asm block that clobbers its register, asserted to survive.
   With a `{"shippedPipeline": "release"}` arm.

</details>

---

## 1. WHERE WE ARE

### The cycle in flight: AP6 — `dependsOn` resolution
Plan-06 §5.1 **B.1–B.12** are the decisions of record. **B.10 amends U-2** (consumer-driven
derivation) and **B.11 carries the design-audit rulings M2–M8 + the U-8 correction** — read both
before touching anything dependency-shaped.

✔**MEASURED baseline of the MERGED tree** (Windows MSVC-Debug, `build/`, 2026-08-14): build **rc=0,
ZERO warnings tree-wide**; full ctest **859/861** at the merge point, with both reds diagnosed and FIXED (§1.3). The pre-cycle baseline at `d4c2836` was 860/860; the gate after the resolver, the two corpus examples and the CR fix was 864/864 (§1.2).
✅ **CURRENT GATE: 866/866 ON ALL FOUR LEGS, 0 failed.** ✔MEASURED 2026-08-16 at `ea47e69`, each leg
synced **by `git fetch` + `reset --hard`, NOT by rsync** — the previous cycle's CRLF confound came
from rsyncing the Windows working tree, and syncing by git removes that class of confound entirely
(every leg reported `DIRTY=0` at the pushed HEAD before building).

### Assembly — where the two halves actually stand
📄 The durable owner is [`.plans/29-inline-asm-plan - tbd.md`](29-inline-asm-plan%20-%20tbd.md).
⚠⚠ **`.temp/PLAN-inline-asm-arc-2026-08-12.md` IS SUPERSEDED AND ITS PHASE NUMBERS ARE RETIRED.**
Plan 29 deleted its `P3`/`P4` rows on 2026-08-12 and says why: *"Two rows for one phase is not
history, it is an ambiguity about what 'P4 is done' means."* Quoting the `.temp` numbering will
mis-size the work — it happened on 2026-08-14 and cost a scope correction.

- ✅ **`.s` as a real input language WORKS.** P1+P2, P2.5 and P4 are done: `asm.lang.json` + two
  dialects, 12 runnable `examples/asm/`, text→LIR→bytes, a `.s` executing on x86_64 **and** arm64.
- 🟠 **Inline asm as a C FEATURE is the open half, and it is P5** — not "P3+P4". Today only the
  **empty** template compiles (→ one `MirOpcode::CompilerBarrier`); `__asm__("nop")` is refused by
  `S_InlineAsmNonEmptyTemplate` (0xE057) and any operand/clobber/label by `S0062`.

### Instrument health — the standing warnings
- ⚠⚠ **TWO independent mechanisms make a red-on-disable report GREEN over a LIVE mutant** — one
  where the mutant was never COMPILED IN (`ninja -t deps` = `#deps 0`, **10 of 403** objects) and
  one where it was never READ (cwd-walk config resolution). In both, every fail-closed clause was
  satisfied. ⇒ treat any green red-on-disable from this tree as UNPROVEN unless the mutant was shown
  to have been **read**. A config-level red-on-disable **MUST** run through `ctest`, never a bare `.exe`.
- ✔ **A THIRD was found 2026-08-14 and is now FIXED**: `tests/test_support/mutate_target_schema.hpp`
  had three fail-open routes (an unmatched `removeMnemonics` entry, an undocumented no-`opcodes`
  early return, and `erase(remove_if…)` returning `end()` on no match). It is this project's primary
  anti-vacuity instrument and it could mutate **nothing** and report success. Now throws on any
  no-op, including a `doc.dump()` before/after byte comparison. ★ It immediately caught a **genuinely
  vacuous shipped pin** (`VaListStrategyKeys.AKeyValidForTheDeclaredStrategyIsAccepted` wrote two
  values the shipped layout already declared, so its "mutant" was byte-identical) — fixed.
- ✔ Anchor guard resolves **truncated** citations by substring: **91 line-wrapped `D-*` names across
  48 files** pass silently → `D-GATE-ANCHOR-GUARD-RESOLVES-TRUNCATED-CITATIONS-BY-PREFIX`.
- ✔ Registry **line-number** citations rot silently. A stale path fails loudly when grepped; a stale
  line number still resolves, to the wrong code.
- ✔ **Counts written from memory keep erring LOW.** Never re-quote a gate figure — re-measure at the
  commit that carries it.

---

## 2. ✔MEASURED 2026-08-14 — the reference-compiler spec for inline asm

📄 Per [[feedback_reference_compilers_are_the_spec]] these tables ARE the specification. gcc 13.3.0
(x86_64 native + `aarch64-linux-gnu-gcc` cross) and clang 18.1.3, `-O2 -S`. **Re-state the versions
beside any re-quote.**

### 2a. What a clobber list MEANS — and it is not what "extended vs basic" suggests
| arm | gcc 13.3 | clang 18.1.3 | value survives? |
|---|---|---|---|
| `__asm__("xorl %eax,%eax")` — basic | `call h` → asm → `ret` | same | **NO — destroyed** |
| `… ::: "eax"` — extended, real clobber | `movl %eax,%edx` → asm → `movl %edx,%eax` | `%ecx` | **YES** |
| `… :::` — extended, EMPTY clobber list | identical to basic | identical | **NO — destroyed** |

⇒ **(1)** emitting a basic template verbatim while assuming ZERO clobbers **is** the reference
semantics — the corruption is the programmer's, by design; refusing it, or conservatively spilling
around it, are both divergences and the second is silent. **(2)** protection must key off the parsed
clobber **LIST being non-empty**, never off "is this the extended form".
⚠ The first version of this probe was WEAK — it computed `y = x*3` before the block and both
compilers simply *scheduled the multiply after* the asm, so nothing was live across it and every arm
looked identical. The value must come from a **call**.

### 2b. What `%0` expands to
| C source | x86_64 | aarch64 |
|---|---|---|
| `"=r"` / `"r"` | `%eax` / `%edi` | `x0` |
| `"=a"` | `%eax`, and it genuinely **PINS** (3 competing `"r"` operands, still `%eax`) | — |
| `"m"` | `(%rdi)` | — |
| `"i"(7)` | `$7` | `7` |
| `"%w0"` | — | `w0` (the 32-bit VIEW — this is `subOf` from the template side) |
| real sqlite `hwtime.h` | `"=a","=d"` → `rdtsc` | `"=r"` → `mrs x0, cntvct_el0` |

⇒ ★★★ **RENDERING is DIALECT vocabulary; the letter→register MAPPING is TARGET vocabulary, and they
must not be merged.** x86 renders `%eax`/`$7`, arm64 renders `x0`/`7` — different sigils for the same
abstract operand, and the sigil is an AT&T-vs-Intel fact (one CPU, two dialects). Collapsing them
re-creates `D-CONFIG-ASM-DIALECT-DECLARED-AS-TARGET-VOCABULARY`, the facet built, reviewed and
reverted the same day. ⇒ the six minimum-viable letters split across **three existing axes**:
`r`→`TargetRegClass`, `a`/`d`→a specific `registers[].name`, `i`/`m`→`OperandKindFilter`.

### 2c. ★ Earlyclobber IS observable, and ignoring it is a silent miscompile
| | `"=r"` plain | `"=&r"` |
|---|---|---|
| x86_64, 1 input | `%0=%eax %1=%eax` — **SHARED** | `%eax` / `%edi` |
| aarch64, 1 input | `x0 x0` — **SHARED** | `x0 x1` |
| aarch64, 2 inputs | `x0 x0 x1` — **SHARED with input 0** | `x0 x2 x1` |

The discriminating shape is an input that is a local temporary **dying at the asm** — with the input
in the ABI arg register and the output in the return register the allocator has no reason to share
and the probe says nothing. ⇒ a template that writes `%0` before reading `%1` destroys its own input.
**Accept-and-ignore is not available**; implement `&` or refuse it loud.
✔ DSS's allocator shares by exactly this rule: a **use** is recorded at `pos`, a **def** at `pos+1`
(`lir_liveness.cpp:347-361`), so single-instruction input and result ranges do not overlap.
★ The fix is ~5 lines inside existing machinery: place the `&` output's def at the **first** expanded
instruction — `firstDef` is a min over defs (`lir_liveness.cpp:360`), so its range then covers every
input's and sharing becomes structurally impossible.

---

## 3. OPERATOR DECISIONS TAKEN 2026-08-14 — do not re-litigate

Full rationale in [`29-inline-asm-plan`](29-inline-asm-plan%20-%20tbd.md) §4.0 and §5.

1. ✅ **The clobber carrier: a per-INSTRUCTION pool that REUSES the `ImplicitRegisterConstraint`
   STRUCT**, indexed from `LirInst::_pad2`. ⛔ **NO new `LirOperandKind`** (appending operands breaks
   `operandsMatchGuard`'s positional kind equality and the verifier's last-two-operands invariant,
   and a clobber-only kind structurally cannot express a fixed-register INPUT `"a"(x)`).
   ⛔ **DO NOT touch `TargetOpcodeInfo::implicitRegisters`** — it is config-written; a second writer
   is *"one field, two writers"*. Operator, verbatim: *"Same type, two owners is fine; one field, two
   writers is not."*
2. ✅ **Extend the ONE shared copy helper, do not hand-roll per-pass copies.** ⚠⚠ **There are FOUR
   rebuild passes, not the three that both the plan AND the independent audit claimed** — ✔MEASURED:
   `lir_2addr_legalize.cpp:80`, `lir_callconv.cpp:3971`, `lir_rewrite.cpp:929` **and
   `lir_wide_call_args.cpp:220`**, all the same `lir_pass_util::copyLiteralPool(src, b)` call. The
   fourth is exactly the one a hand-rolled approach forgets.
3. ✅ **A FAIL-LOUD BACKSTOP is mandatory** — after each rebuild, a `_pad2` index outside the pool, or
   a pool whose entry count dropped, fails loud. **This also fixes the shipped literal pool, which has
   the identical exposure today**; operator: *"the precedent carries the bug"*, so inheriting it is not
   acceptable.
4. ✅ **`asm goto` with NO label section: FOLLOW CLANG — ACCEPT.** gcc 13.3 rejects, clang 18.1.3 and
   19.1.1 accept. ⇒ `D-CSUBSET-INLINE-ASM-GOTO`'s named blocker (*"operator decision"*) is
   **DISCHARGED**, so the CFG half lands in the deciding cycle.
5. ✅ **NO P5a/P5b SPLIT** — the proposed blocker (arm64 `mrs` needs a sysreg table + a 15-bit
   encoding slot) **did not survive measurement**; see §4.

---

## 4. THREE REFUTATIONS WORTH MORE THAN THE CODE — recorded so they are not re-derived

1. ★★★ **The arm64 `mrs` blocker is REFUTED.** All three legs fell: `cntvct_el0`'s absence from
   `registers[]` is irrelevant (`TPIDR_EL0` is also absent and DSS already encodes `MRS Xd,
   TPIDR_EL0`); the shipped **`tlsbase`** row encodes MRS as a **zero-operand fixed word**
   `0xD53BD040 | Rd`, its own `$comment` saying *"zero new slot vocabulary"*, and `cntvct_el0` is
   structurally identical (`0xD53BE000 | Rd`); and *"`hwEncodingOf` hard-caps fixed32 at 5 bits"* was
   MISSTATED — it caps at the **caller-supplied** `maxBitWidth`, the 5 being `kFixed32RegFieldBits`,
   a register-field constant a fixed-word MRS never reaches. ⇒ one opcode row + one dialect row.
2. ★★ **"A side-table would be silently dropped by the rebuild passes" is REFUTED.** The literal pool
   is a module-level side structure that survives all four — because each pass explicitly copies it.
   The argument that forced a new operand kind was false.
3. ★★ **`outputs ⊆ clobbered` — both halves of an apparent contradiction are true.** The loader
   ENFORCES it (`target_schema_json.cpp:2870-2894`); regalloc's forbidden set is `inputs ∪ clobbered`
   and *"outputs are not forbidden"* (`lir_regalloc.cpp:452-466`) — the second is **safe because of
   the first**. ⚠ **That invariant is per-OPCODE and does NOT reach the embedded path**, where the
   clobber set is per-instruction and lowering-built ⇒ **the lowering must replicate the rule itself**:
   every register-pinned output (`"=a"`, `"=d"`) goes into the instruction's clobber set, or a value
   live in `eax` across the block dies with no diagnostic.

⚠ **The highest-severity omission the audit caught, carry it into wave 2:** `lir_rewrite.cpp:541-554`
reads `implicitRegisters` to build the **spill-scratch forbidden set**, and its docblock names the
failure it prevents — *"a spilled idiv DIVISOR reloads into rax and clobbers the dividend … a SILENT
miscompile (121 not 160)"*. The scratch pool is *"the allocatable pool MINUS every register already
assigned to a vreg"*, i.e. it preferentially harvests exactly the unassigned physicals an asm block
clobbers. It is an `if`, **not** a `switch` — the compiler forces nothing. Ship a pin with a
**spilled** `"r"` operand.

---

## 5. PRIORITIES

0. **`NEXT` — THE STANDING BACKLOG LOOP (operator 2026-08-19).** One `/dss-cycle` per invocation,
   working the operator's bands (asm+miscompiles+errors → ap5/ap6 → tools → the rest) **until all
   OPEN rows are fixed, best long-term, no workarounds**. THE LIVE QUEUE IS the TOP cycle block's
   "NEXT" list — read it there, never here; this item exists so the stepper finds the loop.
   ⚠ **RE-DERIVE THE QUEUE FROM THE REGISTRY AT EVERY PICK, NEVER FROM A PREVIOUS LIST.** ✔Proved
   twice: an earlier cycle's list named `D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED` (already closed),
   and the 2026-08-19 backlog review — mine — recommended `D-ASM-PATCH-PARTIAL-OUTPUT-FAILLOUD`
   reading it OPEN HIGH off the gate's count when it had been closed since 2026-06-03 with its
   mark in the wrong cell. Recalling a row is not measuring it.
0. **QUEUED BEHIND THE LOOP — the full sqlite MATRIX RE-RUN, now UNBLOCKED.** Operator sequencing:
   *"we'll re run everything once our compile is fast enough"* — it is (Windows 38–41 s, WSL 33.6 s,
   macOS 23.0 s on the CLI kit). Re-run when a loop cycle changes CODEGEN (diagnostics-only cycles
   do not trigger it). Then FC18 ([[D-DIAG-CORPUS-EVERY-CODE]], sole remaining C23 phase — read its
   BLOCKED note first; prerequisite `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED`, which is also
   the loop's C1 head).
0. ✅ **DONE 2026-08-18 (Cycle P10): FIRST-CLASS, CONFIG-DRIVEN LTO** — [[D-OPT7-CROSSCU-LTO-SINGLE-OPTIMIZE]] CLOSED (see §0.00000000). The grammar + two-stage topology shipped; the split decided by the runtime-measured rule; runtime-differential is a standing instrument.
0. ✅ **DONE 2026-08-18 (Cycle P9): MAKE THE COMPILE FAST.** Four measured causes fixed,
   byte-identical output (§0.0000000). [[D-PERF-WINDOWS-HOST-COMPILES-8X-SLOWER-THAN-LINUX]] stays
   🟠 OPEN at a ~2.1× residual — the row says step one is an experiment, not an edit.

1. **FINISH THE P5 CYCLE.** Wave 2 is unstarted: the typed inline-asm view, the four
   semantic gates re-expressed, HIR/MIR carriage, the MIR→LIR expansion, `asm goto`'s CFG, the
   corpus + diagnostics goldens. Then register the anchor rows and run the 3-leg gate.
   ★ **Projected anchor budget: closes 4, opens 2, net −2.** Four of the implementation plan's seven
   proposed deferrals are being DONE, not parked (arm64, the mutation helper, the empty-template
   register clobber, earlyclobber).
   ⚠ **A test-design trap already identified: `__asm__("nop")` "compiles AND RUNS" is VACUOUS as a
   runtime witness** — a `nop` changes nothing observable, so an expander emitting ZERO instructions
   passes it. And it cannot be fixed with a cleverer basic template, because basic asm's register
   effects are invisible to the compiler *by design* (§2a). ⇒ the witness must be two-part: a
   **byte/structural pin** that the instruction was emitted, PLUS the runtime arm.
2. **`NEXT` — the assembly `.cfi_*` producer** (`D-ASM-CFI-UNWIND-INFO-SILENTLY-DROPPED`), unblocked.
   ⚠ **Sharpened 2026-08-14: "the 18 spellings are accepted-and-dropped" is a ONE-DIALECT claim.**
   ✔MEASURED: x86_64-att carries them, **arm64-gas carries ZERO**, and directive dispatch REFUSES an
   unrecognised spelling — so `.cfi_startproc` on arm64 **fails loud**, it is not dropped. The fix is
   **not symmetric**, and declaring them `ignoredAnnotation` on arm64 "for consistency" would
   propagate the defect into a second dialect. `cfi_escape` must stay a REFUSAL.
3. **`NEXT` — COFF `.obj` unwind tables.** Effort, not knowledge; the MSVC reference is captured
   (`.pdata` 3 `ADDR32NB`, `.xdata` 1, ORDINARY NAMED SYMBOLS not aux section symbols). ⚠ Mach-O
   `MH_OBJECT` is **blocked** — no clang on this host to measure the reference.
4. **`OPERATOR DECISION` ×3** — `D-ASM-RIP-RELATIVE-SPELLING-NEEDS-AN-IP-REGISTER` (declaring `rip` a
   `gpr` hands the instruction pointer to regalloc; blocks assembling real `gcc -S` output) ·
   `D-ASM-ADDRESS-OPERAND-CANNOT-NAME-AN-UNDEFINED-SYMBOL` (`isData` picks GOT vs PLT, a **wire-format**
   consequence) · `D-LSP-TARGET-SPEC-SPLITTER-LIVES-ABOVE-ITS-CONSUMERS` (a type split across ~25 sites).
5. **`QUEUED` — the 91 wrapped anchor citations** (must land atomically with tightening the guard).
6. **`QUEUED`** — FC18 `D-DIAG-CORPUS-EVERY-CODE` ⚠ **which is BLOCKED, and the row says so**: the
   corpus harness renders RAW SOURCE positions (`UnitBuilder::addInMemory`, no target, no `--define`s)
   while the CLI shifts the line by the predefine prologue + one per `--define` — **0 on elf, +2 on
   pe64**. Its deliverable would certify green a CLI that prints shifted positions, once per code
   added. Prerequisite: `D-PP-SEMANTIC-DIAGNOSTIC-POSITION-UNREMAPPED` (HIGH, unconditional trigger —
   under sqlite's ~25 `--define`s **every** semantic diagnostic is ~25 lines off).
   · binary rename → `dsscp` · CI + pkg-publish INERT (PR #45) · public repo (PR #37) · the
   "byte-identical vs GCC" overclaim in `pitch.txt`.

### Two anchors that must NOT be closed — closing them would itself break the bar
- `D-ASM-TARGET-DECLARES-NO-BYTE-ORDER` — no big-endian target exists to key the facet from.
- `D-ASM-COND-ON-TERMINATOR-ARMS-UNWITNESSED` — no shipped target declares `condCodeFromPayload` on
  a return or branch-with-link.

📄 Both trigger-gated. Building either is the speculative build §A.2 forbids *in the other direction*.

---

## 6. ENVIRONMENT — pre-flighted 2026-08-14

- ✔ **The WSL gate leg is TWO MERGES STALE**: `~/src/dss-code-prime` is on
  `feature/sqlite-green-full-57377343437` at `3e86a187` (PR #48) with **343 dirty files**. Its
  "untracked" files (`examples/asm/`, `src/asm/asm_text_to_lir.cpp`, the new plans) are things already
  in main that arrived by **rsync while its git branch stayed old** — it is a disposable mirror.
  ⚠⚠ **BUT it carries 4 stashes and 10+ unpushed July commits.** Sync it by overwriting the WORKTREE
  only; **never reset its git state**, and never `rsync --delete` on a variable-built path (that once
  became `rsync -a --delete / /`).
- ✔ Toolchain present: `qemu-aarch64`, `aarch64-linux-gnu-gcc` 13.3, `/usr/aarch64-linux-gnu`,
  cmake 4.3.2, ninja 1.11.1, g++ 13.3. ⚠ `clang` unsuffixed and `gcc-14` are **ABSENT**; `clang-18` is
  present.
- ★ **3-leg gate**: Win ctest (`build-dbg`) + WSL x86_64 + qemu arm64; the last REQUIRES
  `QEMU_LD_PREFIX=/usr/aarch64-linux-gnu` or ~450 arm64 examples false-red at exit 255. Use
  `DSS_STRICT_ARM_VERDICTS=1` — with strict OFF a missing emulator is a WARNING and the suite still
  passes, so a green run alone is a partial run rounded up.
- ★ **From PowerShell always `wsl.exe -e`**; from Git Bash never `wsl.exe bash -c` with a variable.
  **Quoted heredocs eat backslashes** ⇒ write the script to a FILE and run the file.
- ★ Use `scripts/run-gate/run-gate.sh` with a **TOOL-EMITTED** witness (`'ninja: no work to do|^\[[0-9]+/[0-9]+\]'`,
  `'100% tests passed'`). It correctly REFUSES a caller-authored `BUILD OK` — a watcher polling for a
  self-written success string once span until killed **over a build that had succeeded**.

---

## 7. CONCURRENT BRANCHES

📄 PRs #50/#51/#52 are merged; this branch is cut from main and, as of 2026-08-14, **no overlap
hazard is known**. ⚠ A concurrent governance workstream has shared this tree before ⇒ **stage by
explicit path, never `git add -A`** (`D-CYCLE-CANNOT-ASSUME-IT-OWNS-THE-WORKING-TREE`), and watch for
stray build artifacts (`*.preMutant`, `*.orig`) left by tooling.
⚠ **DCO: every commit needs `Signed-off-by` (`git commit -s`).**

### Dormant branches (no open PR) — do not rebase onto these
`feature/c23-conformance-burndown-2` (the asm cycles; content merged via #51) ·
`feature/c23-conformance-burndown-1` (2026-08-12, GUI + GPU plans) ·
`feature/sqlite-green-full-57377343437` (2026-08-11 — **this is the WSL mirror's branch**) ·
`feature/finish-sqlite-full-green-5366546` (2026-08-10) · ~20 older `feature/0-0-2-p*` branches.

---

## 8. TIMELINE

*Newest first. Accumulates — new cycles are prepended. Includes cycles that did not go well.*

| Date | Commit | What shipped | Gate |
|---|---|---|---|
| 2026-08-18 | *(this cycle, P8)* | **Path identity becomes a TYPE.** `core::PathIdentity` + 14 containers re-keyed + `scripts/check-path-identity/check-path-identity.py`; the 8.3 blindness of `weakly_canonical` under libstdc++ measured and closed. Plus `mustDifferFromBaseline` on the CLI runner (455 manifests / 548 arms armed) and a `--rsync` transport on both ssh carriages (the Mac's profile eats stdin) | Win **898/898** · WSL **898/898** · arm64 + macOS below |
| 2026-08-16 | *(P7 predecessor)* | **`module` corpus example** — `project_module_standalone_build`, the first corpus proof that a `module` project builds standalone (B.13.3). Closed on the standalone half only; artifact-content and must-not-exist assertions are inexpressible in the corpus and stay in unit pins | Win **866/866** · ⚠ example not re-run on the 3 non-Windows legs |
| 2026-08-16 | `f0695b7` | **AP5/AP6 close-out**, 509 files: `scripts/check-diagnostic-codes/check-diagnostic-codes.py` (the ordinal-allocation gate, built after two lanes both took `0xD029`) · `-Werror=switch` tree-wide at one chokepoint (closes G-711) · the ISA-mismatch diagnostic + its unsuppressable row · corpus arming | **All four legs 865/865**: Win · WSL gcc · qemu-aarch64 strict · macOS arm64 |
| 2026-08-14 | `867fa81` | **AP6 in flight.** Resolver + driver wiring landed (the `D_PlanNotLanded` reject is gone); git acquisition; per-target library channel; wrong-format guard at both binders; 3 latent spawn defects fixed; recursive corpus staging. **Two reds found by the merged-tree baseline and fixed**, one of them a test its authoring lane never compiled | merged-tree build **rc=0, 0 warnings** · ctest **859/861** → both reds fixed · balance **982→982 OK** |
| 2026-08-12 | `ca2c6721` | DSS Axis + DSS HIR plan rework | — |
| 2026-08-14 | *(in flight, session 2)* | **Inline-asm P5 wave 2.** Operator rulings taken (§0.1): reuse `ReturnPiece`, ZERO new value-producing opcodes; producer-declared piece source; the `ResultPiece` rename; **`asm goto` WITH outputs BUILDS this cycle**; `%N` is structural. ★ The ruling's own §8 contingency **FIRED** — `ReturnPiece`'s payload carries two facts with one stored (§0.2). ✔MEASURED the four constraint forms vs the core (§0.3): `"=r"` already works, `"+r"` and `"=&r"` are core gaps. ★ Found by reading, not by report: **regalloc cannot see the per-instruction pool at all** — 3 hand-rolled per-opcode-only union sites. Lane V (7 diagnostics) DONE; Lanes 1/G/T killed mid-flight by a context exhaustion and resumed. | ⬜ **not run — UNCOMMITTED**; dirty-tree ctest was **862/863**, sole red `anchor_registry_guard` |
| 2026-08-14 | *(in flight)* | **Inline-asm P5 — embedded `__asm__` in C.** Scope corrected (the `.temp` plan's "P3+P4" is retired numbering; the work is P5). Reference spec measured on gcc+clang (§2). Carrier + `asm goto` decided (§3). Three blockers refuted (§4). Wave 1: mutation-helper fail-open **fixed** + a vacuous shipped pin caught; LIR carrier and target vocabulary in flight. | ⬜ **not run — UNCOMMITTED** |
| 2026-08-14 | — | Two in-passing fixes: a **stale red-on-disable recipe** pointing a mutator at `c-subset.lang.json` for a row that lives in `asm.lang.json`, and an unannotated `[[nodiscard]]` discard (live `-Wunused-result`). Plus the arm64 `.cfi_*` asymmetry folded into its owning row. | balance 982→982 |
| 2026-08-13 | `d4c2836b` | **PR #52 merged.** AP5: build-lifecycle hooks, `dependsOn`, the composition-verb table | — |
| 2026-08-13 | `f3057f42` | **PR #51 merged.** DSS Axis + DSS HIR plan rework — and the asm-cycle content (`4969e9e2`, `e5b60f6c`, `e42ae5a5`, `75ca4034`) | — |
| 2026-08-13 (post-push) | — | Two findings after the cycle closed, neither moving a verdict: a build watcher that could observe FAILURE but not SUCCESS (the producer wrote `BUILD OK` to stdout only), and a **stale anchor figure** — `1018` quoted, tree measures `1019`, parent `989` | guard OK 1019 · balance 983→983 |
| 2026-08-13 | `e42ae5a5` | **Unwind lands**: DWARF CFI + `.eh_frame` on ELF/Mach-O execs (gdb unwinds 4 DSS frames) and in ELF `.o` (9 frames vs 2 stripped) · 2 silent pe64 unwind miscompiles · interior labels end-to-end · **2 false-green red-on-disable mechanisms found** | **Win 851/851 · WSL 851/851 · arm64 594/594 strict** |
| 2026-08-13 | `75ca4034` | asm-anchor burn-down: net −4 anchors; a `.s` calls libc and RUNS | Win 838/838 · ⚠ **WSL + arm64 NOT run** |
| 2026-08-13 | `e5b60f6c` | Second assembly dialect (arm64). **Shipped 2 silent miscompiles** — negative scalars lost their sign; `[x29,#-8]` read as scale | Win 831/831 · ⚠ **1 leg of 3** |
| 2026-08-12 | `4969e9e2` | Inline asm P1+P2 — assembly becomes its own source language | — |
| 2026-08-12 | `60eb8ed8` | **PR #50 merged.** C23 burn-down: silent stringize miscompile, `__VA_OPT__`, GNU spellings, UCRT migration finished | — |
| 2026-08-11 | `0ecec160` | ELF copy relocations **deleted** — name-scoped copy reloc silently emptied glibc's `environ` alias set | 5/5 build · 2 legs by execution |
| 2026-08-10 | `3e86a187` | **PR #48.** pe CRT → UCRT; MIR call-site signature checking | — |
| 2026-08-03 | `f7c378be` | **PR #46.** SQLite compiled from full upstream source, suite green | — |
| 2026-07-20 | `4ccd6c6f` | **PR #47.** Static linking all formats · long double F80/F128 · type identity | — |
| 2026-07-15 | `d0c132c3` | **PR #41.** Cross-toolchain relocatable objects — DSS `.o` links + runs under gcc | — |
| 2026-07-09 | `c7a5377f` | **PR #36.** C23 FC16 + release-optimizer perf arc (>30 min → ~2 min) | — |
| ≤2026-07-08 | — | 🧠 Compressed: C23 FC17/17.5 (`_BitInt`, `thread_local`), C11 `<threads.h>`, arm64 Mach-O, `<stdbit.h>`, Apache-2.0 relicense (PRs #36–#45) | — |
