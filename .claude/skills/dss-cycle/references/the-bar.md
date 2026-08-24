# The bar — non-negotiable, re-read every cycle

## A. The bar — NON-NEGOTIABLE (re-read every cycle)

These hold for every line of code, every test, every commit. A cycle that cannot meet the
bar **stops and reports** — it never pushes a partial or a workaround.

1. **Source / target / linker agnostic.** No identity branch in shared substrate — never
   `if (schema.name() == "...")`, `if (arch == "...")`, `if (format == "...")`. Vocabulary
   is config-driven (`.lang.json` / `.target.json` / `.format.json`); the engine walks a
   closed verb set, never a language/CPU/format identity. This is a hard veto: if the only
   way you see forward is an identity branch, that is a **decision gate** (§B), not a cycle.
   - **★★★ 1b. REUSE THE PIPELINE'S EXISTING VERBS — a language-private verb set is the SLOW
     form of this same violation, and the grep above CANNOT SEE IT.** A new source language
     lands by binding its surface names to vocabulary that **already exists** in the
     pipeline — `LirOperandKind`, `TargetOpcodeInfo` (`terminatorKind`/`isCall`/
     `operandKinds`), `TargetCondCode`, `SymbolBinding`/`SymbolVisibility`,
     `SectionKind`/`DataSectionKind`, `TargetRegisterInfo::subOf`, the relocation set.
     Minting a `<Lang>*`-prefixed closed verb set is **FORBIDDEN**. If the surface genuinely
     cannot be expressed with existing verbs, that is a **FINDING about the shared core's
     expressiveness** — fix it IN THE CORE, where the next language inherits it, and never
     in a language-private set.
     ✔**Operator ruling, 2026-08-12, unprompted:** *"the reusable verbs is the whole core on
     when we creating support for new languages, otherwise we are literally doomed (today is
     asm, soon will be C++, then DSS Axis, then C#, and so on...)"*.
     ★★ **WHY IT IS THE SAME VIOLATION:** the roadmap is many languages on one engine. One
     private verb set per language turns the engine into N language-specific mechanisms
     wearing a config costume — and **no single site is an identity branch**, so §D's
     agnosticism scan stays clean the entire way down. It reads as "more vocabulary". This is
     the slow-motion version of the break §A.1 exists to stop, and it is invisible to the
     instrument that guards §A.1.
     ★ **Two rules that fall out of it, both measured the day it was written:**
     - **Do not restate a fact the target already declares.** A dialect knob duplicating
       `opcodeInfo.isTerminator`/`isCall` is a knob that can **disagree** with the target, and
       the loader cannot detect the disagreement (it has no target in scope). DERIVE it. This
       is the same shape as the reverted `asmSyntax` facet — *a fact with an owner does not
       get a second owner*.
     - **Syntax shape vs semantics is a real distinction and NOT an excuse.** A language does
       need to say "this text shape is a register reference"; it does **not** need its own
       notion of what a register *is*. Bind the shape to the existing kind.
     ⚠ **Check before inventing, and say what you found.** ✔MEASURED 2026-08-12: a plan
     proposed a new `views` facet for narrow registers (`eax`→`rax`@32) *and gave a false
     reason for it* — `TargetRegisterInfo::subOf` had existed all along, loaded, resolution-
     and cycle-validated, with a comment naming that exact example, and both allocator paths
     already skipped such rows. The invented facet would have been a **second spelling of an
     existing facility** in the same config file. Before adding any verb/role/kind, grep the
     pipeline for the concept and **report what you found or state plainly that nothing
     matched** (§C.-1 1b applies to vocabulary, not only to anchors).
     ⚠ There is often a mechanical payoff too: a REQUIRE-ALL config block (e.g.
     `assembly.operandForms`) breaks the load of **every** existing language document each
     time a role is invented.
2. **Best long-term solution, no workarounds.** Implement the complete clean solution now.
   "Tight slice" / "just for this case" / "TODO later" is forbidden. A real blocker is
   named and pinned (§F) — never silently deferred.
3. **No follow-ups for the hard part.** The difficult core of the priority is implemented
   **this cycle** — never sliced off as a "follow-up", "next cycle", "phase 2", or "polish
   later" when no real blocker stops it from landing now. "It is hard", "the cycle is
   getting big", or "I'll circle back" is **not** a blocker. A later cycle is legitimate
   ONLY for work behind a genuine named blocker or an unfired trigger, pinned per §F/§D. If
   the hard part truly cannot land now, that is a **decision gate** (§B): bring the options,
   do not quietly defer.
3b. **★★★ THE GOAL IS TO *WORK*. A REFERENCE THAT FAILS IS NOT A LICENCE TO FAIL — AND NEVER A
   REASON TO BREAK SOMETHING THAT WORKS.** Operator ruling, 2026-08-19, verbatim: *"we must never
   crash on correct code, even if gcc fails, we must do it right. […] If we have a reference that
   works, we must too (of course, with the implementation always following our project's best
   practices)."*
   - **The test is the DISJUNCTION, not the consensus:** if **any** reference compiler (gcc, clang,
     MSVC, …) compiles and runs the construct correctly, **DSS must too**. One working reference is
     enough to make the behaviour required. Unanimity among references is not needed, and a majority
     that fails does not excuse us.
   - **Therefore a reference's FAILURE is never evidence against DSS.** When DSS accepts something a
     particular reference rejects, the question is *"does some reference accept it, and is the
     construct correct?"* — not *"do we match the one that failed?"*. If the answer is yes, DSS is
     **right**, and the divergence is a **NON-DSS CONFOUND**: attribute it away from DSS, record it,
     and move on. Never "fix" DSS by teaching it to fail in company.
   - ⚠ **THIS BOUNDS §A.4's BIDIRECTIONALITY — read them together.** "Accepting what no reference
     accepts is also a defect" still holds, and the operative word is **NO**: the defect is accepting
     what **not one** reference accepts. It was never a rule that DSS must reproduce every
     reference's failure, and reading it that way inverts the project's purpose.
   - ★ **The implementation still has to be ours**: the whole bar applies to *how* it works —
     agnostic, config-driven, best-long-term, fail-loud, strictly tested. "It works" is the
     requirement, not the excuse.
   ✔**THE CASE THAT PRODUCED THIS RULING** (cycle P14, and it nearly went the wrong way): sqlite's
   `ext/misc/fileio.c` compiles under **MSVC** (its `_MSC_VER` activates `windirent.h`'s shim, which
   pulls in `<windows.h>`) and **fails under mingw-gcc** (the shim is gated out). DSS compiles it, by
   a third route — its shipped `<direct.h>` → `<dirent.h>` → `<windows.h>` descriptor chain. A cycle
   was about to *narrow that chain so DSS would fail too*, on the reasoning that DSS should agree
   with gcc. ⇒ **That would have broken working code to match a reference's bug.** MSVC works, so DSS
   working is the correct outcome; the gcc oracle's failure on that TU is a confound to attribute,
   not a defect to import. ⚠ Note what the measurement had to be to see this: **each reference
   probed separately**, not "the reference" as one voice — gcc's failure and MSVC's success are
   different facts, and only one of them was on file when the wrong conclusion was drawn.

4. **Fail loud.** Every unsupported construct emits a real diagnostic, never a silent
   miscompile or a swallowed error. Follow the `*Fatal` + `X_*`/`D-*` patterns already in
   `src/`.
5. **Strict-assertion tests.** Every new test asserts the strongest provable property
   (exact counts, full-sequence/byte equality, `static_assert`, death-test message match).
   See the `dss-code-prime` skill §7. A test that still passes when the implementation is
   silently broken is not strict enough.
   - **Aggregate op-count pins go INERT when a shared helper emits the same op elsewhere.** An
     `EXPECT_GE(And, N)` / op-count assertion is a worthless guard if a branchless-select (or any
     composition helper) already emits that op — deleting the guarded instance leaves the count
     satisfied and the test green (the FC17.9(b) `bit_ceil` shift-clamp pin was inert exactly so:
     two `sel()`s emitted 4 `And`s, so dropping the clamp `And` stayed green). Pin the guarded
     value's **operand chain** (e.g. assert `Shl.operand[1]` IS the clamp `And`, not a bare `Sub`)
     and **demonstrate red-on-disable** by actually removing the guard, not merely asserting present.
   - **★★ THE RED-ON-DISABLE DEMONSTRATION NEEDS ITS OWN GUARD — ASSERT THE MUTATION LANDED.**
     Red-on-disable is this project's PRIMARY defence against vacuous tests (five vacuity species
     in three cycles were each caught by it and by nothing else). It is therefore the one technique
     whose own failure is unbounded: **a mutation that silently no-ops makes the pin report green,
     and that green reads exactly like earned confidence.** ✔MEASURED 2026-08-06: a mutator process
     was killed by a cygwin fork error before it edited anything; the pin passed, as it correctly
     should have, and was briefly read as "the guard is not vacuous"
     (`D-GATE-RED-ON-DISABLE-MUTATION-CAN-SILENTLY-NO-OP`). So every demonstration must be
     **fail-closed**: the witness text is UNIQUE in the subject, the mutant DIFFERS byte-wise
     (`cmp`/hash — **never a line count**, which a same-length replacement slips straight past),
     the witness is ABSENT from the mutant, and the mutant still parses. Never infer that a mutator
     ran from its exit code alone. **Never anchor a mutation to absolute line numbers** — it can
     delete the wrong lines and produce a false red, which is untrustworthy in the other direction.
     ★★ **AND THE MUTANT MUST NOT CONTAIN THE WITNESS STRING — not even in a comment.**
     ✔MEASURED 2026-08-06, hours after the rule above was written and by the agent following it:
     a mutant was spelled `$legForwardPaths = @()  # MUTANT: TCL_LIBRARY dropped again` — and the
     pin, which searches for `TCL_LIBRARY`, **stayed green over a guard that had been removed**,
     because the comment announcing the mutation carried the very token being searched for. (The
     helper stripped whole-line comments, not trailing ones.) ⇒ every fail-closed check passed —
     the witness *was* unique, the mutant *did* differ, it *did* parse — and the demonstration was
     still worthless. **Add a fourth check: assert the witness is absent from the mutant BY THE
     SAME MATCHER THE PIN USES**, not by eye and not by a different reader. Describe a mutation in
     the harness's output, never inside the mutated file.
     ★★ **AND AN EMPTY MUTATION ANCHOR MATCHES EVERYWHERE — SO A FAIL-CLOSED CHECK WRITTEN
     AROUND ONE FIRES *AFTER* THE DAMAGE.** ⚠ ✔MEASURED 2026-08-20 (cycle P23,
     `D-GATE-RED-ON-DISABLE-EMPTY-RESTORE-ANCHOR-MATCHES-EVERYWHERE`): a mutation whose replacement
     text was the empty string made the RESTORE anchor `""`, and `str.count("")` returns **`len + 1`**
     — 8,181 on the subject file. The uniqueness clause therefore tripped on the restore, *after*
     the forward half had already run, and the source was left mutated with a totality
     `static_assert` silently gone. It was caught only because the lane re-read the file.
     ⇒ **Forbid an empty anchor** — delete by replacing with a marker, never with nothing — **and
     run the restore from a `finally` whose success is itself asserted.**
     ★★ The generalization is worth more than the fix: **a fail-closed check placed after an
     irreversible step is not fail-closed, it is a post-mortem.** Every clause above guards the
     VERDICT; this is the one that corrupts the TREE, and the next cycle would have inherited a
     source file that silently disagreed with its own pin.
   - **★★★ A GREEN RED-ON-DISABLE IS UNPROVEN UNTIL THE MUTANT IS SHOWN TO HAVE BEEN *READ*.**
     ✔MEASURED — **three independent mechanisms**, two in one cycle (2026-08-13) and a third in
     P23 (2026-08-20), each produced a green pin over a live mutant with the four fail-closed clauses
     above fully satisfied. They are not variants of one bug; they fail at different layers, which is
     why the rule has to be about the *read*, not about any one layer:
     - **The mutant was never COMPILED IN.** `ninja -t deps <obj>` reported **`#deps 0`** — ninja had
       recorded zero header dependencies, so a header-only change did not rebuild its consumer.
       **10 of 403 objects** in `build-dbg` were in that state
       (`D-BUILD-NINJA-RECORDS-ZERO-HEADER-DEPS-UNDER-CONCURRENT-BUILDS`). ⇒ use the subject
       **binary's mtime** as the build-success criterion. **Never a grep over build output** — the
       same lane's grep reported "BUILD OK" over a link that had failed.
     - **The mutant was never LOADED.** `findShippedConfig` reads `DSS_CONFIG_ROOT` *else* walks the
       cwd. `dss_add_test` sets that variable, so **ctest** reads the intended tree — but running a
       test `.exe` **directly** takes the cwd-walk and silently reads whichever tree the shell stands
       in, so a worktree binary run from the shared tree's cwd read the *shared* config and never saw
       the mutant (`D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE`). ⇒ **a config-level
       red-on-disable MUST run through `ctest`, never a bare `.exe`.**
     - **The mutant was COMPILED IN — TO THE WRONG BINARY.** ✔MEASURED 2026-08-20 (cycle P23,
       `D-TEST-RED-ON-DISABLE-MTIME-WITNESS-MUST-BE-THE-ARTIFACT-THAT-RUNS-THE-ASSERTION`): the
       mutated predicate was a **header inline**. A narrow build rebuilt the shared library and its
       mtime advanced — the instrument the clause above prescribes, behaving exactly as written
       — and the pin stayed **GREEN**, because the assertion under test calls the copy of the
       inline compiled into the **TEST EXECUTABLE**, which was never relinked. ⇒ **The witness must
       be the artifact that *RUNS* the assertion, not merely one that CONTAINS the mutated bytes.**
       For a `.cpp` in the shared library those are the same file, which is why the shorter wording
       survived this long; for a header inline, a `constexpr`, a template, or anything else the test
       TU compiles for itself, they are different files. ★ The tell was in the output and is worth
       keeping: the run reported *"1 failed"* and that one failure was an unrelated `(Not Run)` from
       another lane — **a mutant that reds the WRONG test is the same signal as one that reds
       nothing.** Read *which* test went red, never the count.
     - **The witness MOVED WITHOUT THE MUTANT — and this one indicts the instrument the first
       bullet prescribes.** ✔MEASURED 2026-08-24 (cycle P31,
       `D-TEST-A-PE-IMAGE-MD5-IS-NOT-A-COMPILED-IN-PROOF`): **a PE image carries a LINK TIMESTAMP**,
       so the shipped DLL's md5 moved **between two builds of IDENTICAL sources**
       (`5e6cbe74…` vs `10eb22ea…`), and moved for CONFIG-ONLY mutants that recompile
       nothing. ⇒ **a moved image md5 is NOT evidence the mutant compiled in — it is evidence
       a LINK happened, which happens either way.** ★ The failure mode is the dangerous
       direction: the check PASSES, so the lane believes it holds proof and stops looking, which is
       worse than having no check at all. ⇒ **the subject is the mutated TU's `.obj`**, whose md5
       must MOVE when the mutant is applied and RETURN to its baseline when restored, with **both
       halves measured in the same run**, so the object's own reproducibility is DEMONSTRATED rather
       than assumed. ⓘ Two things stay true and are worth keeping straight: **mtime remains a
       sound BUILD-SUCCESS criterion** (this bullet corrects the md5 refinement, not the first
       bullet's point about greps over build output), and **an UNCHANGED product binary is still
       sound evidence that a config-level mutant recompiled nothing** — several P31 lanes relied
       on exactly that and were right to. ⚠ Do NOT reach for `/Brepro` to make the image
       deterministic instead: that changes what the SHIPPED ARTIFACT IS in order to make a test
       convenient, and it would not fix the config-mutant case at all.
       ★★ **AND THE `.obj` CHECK IS STILL ONLY THE WEAKER HALF.** The load-bearing proof
       that a mutant was READ has always been that **the failure output carries the MESSAGE of the
       refusal it names** — a mutant can be compiled into the right artifact and still exercise
       nothing. Where the two disagree, the message wins.
     - **The OBJECT moved and the mutant still never ran — because the LINK failed.** ✔MEASURED
       2026-08-24 (cycle P31, `D-TEST-A-MOVED-OBJECT-MD5-IS-NOT-A-REACHED-THE-BINARY-PROOF`), one
       cycle after the bullet above prescribed the object as the subject: a code mutant's ctest run
       came back GREEN twice with the object md5 correctly MOVED both times. The compile had
       succeeded; the link had not — `ld.exe: cannot open output file
       bin\dss\libdss-code-prime.dll: Permission denied`, a stalled ctest child still holding the
       DLL — so ctest executed the **PREVIOUS** binary and passed. ★ **A moved object md5
       proves the mutant COMPILED. It does not prove the mutant reached the BINARY UNDER TEST**, and
       the two are separated by exactly one failure mode that this repository hits routinely.
       ⇒ **the BUILD'S RETURN CODE is the other half, checked in the same run**: a non-zero build
       makes the arm VOID, not a data point, and a harness must never let ctest run against a binary
       the build did not produce. ⓘ A third signal is cheap where the harness can take it — the
       product binary's mtime must be NEWER than the mutated object's.
       ★★ **TAKEN TOGETHER WITH THE BULLET ABOVE, THE PAIR IS THE WHOLE LESSON: the IMAGE
       moves when nothing changed, and the OBJECT moves when nothing shipped.** Each check fails in
       the flattering direction — it PASSES, so the lane stops looking — which is why neither
       is trusted alone and why the message-names-the-refusal check below outranks both.
     ⇒ **The fifth check, and it subsumes the others: prove the mutated bytes reached the process that
     ran the pin.** A changed file on disk is not a changed input. Show the artifact rebuilt (mtime),
     or the config tree that was read (`DSS_CONFIG_ROOT`), or both — and if you cannot show it, the
     demonstration proved nothing no matter how red or green it came out.
   - **★★★ AND THE FIFTH CHECK HAS A MIRROR: PROVE THE *RESTORED* BYTES REACHED THE PROCESS TOO.**
     A red-on-disable makes **two** claims — *mutant RED* **and** *subject GREEN* — and a stale binary
     silently invalidates the second. The rule above is stated only for the mutate direction, and every
     word of it applies identically to the restore.
     ✔MEASURED 2026-08-17 (`D-GATE-RED-ON-DISABLE-RESTORE-NOT-PROVEN-TO-REACH-THE-PROCESS`): a mutation
     script restored the SOURCE in its `finally` and never rebuilt, so the next script's "UNMUTATED"
     column ran against a binary that still contained the mutant. **Both of its columns were therefore
     the mutant — and they agreed perfectly, which reads exactly like a stable measurement.** It
     produced a false *"these shapes fail even unmutated"* verdict that briefly looked like a
     correctness bug in the fix under test; the corrected run **inverted the verdict outright**.
     ⇒ assert a build witness on **every** transition — warm-up → GOOD → MUTANT → RESTORED — and abort
     fail-closed if any artifact's mtime did not advance. Measure the good column only after a
     witnessed rebuild.
   - **★★ A VACUOUS ASM PIN CAN SURVIVE *BOTH* ARMS — `release` is not a rescue.** ✔MEASURED 2026-08-17,
     twice, in two different shapes. (a) A two-input asm example behind a CALL left its mutant GREEN at
     baseline and red only at release; the same defect lowered directly in `main` reddened baseline.
     (b) Worse: a **single `"+r"` operand** stayed green over a live mutant at **both** debug and
     release — the read half's `mov` targets a dead vreg emitted just before the template, the result
     vreg's range starts right after, they never overlap, and the linear scan hands the result that
     very register, already holding the right value. Two tied operands → red on both arms.
     ⇒ **run the mutant against every arm and report which actually went red.** A pin that survives
     because an undefined register happened to hold the right value is not a pin, and which shape gets
     that luck cannot be read off the source.
   - **★★ A PIN MUST DRIVE ITS SUBJECT THROUGH THE SUBJECT'S REAL INPUT PATH — never re-type its
     data.** ✔MEASURED 2026-08-06: a pin stubbed a driver's vocabulary list with eight hand-typed
     tokens — clean by construction, in a shape the driver NEVER RECEIVES — and so could not see
     that the real path returned `ran\r` on Windows (Python writes stdout in text mode; `read -r`
     strips `\n` and keeps `\r`), which would have made the driver reject EVERY legitimate token
     and fail every run (`D-TEST-A-PIN-THAT-STUBS-ITS-SUBJECTS-INPUT-IS-TESTING-THE-STUB`).
     **A pin that supplies its subject's input in a form the subject never sees is testing the
     stub.** Extract and execute the shipped code path. Where a stub is genuinely unavoidable,
     assert the stub matches what the real path produces. ★ And prefer assertions on **CONTENT**
     over **COUNT**: "eight tokens" was satisfied by eight corrupted tokens; "the tokens are clean"
     was not.
   - **Multi-site / multi-form contracts** — the "apply X at every site/form of class C"
     class (e.g. "strip the specifier prefix at every positional decl resolution"). A green
     suite over a SUBSET of the sites/forms is NOT proof: latent misses at the unexercised
     sites survive review *and* green. (Cycle-13 audit case: a 4th missed strip site — the
     enum-enumerator value loop — survived the cycle-12 review because its test exercised
     only a variable decl.) Close it one of two ways: **(a)** funnel ALL sites through ONE
     chokepoint so coverage is by-construction — duplication is what breeds the missed site;
     or **(b)** have the closing test exercise EVERY form of C, *including forms not yet
     consumed by any shipped language*, via a synthetic schema that constructs the consuming
     shape itself. An unconsumed substrate's misses are latent by definition — the test must
     build the consuming shape, not wait for a real consumer to expose it.
   - **Real-execution corpus example** — when the feature produces *observable end-to-end
     behavior* (a source construct that compiles to a binary whose exit code / stdout reflects
     it), ship a **runnable corpus example** exercising it: `examples/<lang>/<name>/`
     (`main.<ext>` + `expected.json` — the differential runner compiles → spawns → asserts the
     exact exit/stdout). A binary that runs correctly proves the whole source→…→`.exe` chain a
     unit/MIR-tier test cannot. **First check the existing corpus** — if a fixture already
     exercises the feature, reuse it (add an `optimizedPipelines` arm or an assertion) rather
     than duplicate; add a NEW example only for a genuine coverage gap. **Make it a *good*
     exercise, not a vacuous one:** the feature must actually manifest at runtime with operands
     no earlier pass can fold away (cycle 10r's division corpus uses runtime function-args,
     never a literal `100/7` that const-folds before the idiv ever runs). **Witness the
     OPTIMIZER, not just the lowering:** a runtime-observable feature's corpus MUST carry an
     optimized arm that runs the REAL shipped pipeline — `{"shippedPipeline": "release"}`, NEVER
     a hand-listed `passes` subset and NEVER baseline-only — so the optimizer×feature composition
     (Inlining / Mem2Reg / LICM over the feature's NEW MIR shapes) is exercised end-to-end at
     runtime. A baseline-only example runs the no-op `debug` pipeline, and a hand-listed subset
     drops passes the release build runs; either silently lets a future optimizer regression — or
     a frame-slack-masked overrun — pass green (the FC7-C3 array-storage width overrun the
     `passes`-subset corpus MASKED; the FC12 variadic cycle then shipped 18/19 examples
     baseline-only, leaving the optimizer's effect on the new va_list cursor/alloca shapes
     unwitnessed). The MIR/LIR pins lock the *pre-optimizer* shape; only a `release`-arm corpus
     proves the optimizer preserves the feature's runtime semantics. **Carve-out — do not
     manufacture a vacuous corpus:** a feature with *no* runtime-observable behavior (pure
     substrate, a diagnostic-only fail-loud, a MIR-tier transform with no runtime difference —
     e.g. single-CU `static`→Local DCE just drops an unused symbol — a behavior-preserving
     refactor) is proven by its appropriate-tier strict test (+ red-on-disable); a corpus that
     exercises nothing is itself the masked-effectiveness trap.
   - **Cross-target runtime closure — encoding pins are NOT runtime proof.** When a feature's
     correctness is *runtime control-flow* on a target the local gate cannot execute (any
     non-host CPU/ABI — e.g. an ARM64 binary on an x86_64 host), byte/encoding pins prove the
     bytes are *right*, never that the program *runs* right: a function can encode every
     instruction perfectly yet omit the link-register (x30) spill in a non-leaf frame → perfect
     bytes, runtime SIGSEGV (the v0.0.2 catch — `2b07e3b` shipped a "Runnable ARM64-ELF FFI
     proof" that byte-pinned green then SIGSEGV'd on the native-arm64 leg; `dbf84b0` was the x30
     fix). So such a feature is **NOT `✅ CLOSED` on local-green + byte-pins** — it stays
     *runtime-pending* until the binary has actually **executed on the target** and produced the
     asserted exit/stdout. Each cross-target has a matching CI leg that builds AND runs `ctest`
     **natively** — gate the closure on that leg going **green** (push, confirm via next cycle's
     Step 0 baseline or `gh run watch`, then mark CLOSED; never on the push alone):
       - **ARM64-Linux** → the native `ubuntu-24.04-arm` leg (`run-linux-arm64: true`); RISC-V /
         WASM → an emulator-gated leg.
       - **macOS-ARM64** → the **`macos-latest` (Apple Silicon) leg** (`run-macos: true`) — it
         runs `ctest` on real arm64 hardware, so it auto-executes a Mach-O corpus exactly like the
         arm64-Linux leg (it is NOT leg-less).
     **Local pre-push run — native when the host matches the target.** If you build ON the
     target's own OS/arch, the corpus executes in your LOCAL `ctest` and goes **green locally**
     before the push: e.g. **on a macOS (Apple Silicon) machine the macOS-ARM64 corpus runs in
     local `ctest` → green → push → the `macos-latest` leg re-confirms** (no human step). Off the
     target host, ARM64-Linux can still run locally under `qemu-aarch64`, but **macOS-ARM64 is the
     one target with no off-Mac emulator** (nothing runs a Mach-O on Windows/Linux) — so from a
     non-Mac host (the loop's usual env) either hand the Mach-O to a Mac for a manual pre-push run
     (a 2-step, **§B** hand-off: present the binary + expected exit/stdout) or rely on the
     `macos-latest` CI leg to execute it post-push.
     In every case ALSO ship a **host-independent** structural pin that is red-on-disable on
     *every* leg (e.g. the non-leaf frame puts the link register into `savedRegs`), so a
     regression is caught even when the one execution path is unavailable — the execution run is
     the end-to-end witness, the structural pin the always-on guard; keep both, never collapse
     them.
6. **The full commit gate.** The operational checklist (with commands) is **Step 6 (§C)** —
   the single source of truth for the gate items. **Any red the cycle cannot self-repair →
   STOP and report. Do not push.** Self-repair = a mechanical fix obvious from the failure
   (missing include, stale assertion, fold-induced break); if the red implies a design choice
   or reveals a real blocker, it is a **§B gate**, not a repair.
7. **No un-anchored issue — every issue you come across is ANCHORED *and* HANDLED, never worked
   around.** The moment a cycle *comes across* ANY issue — a bug, a silent-miscompile risk, a
   build/gate/CI fragility, a flaky test, a stale doc, a missing guard, a surprising behavior —
   it is **FORBIDDEN** to leave it un-anchored **or** to route past it with a workaround. This
   holds **even when the issue is outside the current cycle's scope, and even when its proper
   fix belongs to a later cycle.** "Not my cycle" / "I'll remember it" / "I excluded the failing
   test" / "it passes on the other leg" / "green modulo X" is *precisely the trigger to anchor*,
   never license to drop. Two obligations, BOTH mandatory, BOTH in **this** cycle:
   - **(a) Anchor it now** — a real registry row in `_deferred-anchor-registry.md` (name +
     what/why + trigger + closing-work), committed THIS cycle. A prose-only note in a commit
     message, a chat reply, or a code comment is **NOT** an anchor: an un-anchored issue is
     invisible to the next cycle, to the anchor guard, and to the plan sweep — so it *will* be
     silently lost. (If the issue is a live `D-*` you must also cite it in `src`/config; if it is
     purely infra/docs, the registry row alone suffices.)
   - **(b) Address it properly** — **FIX it now** if it blocks the cycle's gate, is small, or is
     within reach (default to fixing); **ELSE** pin it as a genuine **deferred anchor** with an
     explicit trigger + closing-work (§F/§D) — a later cycle is legitimate ONLY behind a named
     blocker or an unfired trigger. Either way it is *handled*: NEVER a silent skip, a masked
     test exclusion, a swallowed error, or a "temporarily disabled" that no anchor tracks.
   - **★★★ (c) THE QUICK-FIX RULE — AN ANCHOR IS NOT A PLACE TO PUT WORK YOU COULD HAVE DONE.**
     ★★★ **THE DEFAULT IS FIX. DEFERRAL IS A §B DECISION THE USER MAKES, NOT ONE THE CYCLE MAKES.**
     A cycle may not, on its own authority, decide that something it found is somebody's problem
     later. Either it closes, or it goes to the user as a named choice. This is enforced by the
     **anchor balance gate** in Step 6: end with more OPEN rows than you started and the gate FAILS,
     exactly like a red test.
     The registry is an **audit trail**, not a backlog, and it is measurably drifting into one: it
     passed **885 rows** on 2026-08-07 and **350 OPEN** on 2026-08-11 — 22 of them opened by a single
     five-commit branch — while rows are still being opened for defects that were then fixed minutes
     later in the same cycle. A row that describes a defect nobody is going to fix this year is not
     "handled" — it is the deferral §F.0 forbids, wearing a registry row as a disguise.
     ★★ **AND THE OPERATOR IS THE ONE WHO PAYS.** ✔MEASURED 2026-08-11, the user, twice, unprompted:
     *"I HATE YOUR WAY TO CLOSE AN ANCHOR WHILE OPEN ANOTHER 534354 anchors… CAN'T YOU PLEASE START
     AND FINISH A JOB WITHOUT LEAVING ANYTHING BEHIND?"* Every open row is a decision billed to a
     human on every future sweep. A cycle that closes 5 and opens 9 did not make progress; it moved
     work from a place where it was being done to a place where it is being counted. So:
     - **THE COST TEST, and it is a hard rule: if writing an honest anchor row costs more than
       fixing the thing, FIXING IS MANDATORY.** An honest row carries what/why/trigger/closing
       work/cross-refs — for most small defects that is more thought and more keystrokes than the
       fix. Whenever you catch yourself composing a row for a one-line guard, a stale comment, a
       missing `rm -f` before a marker write, a wrong path spelling, a misleading diagnostic
       string: **stop writing and fix it.** Then still write the row — **born `✅ CLOSED`**, as
       the record of what happened, with the fix and its verification in it. A closed row costs
       the next reader nothing; an open one costs them a decision every time they sweep.
     - **DEFAULT TO CLOSING IN THE SAME COMMIT.** The question is never "should this be anchored?"
       (it always should) but "is there a NAMED blocker stopping me closing it right now?" —
       §F.0's (a)/(b)/(c) gate, applied to every candidate row, not only to the cycle's headline
       work. "It is out of scope for this cycle" is NOT a named blocker for a five-minute fix; it
       is the most common way the list grows.
     - **THE SWEEP IS PART OF THE CYCLE, NOT A SEPARATE PROJECT.** When a cycle touches a file
       that already carries OPEN rows against it, close the ones now within reach — you have the
       context loaded, which is the expensive part, and it will not be loaded again for a while.
       ★ ✔MEASURED 2026-08-07: a recipe defect was re-investigated from scratch because its row
       had sat OPEN since 2026-08-05 with the fix already written in its closing cell. The open
       row did not save that work; it *duplicated* it. An anchor only pays for itself if someone
       later reads it — and the longer the OPEN list, the less likely that is.
     - **REPORT THE OPEN COUNT AT STEP 10, EVERY CYCLE, WITH ITS DELTA.** A cycle that closes
       fewer rows than it opens is not automatically wrong — a real investigation legitimately
       opens rows — but a sustained positive delta means the quick-fix rule is being skipped, and
       the number is what makes that visible instead of arguable. `scripts/check-anchor-registry/check-anchor-registry.sh`
       already prints the total; the per-cycle honest line is "opened N, closed M, net ±K".
   A workaround that *hides* an issue (excluding a failing test, catch-and-swallow, "it's green
   on the other leg so ignore it here") is the exact silent-failure the bar exists to prevent —
   it violates §A.2 (no workarounds) and §A.4 (fail loud) as well as this rule. **Motivating
   catch:** the TF-C51 fat-archive gate hit a real GNU-on-Windows COFF `-Wa,-mbig-obj` scope gap
   on an *unrelated* test TU (`test_mir_to_lir.cpp`, "file too big"); the first instinct —
   exclude that test from the Windows leg — was a workaround. Correct handling per §A.7:
   root-cause → anchor `D-BUILD-GNU-WINDOWS-BIGOBJ-SCOPE` → fix it (project-wide flag) → witness
   the TU now builds+passes → commit. **An orthogonal issue you merely *found* is still yours to
   anchor + handle** — the discovery is the obligation.

---
