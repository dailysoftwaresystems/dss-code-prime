# DSS Code Prime — HANDOFF

> **REWRITTEN at the end of every cycle** (`/dss-cycle` Step 8.1) and **READ FIRST at the start of
> every cycle** (Step 0). §1–§4 are a *replacement* — stale lines are deleted, not appended past.
> **§5 TIMELINE is the sole exception and accumulates.** State is what is true now; the timeline is
> how it got here.
>
> Every claim is labelled ✔**MEASURED** / 📄**DOCUMENTED** / 🧠**INFERRED**. An unlabelled claim here
> is a defect: this file is read by someone with no context, which is exactly when an unmarked
> inference does the most damage.

**Last updated:** 2026-08-14, **second session** (⚠ **MID-CYCLE, rewritten as insurance after the
first session exhausted its context and its three implementation lanes were killed by the process
exit — see §0, especially §0.4 for what is half-done in the tree**)
**Branch:** `feature/c23-conformance-burndown-3` · **HEAD:** `d4c2836b` ✔MEASURED, `== origin/main` (0/0)
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

### 0.0 ⚠ THE NEXT CYCLE'S MANDATE — four blockers, operator-scheduled 2026-08-15
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
⚠ **Never pipe `tools/run-gate.sh` into `tail`** — the harness then reports the PIPE's rc. It said
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

1. **`NEXT` — FINISH THE P5 CYCLE.** Wave 2 is unstarted: the typed inline-asm view, the four
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
- ★ Use `tools/run-gate.sh` with a **TOOL-EMITTED** witness (`'ninja: no work to do|^\[[0-9]+/[0-9]+\]'`,
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
