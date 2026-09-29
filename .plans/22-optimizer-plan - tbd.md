# In-tree Optimizer — Sub-Plan (22)

> Owns the **optimization layer**: a multi-tier set of transformation/analysis passes over HIR, MIR, and LIR, plus the per-target cost/machine models that parameterize them. ONE engine of universal algorithms (bucket-2) reading JSON-declared vocabulary (bucket-1) — there is **no per-language and no per-target optimizer C++**. Adding a target's tuning = drop numbers in its `.target.json`; adding a language costs nothing (everything is already neutralized at the HIR→MIR boundary).
>
> **Thesis extension (the load-bearing bet of this plan).** Optimization *heuristics* — the cost model, the pass-pipeline order, the profitability thresholds — are **data, not code**, from the first PR. That is what turns tuning from a decade of hand-work into a *searchable space*: the same property that lets a vendor ship a `.target.json` lets the optimizer eventually **tune itself** against a corpus. A frozen-C++ heuristic kills that endgame on arrival; a JSON heuristic preserves it for nearly free. This plan therefore treats "heuristics-as-data" as a non-negotiable substrate decision, not a future refactor.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | 🟩 **IN-FLIGHT — OPT1–OPT6 v1 + v1.x mandatory subset DONE; OPT7-prep cycles 10a–10r LANDED (incl. Arc A integer-division codegen, 2026-06-04); OPT7 cross-CU INLINING arc LANDED 2026-06-05 (`D-OPT7-1` ✅ single→multi-block→non-leaf/SCC-gated→IntrinsicCall splice; MIR-tier whole-program merge `mergeCuMirs` = `D-OPT7-CROSSCU-MIR-MERGE` ✅; `inlineThreshold` config knob shipped in release.json; runtime corpora `weak_inline_crosscu` / `non_leaf_inline` / `cross_cu_call` inlining arm — see the cycle log below).** v1 ships the mandatory scalar subset; the full multi-tier roadmap + the autotuner research arc are scoped here and anchored as deferred cycles. **Maximal scope by directive** — HIR/MIR/LIR/per-target passes AND the learned-heuristics frontier all live in this one plan. **All gate conditions met**: (a) file-emission proven 2026-06-02 (`int42.c`, `hello_puts`, `hello_writefile`, `hello_printf` on Windows x86_64); (b) ≥18-example corpus (post-cycle 10h-10i additions: `deref_load`, `deref_store`, `long_primitive_smoke`, `strict_alias_cse_capstone`) — distinct CFG/arithmetic/aliasing shapes for differential verification of every OPT1–OPT6 transform; (c) **alias arc CLOSED end-to-end** (cycles 10a-10g substrate + 10i corpus-binary capstone + MIR-tier effectiveness pin tightened 10o); (d) **LICM chained-invariants CLOSED** (cycle 10j, per-loop fixed-point + silent-miscompile negative regression guard). |
| Predecessors  | ✅ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) (MIR SSA-over-CFG + dominators-in-verifier + the `lir_pass_util` build-once-freeze pass pattern — ML1–ML8 closed). ✅ [`12.5-const-eval-plan`](./12.5-const-eval-plan%20-%20ok.md) (shared scalar const-eval engine — the MIR fold pass reuses its arithmetic core). ✅ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) (LK1–LK10 closed + RUNNABLE 2026-06-02 — file emission + D-LK10-ENTRY Stage 1 + D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY). ✅ Step 13.5 cycle 1+2 (2026-06-03 — D-CSUBSET-WHILE-LOOP-SUBSTRATE + 14 runnable corpus examples providing differential-verification gates). ✅ **D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD** (2026-06-03 — `SymbolBinding` / `SymbolVisibility` lifted from `link/object_format_schema.hpp` to `core/types/symbol_attrs.hpp`; threaded onto `MirFunc` + `MirGlobal` POD via the existing 4-byte pad slot; `isExternallyVisible(binding, visibility)` is the DCE-protect predicate the optimizer's DCE pass MUST consult before deleting any function/global. c-subset front-end defaults Global+Default for every user function/global — the C-without-`static` convention — so DCE today preserves everything. Closes plan-22 §2.9 prerequisite for OPT1's DCE step). **All upstream gates met; OPT1 unblocked.** |
| Project gate  | ✅ **GATE MET 2026-06-02** (file emission) + ✅ **CORPUS MET 2026-06-03** (14 runnable c-subset examples). **Prior gate-status note** (2026-05-30, since closed): the linker then produced only in-memory `LinkedImage::bytes` and intra-module relocs (LK6 c1) — no on-disk executable file. **Closure path**: LK10 cycle 2 file emission + D-LK10-ENTRY Stage 1 entry trampoline + D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY Win64 frame-bias closure landed runnable on-disk binaries; step 13.5 cycle 1+2 closed the while-loop / multi-fn / setcc-width substrate + the 14-example corpus. Rationale honored: "it runs everywhere" (file that loads + runs ✅) is in before making it run *fast* everywhere. |
| Successors    | Feeds the same backend it always did — [`13`](./13-assembler-plan%20-%20tbd.md) / [`14`](./14-linker-plan%20-%20tbd.md) consume optimized MIR/LIR transparently. [`17`](./17-shader-gpu-plan%20-%20tbd.md) (SPIR-V) + [`18`](./18-wasm-plan%20-%20tbd.md) share the MIR optimizer verbatim (the config-thesis multiplier — one pass, every backend). |
| Scope         | **Bounded for v1; roadmap maximal.** v1 mandatory: OPT1 (substrate + heuristics-as-data) + OPT2 (DCE / const-fold / copy-prop). v1.x–v2: OPT3–OPT8 (redundancy, control-flow, LIR peephole/coalescing, loops, inlining, scheduling). Research frontier: OPT9 (vectorization), OPT10 (the autotuner). Every non-v1 item is an anchored D-OPT deferral, not a silent gap. |

---

## 0.1 Stepper

### OPT-tier status overview (high-level — read this first)

| Tier  | Pass                                       | Status               | Cycles done | Last commit |
|-------|--------------------------------------------|----------------------|-------------|-------------|
| OPT1  | Substrate (engine + PassId + pipelines)    | ✅ **DONE**          | c1          | `5d29532` + `7d5932e` |
| OPT2  | Const-fold (MIR-tier)                      | ✅ **DONE**          | c1          | `8bae225` + `1a2bed5` |
| OPT3  | DCE + CompileConfig threading              | ✅ **DONE**          | c1          | `52c1380` |
| OPT4  | Pre-cond refactors (dom-tree + rebuilder) + Mem2Reg + SSA copy-prop | 🟩 **c1+c2+c3 DONE** | c3          | `256b970` + `2e89b83` + `f496aea` |
| OPT5  | CSE / GVN + SimplifyCFG + pipeline fixed-point | 🟩 **c1+c2 DONE** | c2          | `ec7220b` + `96bb941` |
| OPT5+ | **SimplifyCFG**† — branch-fold + jump-thread + block-merge + marker re-derivation (MIR; recurring) | 🟩 **c2+c3 DONE** (single-non-Linear-marker case; both-sides-non-Linear case anchored at `D-OPT4-1-NON-LINEAR-MARKER-MERGE` — **✅ CLOSED 2026-06-12 by the D15 cycle-B `rederiveStructCfMarkers` substrate: the gate's marker condition deleted, both-non-Linear merges admitted, markers re-derived canonically; D-OPT4-1 FULLY closed**) | c2+c3 | `96bb941` + `738413d` |
| OPT6  | LICM (Loop-Invariant Code Motion) | 🟩 **c1 DONE + chained-invariants CLOSED 10j + preheader-skip Info diag 10l** (preheader insertion still deferred — trap-safe conservative-safe default RUNTIME-PINNED cycle 11 (`licm_trap_safe_hoist/`, regression-proven, retires the 10j HARD STOP); aggressive hoist-when-safe needs value-range **OPT13**; preheader-insertion needs Phi-merge substrate, supervised cycle) | c1 + 10j + 10l | `164a1ca` + `35cc798` + `7629f5f` |
| OPT7  | Inlining                                   | 🟩 **cycles 1–6 done (the user-directed OPT7 sequence c3–c6 COMPLETE)** — single-block (c1) + multi-block-leaf (c2) splice + §2.9 legality gate; **cross-CU MIR merge LANDED (c25, `D-OPT7-CROSSCU-MIR-MERGE` ✅); cross-CU INLINING ✅ DONE (c26, `D-OPT7-1` ✅) — the optimizer now runs on the whole-program merged module → cross-CU calls inlined**; **+ general NON-LEAF inlining + SCC recursion safety (c27: lifted Call-leaf, new `call_graph_scc` Tarjan, recursive-SCC refusal — generalizes self→mutual recursion; non-leaf is correctness-preserving + opt-in)** **+ inline COST MODEL + SHIP-IN-RELEASE (c28: `OptPipeline.inlineThreshold` config-driven; `Inlining` now in `release.pipeline.json` — a real -O optimization)** **+ IntrinsicCall inlining relaxation (c29: the gate admits an IntrinsicCall-bearing callee; frame-sensitive gating deferred trigger-gated behind a fail-loud registry-empty tripwire)** | `opt/passes/inlining.cpp` + `opt/analysis/call_graph_scc.{hpp,cpp}` + `mir/merge/mir_merge.cpp` | `test_inlining` + `NonLeafCalleeIsInlined` + `MutualRecursiveCallIsNotInlined` + `IntrinsicCalleeIsInlined` + `MultiBlockIntrinsicCalleeIsInlined` + `VoidIntrinsicCalleeIsInlined` + `NoShippedConstructLowersToIntrinsic` + `test_call_graph_scc` + `CrossCuCallIsInlinedOnMergedModule` + `WeakCalleeSurvivingMergeIsRefusedByMergedOptimize` + `InlineCostModelRefusesLargeCallee` + `InlineThreshold*Rejects` + `ShippedReleaseContainsInlining` + `test_mir_merge` + `test_type_reintern` + `non_leaf_inline` + `multiblock_inline` + `weak_inline_crosscu` + the `cross_cu_call` Inlining arm |
| OPT8  | LIR peephole + register coalescing         | ✅ **COMPLETE 2026-08-26 (cycle P40 lane I)** — BOTH halves ship. **Peephole** landed in P36 (wired at step 8b of `compile_pipeline.cpp`, `lir/test_lir_peephole` a registered `ctest` entry) with R2 fallthrough elision in P39. **Register coalescing** is now in the linear-scan allocator: it allocates CLASSES of copy-related vregs, merged by a union-find whose edges are (a) full-width declared class MOVEs between two vregs and (b) the (result, tied-operand) pair of a `requires2Address` opcode, under four vetoes — interference (`lirRangesInterfere`, the ONE predicate shared with the allocator's own `expireActive` reuse rule and with the independent auditor), anti-affinity against an untied operand (D-CSUBSET-BINOP-RIGHT-CLOBBER by another route), register class, and PRESSURE. Plus **spill-slot coalescing** (a slot is reusable once its occupant's hull ends) and **pre-coloring** (`D-PLAN12-REGALLOC-PRE-COLORING-HINT-FOR-ARG-CALL-ARG`): a parameter is homed in its own incoming argument register and a copy with one PHYSICAL end biases the vreg into it, so `maybeMov` emits nothing. Fail-loud: `findAllocationConflict` re-derives interference from liveness + the finished assignment table, never from the union-find, and `regallocFatal`s rather than emit a two-values-one-register artifact. ✔MEASURED over 517 `examples/c/**` ELF64/x86_64 artifacts against `0c151f2a` under one PINNED config snapshot: debug **16089 → 12761 register-to-register copies (−20.7%)**, total emitted instructions **123930 → 120665 (−2.6%)**, **421 examples smaller / 91 unchanged / 5 larger (worst +4)**; release **16229 → 14124 copies (−13.0%)**, **101255 → 99021 instructions (−2.2%)**, **437 smaller / 78 unchanged / 2 larger (both +1)**. Spill counts and the set of compile failures are UNCHANGED in both configs. ★ Where the residue is, ✔MEASURED through the compiler's own post-stage LIR dump over 500 examples: only **1042** copies survive at POST-REWRITE — the tier the coalescer acts on — against **11925** at POST-CALLCONV, so what is left is overwhelmingly outgoing-argument moves that `materializeCallingConvention` mints AFTER the peephole has run. Removing those needs the pre-coloring hint on the USE side, which `D-PLAN12-CLOSED-2026-P40-LANE-AND-THE-ROW-WAS`'s move-cycle refusal blocks. — ⚠ **OPT8 IS EXACTLY THESE TWO THINGS AND NOTHING ELSE; see §0.4.** The label was used for three different scopes across this document and the other two now have their own ids: instruction scheduling is **OPT19**, and the value-range analysis several rows park on as "OPT8+" is **OPT13**. | P36 + P40 | `examples/c/regalloc_coalesce_copy_chain` |
| OPT9  | Vectorization (auto-SIMD)                  | ⏳ **planned (v2)**   | —           | — |
| OPT10 | Autotuner (research frontier)              | ⏳ **planned (v2+)**  | —           | — |
| OPT11 | **Cross-CU SUMMARY INDEX** (replaces the monolithic whole-program merge) | 🟩 **PHASE 1 LANDED 2026-08-26 (cycle P36); THE ARC IS OPEN** — operator-decided 2026-08-25, see §0.2. The per-object summary and MIR sections are DECLARED in all ten relocatable/staticlib format documents with a per-format non-ALLOC spelling, and the producer plus the index live in `src/mir/summary/` behind three registered `ctest` entries. ⚠ The remaining work is [[D-OPT11-LAZY-IMPORT-EDGE]], which is OPEN — and its option A (*"reuse the fixpoint's own max"*) is CONTAMINATED: every figure derived from the pre-P36 `max` is an artifact of a fixpoint that DIVERGED rather than converged late. | P36 | — |
| OPT12–OPT22 | **The remaining distance to a first-class optimizer** — 11 items across the scalar, interprocedural, machine and feedback tiers | ⏳ **ROADMAP — operator-directed 2026-08-25 (P36); see §0.3** | — | — |

**Mandatory for v1**: OPT1 ✅ + OPT2 ✅ + OPT3 ✅.
**v1.x→v2 roadmap**: OPT4–OPT8.
**Research arc**: OPT9 + OPT10.

† **SimplifyCFG is a recurring cleanup, not a linear OPT-N step** — hence the `OPT5+` label rather than its own integer (renumbering LICM→Autotuner would churn name-stable pins for no gain). Implemented in **two cycles** (OPT5+ c2 — branch-fold + empty-block jump-thread + pipeline-level fixed-point loop, commit `96bb941`; OPT5+ c3 — general block-merge + single-non-Linear-marker re-derivation, commit `738413d`), carrying the load-bearing **StructCfMarker repair** (D-OPT4-1, §2.7 — re-derive marked regions post-transform so WASM/SPIR-V codegen stays intact; this is the DSS-specific constraint LLVM's SimplifyCFG doesn't have). The both-sides-non-Linear case (full CFG-shape-driven re-derivation) is anchored at `D-OPT4-1-NON-LINEAR-MARKER-MERGE` in §3.1 — **✅ CLOSED 2026-06-12 (D15 cycle B): `deriveStructCfMarkers` (dom + post-dom canonical derivation, `src/mir/mir_struct_markers.{hpp,cpp}`) replaced the incremental repair entirely — every producer re-derives, the verifier checks stored==derived equality, the merge gate's marker condition is gone; D-OPT4-1 is FULLY closed**. **Thereafter a recurring pipeline citizen**: listed multiple times in `*.pipeline.json` and run to fixed point interleaved with ConstFold + DCE — ConstFold turns `if (5 < 3)` → `if (false)`, SimplifyCFG deletes the dead arm, DCE removes the now-unreachable code, exposing more folding (the classic mutually-enabling cleanup cluster). See the §3 SimplifyCFG row + `D-OPT-FIXED-POINT-LOOP` (closed OPT5+ c2).

## 0.2 ★★★ THE OPERATOR'S ARCHITECTURE RULING — SUMMARY INDEX + OPT8, AND **NO TRADE-OFF** (2026-08-25, cycle P36)

> **Verbatim:** *"use /dss-cycle so we have the BEST inlining EVER. We know how our references do, so we
> know what to do. We need the summary index + OPT8. No trade off will happen."*
> Taken together with the same day's §B ruling on `D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION`:
> *"Yes — build it, it IS the compile-time answer."*

**THE DECISION.** DSS replaces the **monolithic whole-program MIR merge** — which runs on EVERY build today —
with a **per-module SUMMARY INDEX**: a compact record of each TU's call graph and symbol/linkage facts, a cheap
global pass over the summaries alone to decide what to import where, and then **every TU optimized IN PARALLEL,
importing on demand only the functions chosen for inlining**. The whole-program *decision* is decoupled from the
whole-program *transformation*. This is OPT11, and it composes with separate compilation (`.c` → `.o` per TU) so
that per-TU results stay cacheable and rebuilds stay incremental.

★★★ **"NO TRADE OFF" IS A REQUIREMENT ON THE DESIGN, NOT AN ASPIRATION.** Both clauses bind, and a design
surrendering either one is refused:
1. **Inlining quality at least as good as today's whole-program merge** — the "BEST inlining EVER" clause;
2. **Parallel + incremental + cacheable** per-TU optimization — the reason the index exists at all.

### Why — the measurements, and they invert the received story

⚠ **THE PREMISE THIS PLAN CARRIED UNTIL TODAY WAS WRONG: THE REFERENCE COMPILERS DO NO CROSS-CU INLINING AT ALL.**
✔MEASURED 2026-08-25, gcc 13.2.0, two TUs where one calls a small function in the other:

```
gcc -O2   (separate TUs)   ->  2 call instructions remain in use()   = NO cross-CU inlining
gcc -O2 -flto              ->  0 call instructions                   = cross-CU inlining
```

`gcc -O2`, `clang -O2` and MSVC `/O2` — **the exact arms `README.md` benchmarks DSS against** — compile each TU
separately. What the C ecosystem actually uses is **headers** (`static inline`, textually included by the
preprocessor) and **amalgamation** (SQLite ships a 250k-line `sqlite3.c` precisely so a compiler can inline across
"modules"). LTO is opt-in and off by default everywhere, because it is expensive:

```
103-TU full-source sqlite, same host, same -j4
gcc -O2         compile 10.82s   link  0.11s   TOTAL 10.93s
gcc -O2 -flto   compile  4.51s   link 26.42s   TOTAL 30.94s
                                cross-CU inlining costs gcc 2.83x
```

★ **READ THE SHAPE, IT IS THE ENTIRE ARGUMENT FOR THE INDEX.** Under LTO gcc's per-TU compile gets *cheaper*
(10.82 → 4.51 s — the real work is deferred) and the **link explodes to 26.42 s of SERIAL whole-program work**.
That is the monolithic-LTO bottleneck, and DSS has the identical signature: ✔MEASURED (P36 lane D, 103-TU sqlite)
the front-half CU pool at **3.96×**, the back-half at **3.96×**, and the merged-module `optimize` at **1.10× at
`--jobs 4`**. **gcc has our exact problem; they simply only pay it when the user asks.** DSS pays it on every
build, and is then benchmarked against arms that are not paying it at all.

ⓘ For scale, and it is why this is not a catch-up exercise: post-P36 DSS at `-j4` is ~27.6 s against gcc `-flto`
~30.9 s on the same sources and box. ⚠ **INDICATIVE ONLY — separate runs, different target formats (elf64 vs
pe64), a loaded machine.** It must be re-measured same-run/same-target/quiet before it is published anywhere.

### What this obligates elsewhere

- ⚠ **`README.md` compares DSS against a mode that is not doing what DSS does.** The honest table needs an
  `-flto` arm beside `-O2`, because only the LTO arm performs cross-CU inlining. Owned by the orchestrator.
- ⚠ **The "the answer is the pool" claim is REFUTED and must be corrected wherever it is published.** The CU pool
  achieves 3.96×; the serial fraction is the whole-program optimizer
  (`D-PERF-PROGRAM-STAGE-OPTIMIZER-IS-SINGLE-THREADED`).
- **OPT8 (LIR peephole + register coalescing) is scheduled alongside**, and is the right target for the OTHER
  half of the gap: DSS already has *more* interprocedural scope than the references at `-O2`, and its emitted
  sqlite still runs **1.13×–1.78×** slower. That deficit is per-function codegen quality — instruction selection,
  register allocation, the pass set — **not** inlining scope.
- **Determinism is the load-bearing risk.** Per-TU parallel optimization with on-demand imports must emit a
  byte-identical artifact run to run: no iteration-order dependence, no address-ordered containers, no
  first-finisher-wins. The corpus proof already in use (`examples/c/**` 630/630 + a stable artifact md5) is the
  standard it must meet.

## 0.3 ★★★ THE REMAINING DISTANCE TO A FIRST-CLASS OPTIMIZER (operator-directed, 2026-08-25, cycle P36)

> **Verbatim:** *"so also adjust our optimizer plan so we have the BEST FIRST CLASS OPTIMIZER, add the
> remaining items"*

### What we actually have — measured, not recalled

✔MEASURED 2026-08-25 by reading `src/opt/optimizer.hpp`'s `PassId` and `release.pipeline.json`, the engine
knows **exactly nine passes**:

```
Identity · ConstFold · Dce · Mem2Reg · CopyProp · Cse · SimplifyCfg · Licm · Inlining
```

That is a sound classical **scalar** core and it is genuinely short of a production backend. ★★★ **AND THE
MEASUREMENTS SAY THE DEFICIT IS REAL AND IS *NOT* IN INLINING SCOPE.** DSS merges whole-program MIR on every
build while gcc/clang/MSVC at `-O2` do **no cross-CU inlining at all** (§0.2) — **we have MORE interprocedural
reach than any reference and STILL emit slower code**:

```
DSS emitted sqlite, `speedtest1 --size 25`, vs each host's own reference
  vs MSVC          1.13x   <- nearly parity
  vs gcc / clang   1.41-1.43x
  vs Apple clang   1.78x   <- the far end
```

⇒ The runtime gap is concentrated in the **scalar depth** and the **machine tier**, which is where every item
below lives. ⚠ That is a diagnosis, not a promise: the list is a ROADMAP of anchored intent, not a schedule,
and nothing here is scheduled until a cycle takes it.

### The twelve items

⚠ **STATUS DISCIPLINE:** every row is ⏳ and carries the tier it belongs to and what it depends on. A row's
"why" is MEASURED where a number is given and INFERRED where it is not; the two are never mixed silently.

| # | Item | Tier | Why — and how strong the claim is |
|---|---|---|---|
| OPT12 | **SROA** — scalar replacement of aggregates | MIR | `Mem2Reg` promotes a whole alloca or nothing; SROA splits a struct into its fields first so each becomes SSA. **INFERRED (high confidence): sqlite is struct-dominated**, so a whole-alloca-only promoter leaves most of its locals in memory. Likely the single largest scalar item. |
| OPT13 | **SCCP** + value-range analysis | MIR | Sparse conditional constant propagation subsumes `ConstFold`+`Dce` interleaving and proves branch-reachability facts neither can. ★ **The plan ALREADY DEPENDS ON THIS:** OPT6's aggressive hoist-when-safe is recorded as needing "value-range OPT8+", and it has been waiting on an analysis nobody scheduled. |
| OPT14 | **GVN-PRE** — partial redundancy elimination | MIR | `Cse` removes only FULL redundancy (available on every path). PRE removes expressions redundant on *some* paths — the classic loop-and-branch case ordinary C generates constantly. |
| OPT15 | **Loop transforms** — unrolling + induction-variable simplification / strength reduction | MIR | `Licm` moves invariants OUT; nothing simplifies what stays IN. IV strength reduction and unrolling (with a cost model, heuristics-as-data per this plan's thesis) are the standard next step. |
| OPT16 | **Memory optimizations** — dead-store elimination, load/store forwarding, memset/memcpy idiom recognition | MIR | The alias arc is CLOSED (§0 gate condition c), so the substrate exists and is unused for these. Idiom recognition matters disproportionately for a database engine's buffer code. |
| OPT17 | **Tail-call optimization** (sibling calls) | MIR/LIR | Absent. Turns a self- or mutual-recursive tail call into a jump — also relieves OPT7's recursion refusal, which currently refuses ALL cycles. |
| OPT18 | **Interprocedural analyses over the summary index** — IPCP, dead-argument elimination, attribute inference (`noalias`/`readonly`/`nounwind`), indirect-call promotion | IPA | ⛔ **BLOCKED ON OPT11.** ★ Once summaries exist, inlining stops being the ONLY interprocedural transform. Attribute inference is the highest-leverage of these because every scalar pass above gets stronger with it. Indirect-call promotion matters specifically for sqlite's `sqlite3_io_methods`-style function-pointer dispatch. |
| OPT19 | **Instruction scheduling** (target pipeline model) | LIR | ✔MEASURED: **DSS has no scheduler at all.** Emitted code is in lowering order. On any out-of-order core this costs less than it once did, but it is a real, universally-present component of a production backend and it is wholly absent here. |
| OPT20 | **Register allocation quality** — live-range splitting + rematerialization | LIR | ✔MEASURED: the sqlite build emits `R_SpilledDueToPressure` naming functions spilling **4, 10 and 13** vregs. **Spills are demonstrably happening on the corpus.** OPT8's coalescing is the first half of this; splitting and remat are the rest. |
| OPT21 | **Block placement** — hot/cold splitting, fallthrough and I-cache layout | LIR | Nothing orders blocks for locality today. Cheap relative to its effect on branchy interpreter-shaped code, which is exactly what a SQL VM is. |
| OPT22 | **PGO** — profile instrumentation + profile-guided decisions | feedback | Every inline, unroll and placement decision above is made from a STATIC estimate. The references get real branch and hotness data. ⓘ Distinct from OPT10's autotuner: OPT10 tunes the *heuristics*, PGO feeds the *program's own behaviour* into them. |

| OPT23 | **Target cost model + machine model** — the heuristics-as-data foundation the machine tier is missing | LIR/config | ⛔ **THE REAL NEXT CYCLE AFTER OPT8 — operator ruling 2026-08-26, see §0.4.** ✔MEASURED 2026-08-26: `costModel` and `machineModel` appear in **ZERO** files under `src/` and `src/dss-config/` — this document's own architecture section describes both in the present tense and NEITHER EXISTS. ★ It is a foundation, not a pass: **OPT19 cannot be built at all without `machineModel`** (a list scheduler is ports + latencies or it is nothing), OPT20's splitting/remat needs spill costs to decide anything, and OPT15's unrolling needs a size/benefit estimate. Carries **canonical machine patterns** with it, since a pattern set without a cost to rank alternatives by is a rewrite table rather than an optimizer. ⓘ Not a blocker for OPT8 itself: ✔lane G's peephole R1 was checked and is unconditionally profitable (a schema identity test against `registerClassOps[].move`), so it needs no cost input — which is why the ruling schedules this AFTER lane G rather than pausing it. |

### Sequencing, and why this order

1. **OPT11 first** (in flight) — it is the substrate for OPT18 and it removes the serial merge that makes every
   future whole-program pass expensive.
2. **OPT8 next** (in flight) — the machine tier is where the measured runtime gap concentrates, and coalescing
   is the cheapest real win there.
3. **THEN OPT23 — AND THIS IS A SCHEDULED CYCLE, NOT A ROADMAP ENTRY** (operator ruling 2026-08-26, §0.4: *"make the next a REAL next cycle after OPT8 lane G, don't forget"*). The cost model + machine model land in the cycle that follows lane G's OPT8, before any further machine-tier work, because OPT19/OPT20/OPT15 each read them and none can be built honestly without them.
4. **Then OPT12 / OPT13** — SROA and SCCP are the two scalar items that make every *later* pass stronger, and
   OPT13 additionally discharges a dependency OPT6 has been carrying unscheduled.
5. **OPT19–OPT21** are a coherent machine-tier arc and are best taken together with a runtime benchmark in the
   loop, since none of them is provable except by measurement.
6. **OPT22 last** — a profile is only worth collecting once the decisions it feeds exist.

⚠ **THE BAR IS UNCHANGED AND APPLIES TO EVERY ROW.** Each pass is heuristics-as-data (this plan's load-bearing
thesis — thresholds and pipeline order live in config, never as constants in the engine); each is
source/target/format agnostic; each ships with a runnable corpus example carrying a `release` arm; and each
gets a negative pin, because **an optimizer bug is a silent miscompile by construction**.
★ And the neutrality test differs by tier: a pass that changes emitted code CANNOT be pinned by artifact md5
equality — it is pinned by run-to-run determinism plus a passing corpus (`examples/c/**`), the standard
established for the OPT11 arc in §0.2.

## 0.4 ★★★ THE OPERATOR'S OPT8-SCOPE RULING — OPT8 IS TWO THINGS, AND OPT23 IS A REAL NEXT CYCLE (2026-08-26, cycle P36)

The operator asked whether OPT8 was covering five items — peephole, coalescing, target cost model,
canonical machine patterns, and groundwork for RA/scheduling. ✔MEASURED answer: **two of the five.**

| item | where it lives | state as measured 2026-08-26 |
|---|---|---|
| peephole | **OPT8** | in flight, cycle P36 lane G |
| register coalescing | **OPT8** | in flight, cycle P36 lane G |
| target cost model | **OPT23** (new) | `costModel` in **ZERO** files under `src/` and `src/dss-config/` |
| canonical machine patterns | **OPT23** (new) | absent |
| groundwork for RA / scheduling | **OPT19** + **OPT20** | no scheduler file exists at all; coalescing is only the first half of OPT20 |

★ **THE LABEL WAS OVERLOADED THREE WAYS IN THIS DOCUMENT**, which is why the question had no clean
answer, and that was a real defect rather than a wording preference:

1. the scope row said **peephole + coalescing** (this is the true OPT8, and it is what lane G is building);
2. the architecture sections said **instruction scheduling** — `pass_schedule.cpp`, `pass_lir_schedule`,
   and the dependency arrow `OPT7 (inlining) ─► OPT8 (scheduling)`;
3. OPT6's deferred aggressive hoist was parked on **"value-range OPT8+"**, which is a third thing again.

⇒ (2) is retargeted to **OPT19**, (3) to **OPT13**, and OPT8 now says out loud that it is exactly two
things. ⓘ The cycle-11 historical records that use the old spelling are left AS WRITTEN — a plan must
not edit its own history to tidy a label — and this table is the map for reading them.

**THE RULING, verbatim:** *"Option 1, but make the next a REAL next cycle after OPT8 lane G, don't forget"*.

So **OPT23 is scheduled, not shelved.** It is the cycle that follows lane G's OPT8, ahead of every other
machine-tier item, and the sequencing list above says so in the place a future cycle will actually read.
⚠ The failure this guards against is the one this plan has already suffered once: OPT6's aggressive hoist
has been waiting on a value-range analysis **nobody ever scheduled**, recorded only as a dependency inside
another row's prose. A dependency named in prose and absent from the sequence is a dependency that never
happens.

★ **WHY IT IS A FOUNDATION AND NOT JUST ANOTHER PASS:** OPT19 cannot be built at all without
`machineModel` — a list scheduler is ports and latencies or it is nothing — and OPT20's live-range
splitting and rematerialization have no basis for a decision without spill costs. This document's own
architecture section already specifies both, in the present tense, for a `machineModel` that does not
exist. OPT23 is building what this plan already claims to have.

ⓘ **Why lane G was NOT paused for it**, which was the third option considered and rejected on a
measurement: lane G's peephole rule R1 is redundant-copy elimination gated on a schema identity test
against `registerClassOps[].move`, and deleting a register's full-width copy of itself is unconditionally
profitable — there is no cost trade-off to get wrong. ✔It was checked before the decision rather than
assumed. Later peephole rules and coalescing's spill decisions WILL want OPT23, which is exactly why it
is next rather than eventual.

## 0.5 ★★★ THE OPERATOR'S OPT11 IMPORT-EDGE RULING — THE LAZY EDGE, AND WHY IT IS NOT A PREFERENCE (2026-08-26, cycle P36)

**RULING: the LAZY EDGE.** The per-TU optimizer calls `SummaryIndex::definitionOf` when its own gate
wants a body it does not have. The two eager policies — **A** depth-bounded closure at K = the Inlining
fixpoint's `max`, and **B** ThinLTO-style per-TU instruction-budget decay — are rejected. Both remain
reachable in config; neither is the architecture.

### Why A's one virtue is its worst property

A's pitch was that it invents no constant — it reuses the Inlining fixpoint's own `max` from the release
pipeline document. ⚠ But that `max` has just been MEASURED to be **truncating** (see
[[D-OPT-INLINING-FIXPOINT-TRUNCATES-BEFORE-CONVERGING]]: splices 1643 → 646 → 225 → 408, identical across
four independent runs, capped at 4 while the curve is still moving). **So A does not reuse a principled
bound — it INHERITS A KNOWN ARTIFACT and promotes it from a pipeline cap to an ARCHITECTURAL import
bound, where it is far harder to dislodge.** The finding is not context for the fork; it is the argument
against A.

### The categorical difference — A and B PREDICT; the lazy edge does not have to

A bounds by structure, B bounds by cost, and **both are PREDICTIONS of what the optimizer will want,
computed before it runs**. ★★★ And the prediction is made against **a call graph that does not exist
yet**: any eager import set is computed on the PRE-optimization graph, while the entire reason the
pipeline iterates is that each pass exposes call sites the previous one could not see. A callee first
exposed mid-fixpoint by `ConstFold`/`CopyProp` is a splice the merged module makes today and an eager
index would MISS — not a corner case, but **the mechanism the fixpoint exists for**. ⇒ The measured
**225 → 408 rise is direct evidence that the wanted-callee set GROWS during the fixpoint**, so a closure
taken before iteration 1 is blind to it BY CONSTRUCTION. A and B do not merely lose quality at the
margin; they cannot see the thing being optimized for.

### One fact, one owner

Under A/B there are TWO authorities: `inlineLegalityGate` decides what may be inlined, and the import
policy decides what is even AVAILABLE to consider. When they disagree the gate is **SILENTLY STARVED** —
it never sees the candidate, so it never declines it, so nothing reports anything. **A quality loss with
no diagnostic**, which is the failure class this project treats as worst. Under the lazy edge there is
ONE authority: the import set is a CONSEQUENCE of the gate, derived rather than declared, and the two
cannot drift because there is nothing to drift from. ★★ That is also what makes the remaining tunable
honest — **prefetch depth affects LATENCY ONLY**, and a knob that cannot change the result is the only
kind that belongs in config for this decision. It is the "no trade-off" property of §0.2 obtained **by
construction rather than by tuning**.

### ✔MEASURED 2026-08-26 — the cost objection to lazy EVAPORATES

The operator required this measured before the fork could be priced, and it inverts the pricing:

| claim | measurement |
|---|---|
| `.dss.mir` body encoding exists | **NO** — the only two occurrences in `src/` are COMMENTS describing the design |
| `FunctionCloner` clones a SUBSET | **NO** — it exists only on the whole-module merge path (`mir_merge.cpp`, `mir.cpp`) |
| the summary index is wired into the pipeline | **NO** — `SummaryIndex` / `buildModuleSummary` have **ZERO call sites outside `src/mir/summary/`** |
| `mergeCuMirs` is still the only cross-CU path | **YES** |

⇒ **Branch (i) holds: ALL THREE options need the transformation half.** `.dss.mir` encoding,
`FunctionCloner` over a subset, and the per-TU parallel optimize driver are demanded by **OPT11's
destination architecture**, not by the lazy policy sitting on top of it. **A does not "ship today"** —
its default is a policy field on a struct nothing consults. The cheap-option framing was false, and A is
the same work plus a worse bound. (Branch (ii) — "A ships without it, therefore A is not the per-TU
architecture at all" — is REFUTED, not selected.)

### Why this beats the policy LLVM actually ships, and why that is not arrogance

B is ThinLTO's real policy. ★★★ **ThinLTO's eager import is forced by a build-system constraint DSS does
not have:** the thin-link phase must fix each backend job's inputs UP FRONT so those jobs can be
dispatched independently, potentially across machines and a distributed cache. **The budget is a
compromise with that dispatch model, not a claim about optimization quality.** DSS optimizes in one
process against a shared in-memory index, so it can take the design that constraint forced LLVM to give
up. ⚠ **The consequence, named now so it is a known trade and not a later surprise:** if DSS ever wants
distributed or cached per-TU builds, lazy fetch means a job's inputs are not known ahead of time. The
door is not closed — **the fetch SET is recordable after the fact**, so a cache key can be computed
post-hoc and a distributed mode could replay a recorded set — but that is future work, written down here
rather than discovered later.

### ★★ THE INVARIANT THAT MAKES "NO TRADE-OFF" TESTABLE INSTEAD OF ASSERTED

> **THE OPTIMIZED OUTPUT MUST BE BYTE-IDENTICAL FOR ANY PREFETCH DEPTH.**

Run the corpus at prefetch **0** and at prefetch **8**; assert the emitted artifacts match. If they
differ, prefetch has **LEAKED INTO QUALITY** and the design's central property is broken — and that is
exactly the failure mode that would otherwise be invisible, because both runs would be green and merely
DIFFERENT. This is the red-on-disable of the whole design, it is cheap, and it is what separates
"no trade-off by construction" from a claim about intent. ★ Pair it with the correctness anchor for the
whole port: **prefetch 0 must produce the SAME output as today's merged module** for the cases the merged
module handles.

### Mechanics that must be right — reuse, do not invent

- **THE INDEX IS FROZEN FOR THE DURATION OF A ROUND.** If one TU's optimization could mutate what another
  reads, results become order-dependent under parallelism — and order-dependence in an optimizer is a
  **nondeterministic build**, which is worse than a quality loss because it cannot be reproduced. Freeze,
  then fan out.
- **RECURSION / CYCLES: reuse the proven guard, do not mint a second one.** ✔`forEachDescriptorInClosure`
  (`src/ffi/shipped_lib_descriptor.cpp`) is a single DFS keyed on the weakly-canonical descriptor path
  (`descriptorPathKey`, the same key the semantic `readDescriptors` dedup and `cachedDescriptorJson`
  use), with a visited set that admits a path AT MOST ONCE — so a cycle A→B→A stops at the second A and a
  diamond's shared leaf is visited once. That is the pattern the on-demand fetch adopts.
- **Bound the fetch by the SAME fixpoint `max` the gate uses**, so there is still one owner — and note
  that this `max` is the number §0.5 above says is currently wrong. **Fix it where it lives; never
  compensate for it here.**

### Measure / invert — the standing order for the next OPT11 cycle

1. ✔**DONE 2026-08-26** — the §4 re-pricing above. Branch (i).
2. **Characterize the 3→4 rise with `max` raised, BEFORE this plan or any report quotes 2,922 again.**
   If the curve converges at 6 or 8, both the bar AND A's inherited bound move.
3. **If `SummaryIndex::definitionOf` cannot serve a body without materializing the whole module**, lazy's
   per-fetch cost is not what it looks like — say so, because that turns the prefetch story from "latency
   tuning" into something with a real floor.
4. **If the corpus shows prefetch 0 and prefetch 8 differing on day one, STOP and report it.** That is the
   design's own alarm, and it is more informative than any benchmark number.

### Cycle-by-cycle log

Cycle-by-cycle progress tracker. Each row = one landed cycle with its commit + closure status + headline deliverable. Newest at top.

| # | Cycle | Headline | Commit(s) | Status |
|---|-------|----------|-----------|--------|
| 11 (OPT6 trap-safe pin) | **D-OPT6-LICM-TRAP-SAFE-HOIST runtime negative pin** ⓘ *(historical record of cycle 11, left as written; note that `isTrapEligible` was renamed `mayFaultWhenSpeculated` and unified with `Load` on 2026-08-11 — grep the new name, and see [[D-OPT6-LICM-SPECULATIVE-LOAD-HOIST]])* | New corpus fixture `examples/c-subset/licm_trap_safe_hoist/` — a 0-trip-count loop with a loop-invariant, unconditionally-dominating, trap-eligible `num / d` (d=0). Correct LICM keeps the SDiv in the body → exit 0; a regressed LICM hoists it to the preheader (which runs even at 0 trips) → `100/0` → `#DE`. **Regression-proven**: with `isTrapEligible` removed the `licm-after-mem2reg` arm crashes `0xC0000094` vs baseline 0; restored → all arms exit 0. Retires the cycle-10j HARD STOP (pin was unconstructible pre-Arc-A). Vacuity caught + fixed mid-cycle (inline `100` literal → named local `num` so the SDiv is genuinely invariant). **Net src diff ZERO** — the `isTrapEligible` gate already existed + is anchor-cited. Conservative-safe default now PINNED; aggressive hoist-when-provably-safe stays deferred behind value-range/OPT8+. Focused review: solid as-is, no FOLD-NOW. AGNOSTICISM CLEAN (corpus data only). | `Cycle 11` (this commit) | ✅ **CLOSED 2026-06-04** (193/193 ctest) |
| OPT7-prep 10p–10r | **Arc A — c-subset integer division codegen (idiv/div) end-to-end** | **10p** `cb3ac20`: config-driven implicit-register-constraint substrate (`ImplicitRegisterConstraint` per-opcode; loader name→ordinal resolution; 7 reject arms). Ships UNCONSUMED (pre-10q invariant pin). **10q** `bf7c7f8`: SDiv/UDiv codegen — compound opcodes + regalloc `implicitRegisters.clobbered` consumer + FLAG 1/2 guards. **10r** `50a07ab`: the cycle-10q compound `[48 99 48 F7]` encoding silently superseded the encoder's auto-REX → lost REX.B for R8–R15 divisors → `STATUS_INTEGER_DIVIDE_BY_ZERO`; fixed by splitting into 4 single-instruction opcodes (`cqo`/`idiv_op`/`xor_rdx_zero`/`div_op`) so each gets correct auto-REX; `DivSlotPair` named-aggregate; `outputs ⊆ clobbered` loader invariant (exposed + fixed a latent under-declared RAX clobber). Closes `D-CSUBSET-DIVISION-OP-CODEGEN` + `D-CSUBSET-DIVISION-CORPUS-MATERIALIZE-BUG`; **unblocks `D-OPT6-LICM-TRAP-SAFE-HOIST`** (idiv `#DE` now observable). 7-agent fold, 10 FOLD-NOW. AGNOSTICISM CLEAN (`lowerDiv` schema-driven; mnemonics in JSON). | `cb3ac20` + `bf7c7f8` + `50a07ab` | ✅ **CLOSED 2026-06-04** (192/192 ctest) |
| OPT7-prep 10h–10o | **Alias arc capstone + c-subset substrate + LICM/CSE hardening** (autonomous-run series 2026-06-04) | **10h** `73ed927`: deref smoke (`deref_load`/`deref_store` fixtures) + `long`→I64 in c-subset.lang.json + I32→I64 sext encoding `movsxd r64,r/m32 = REX.W 0x63 /r` in x86_64.target.json + smoke fixture `long_primitive_smoke` with `0 - 1` discriminating sentinel (post-fold) + GCC CI fix for cycle-10g's `ASSERT_EQ`-in-non-void-helper (refactored to `[[nodiscard]] bool`); 7 FOLD-NOW items applied. **10i** `d222f20`: D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT — re-scoped from char-punning (contradictory under c-subset's correct `charTypesAliasAll=true`) to strict-aliasing arm via `int*` + `long*` distinct non-char primitives. Corpus fixture `examples/c-subset/strict_alias_cse_capstone/` (two-armed `optimizedPipelines`: `[Mem2Reg, Cse]` + full release-like, exit 10 both arms) + MIR-tier `Optimizer.EffectivenessAliasArcStrictTBAACapstone` (3 ARMs: Identity / `[Cse, Dce]` / shipped release with explicit "release.pipeline.json contains Cse" pin). **10j** `1f5140f` + `35cc798`: hard-stopped D-OPT6-LICM-TRAP-SAFE-HOIST per autonomous-loop guardrail (negative miscompile pin requires SDiv codegen — anchored `D-CSUBSET-DIVISION-OP-CODEGEN`); CLOSED D-OPT6-LICM-CHAINED-INVARIANTS via per-loop fixed-point iteration (monotone-growing `hoistedInThisLoop` set; iter cap structurally-derived in 10o from `sum blockInstCount(loop.body) + 1`) + silent-miscompile negative regression test (`ChainedInvariantsRejectedWhenSiblingOperandIsLoopVariant`). **10k** `a28567c`: D-CSUBSET-LOCAL-INT-CODEGEN-NEGATIVE-PIN closed — new `tests/test_support/mutate_target_schema.hpp` helper `mutateShippedTargetSchemaJson(targetName, removeMnemonics)` (JSON-mutation beats parallel-broken-file or internal-shape coupling); 3 attribution pins guard helper-regression silent-pass; `findShippedConfig` gained `DSS_EXPORT`. **10l** `7629f5f`: LICM ambiguous-preheader silent `continue` upgraded to Info-severity `X_OptPassSkipped` citing D-OPT6-LICM-PREHEADER-INSERTION (bridge until full preheader-insertion lands). **10m** `a9590b8`: D-CONFIG-DIAGNOSTIC-CODE-PER-KIND — added `C_InvalidTargetName=0xC035` + `C_InvalidFormatName=0xC036`; 3 `findShippedConfig` callers routed per-kind. **10n** `e6c6453`: doc cleanup (test comments aligned with 10m). **10o** `14ff9e1`: 3-audit FOLD-NOW — LICM iter cap structural-derived (was arbitrary literal 64, would have falsely aborted >64-link chains); capstone ARM 2 tightened to `mutationCount[Cse] == 1u` (was `>= 1u`); capstone Add-operand-identity pin (`addOps[0].v == addOps[1].v` proves CSE substitution direction, not just "a Load disappeared"). **CI fix** `abb9a4c`: missing `#include <format>` in licm.cpp (10l's `std::format` usage) + defensive `<span>` in target_schema.cpp — both unblocked macOS clang + Linux GCC CI which had broken on 10l. **Plan alignment** `ec15e0c0`: plan 22 §3.1 rows for capstone + chained-invariants flipped to ✅ CLOSED. **AGNOSTICISM CLEAN throughout** — all changes target-blind MIR-tier or pure JSON config. | `73ed927` + `d222f20` + `1f5140f` + `35cc798` + `a28567c` + `7629f5f` + `a9590b8` + `e6c6453` + `14ff9e1` + `abb9a4c` + `ec15e0c0` | ✅ **CLOSED 2026-06-04** (191/191 ctest) |
| OPT5+ c3 | **General block-merge + StructCfMarker re-derivation** | `src/opt/passes/simplify_cfg.{hpp,cpp}` extended with block-merge. For `(P, B)` where `P.terminator == Br(B)`, `preds[B] == {P}`, B has no Phi, B.instCount > 1, and ≤1 of {P.marker, B.marker} is non-Linear: B's non-terminator insts inline into P; B's terminator becomes the merged block's; B is elided. **Substrate**: new `MirRebuildPolicy::absorbSuccessor` hook (default nullopt); rebuilder phase-2 walks absorb chain (`while (auto next = absorbSuccessor(currentSource))`); phase-3 phi-incoming flush routes `inc.pred` through `redirectBlockTarget` so absorbed-pred slots redirect to absorb head; phase-3 blockMap-miss-after-redirect is fail-loud (was silent continue — could drop one phi edge). **Marker re-derivation** (closes `D-OPT4-1` single-non-Linear-per-chain) *(this incremental mechanism was SUPERSEDED + DELETED 2026-06-12 by the D15 cycle-B `rederiveStructCfMarkers` canonical derivation — historical record)*: `onBlockBegin` walks chain; if head's source marker is Linear, scans for chain's unique non-Linear marker + setBlockMarker overrides. `chainNonLinearMarker_` tracks per-head state at analyze time; admissions that would push a chain to 2 non-Linear markers are rejected. **Out-of-scope** (anchored `D-OPT4-1-NON-LINEAR-MARKER-MERGE`): both-sides-non-Linear at admission. **`mapOperand` helper** extracted in mir_rebuild_helper — eliminates 6× `substituteOperand(rewriteOperand(substituteOldOperand(...)))` repetition. **7-agent fold (8 FOLD-NOW items)**: 2 cap-exhaustion silent returns → fail-loud (silent-failure CRITICAL); jumpThreadMap_/absorbedToHead_ disjointness invariant fail-loud (HIGH — exclusion violation would silently miscompile); phase-3 phi-incoming blockMap-miss → fail-loud (HIGH); chain-non-Linear invariant tracking (HIGH — multi-non-Linear chain silently drops marker info); `absorbedBlocks_` set dropped (single source of truth in `absorbedToHead_` keys); `mapOperand` extracted; tighter Phi-pred pin in `PhiIncomingRedirectedAcrossMerge` (asserts EXACT redirect-head match); `ChainNonLinearMarkerRejectsSecondAdmission` test added. 8 new block-merge tests in test_simplify_cfg.cpp. **AGNOSTICISM AUDIT: CLEAN**. 185/185 ctest. | `738413d` | ✅ **CLOSED 2026-06-03** |
| OPT6 c1 | **LICM (Loop-Invariant Code Motion) + SimplifyCFG marker-preservation pins** | `src/opt/passes/licm.{hpp,cpp}` + `PassId::Licm = 7`. Natural-loop detection via back-edges (`mirNaturalLoops` in `mir_dom.hpp`); hoist pure invariants (operands all defined outside loop body) to unique non-back-edge preheader. Out-of-scope (anchored): chained invariants / preheader insertion / trap-safe hoist / Load aliasing. **Substrate**: new `MirRebuildPolicy::onBlockBeforeTerminator` hook (closes `D-OPT-MIR-REBUILDER-ONBLOCKBEFORETERMINATOR-HOOK`); `mirNaturalLoops` helper (deterministic — body + loops sorted; header-in-body + no-gaveUp-in-body fail-loud invariants); `recordHoist` single-writer chokepoint. **7-agent fold + 2nd-look (post-cycle)**: deterministic sort (silent-failure HIGH); `!opBlock.valid()` defensive guard (HIGH); `recordHoist` single-writer (type-design); body invariant assertion; 3 new tests including `InvariantHoistLandsInPreheaderNotLoopBody` (verifies placement). **2nd-look surfaced 2 CRITICAL bugs**: nested-loop double-record aborted on legitimate input (closed `D-OPT6-LICM-NESTED-LOOP-DEDUP`); `mirNaturalLoops` admitted gaveUp blocks into loop body → SSA-soundness silent miscompile (closed `D-OPT6-LICM-GAVEUP-BODY-FILTER`). **STRENGTHENS 96bb941**: 2 new marker-preservation pins in test_simplify_cfg make D-OPT4-1 REAL. 185/185 ctest. | `164a1ca` + `93362f1` | ✅ **CLOSED 2026-06-03** |
| OPT5+ c2 | **SimplifyCFG (branch-fold + empty-block jump-thread) + pipeline-level fixed-point loop** | `src/opt/passes/simplify_cfg.{hpp,cpp}` + `PassId::SimplifyCfg = 6` + release pipeline gains `maxIterations: 4`. Branch-fold: `CondBr(Const(true\|false), T, F)` → `Br(T\|F)`. Empty-block jump-thread: trampoline B with `Br(S)` + S has no Phis → elide B; preds redirect to S via path-compressed `jumpThreadMap_`. 2 new `MirRebuildPolicy` hooks (`redirectBlockTarget` + `tryRewriteTerminator`); fail-loud `mapSucc` replacing `blockMap_.at`. **Pipeline fixed-point loop** (closes `D-OPT-FIXED-POINT-LOOP` + `D-OPT1-PASS-RUN-MAX-ITER`): `OptPipeline.maxIterations: uint8_t` (range [1, 32]); engine reruns passes until convergence OR cap; `OptResult.fixedPointReached` default `true → false` (pessimistic). **7-agent fold**: fail-loud `mapSucc` (silent-failure CRITICAL); `recordTerminatorInRewrite` override false (HIGH); fixedPointReached default flip (HIGH); `elided_` set dropped; uint16→uint8; 5 new engine/loader tests; **AGNOSTICISM CLEAN**. 9 SimplifyCFG unit tests. 184/184 ctest. | `96bb941` | ✅ **CLOSED 2026-06-03** |
| OPT5 c1 | **CSE / GVN — dom-tree-scoped value numbering** | `src/opt/passes/cse.{hpp,cpp}` + `PassId::Cse = 5` + release pipeline becomes `[Identity, ConstFold, Mem2Reg, CopyProp, Cse, Dce]`. **Algorithm**: iterative dom-tree DFS with Visit/Leave frame stack + scoped hash table whose entries are rolled back on Leave. For each non-side-effecting non-Phi non-Load non-Volatile non-terminator non-Alloca inst: build `CseKey{opcode, type, sorted-canonical-operand-VNs, payload}` (operands resolved transitively through `cseMap_` first; sort if `isCommutative(op) && operands.size() == 2`). Lookup → redirect via `cseMap_[id] = canonicalId`; miss → insert. Path-compress + verify post-DFS. Same pre-analyze + `substituteOldOperand` substitution shape as CopyProp — dead duplicates survive rebuild → DCE sweeps. **Substrate**: new `src/mir/mir_opcode.hpp::isCommutative` constexpr predicate (closed switch over Add/Mul/And/Or/Xor/FAdd/FMul/ICmpEq/ICmpNe/FCmpOeq/One/Ueq/Une/VAdd/VMul); new `src/opt/passes/path_compress.hpp` shared `resolveTransitive` + `pathCompressAndVerify` template helpers (extracted from CopyProp + CSE — single source of truth for the chain-walk + cycle-fail-loud + post-compression invariant assertion). **D-OPT1-CSE-NONCOMMUTATIVE-PIN CLOSED** via `examples/c-subset/cse_noncommutative/`'s `cse-noncommutative-pin` differential arm (`[Mem2Reg, Cse]` → exit 58, NOT the buggy 70 a permissive predicate would produce on `(a-b)*2 + (b-a)*3 + 60`). `cse_candidate` gains 3 arms (`cse-only`, `mem2reg-cse`, `mem2reg-cse-dce`). **11 unit tests** in `tests/opt/test_cse.cpp`: SameBlockArithmeticCsed / CommutativeOperandsCanonicalized / NonCommutativeSubtractionNotMerged / SideEffectingStoreNotCsed / LoadNotCsed / VolatileBinaryOpNotCsed / DominanceScopingDiamondNoCrossArmMerge / DominanceScopingEntryDefAvailableInChild / MultiFunctionModuleEachCsedIndependently / RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo / ResultTypeDiscriminatesKey / TransitiveChainResolves. **7-agent fold (FOLD-NOW items)**: defensive `op == MirOpcode::Alloca` exclusion (silent-failure HIGH — pins against future opcode-table cleanup that flips Alloca's `hasSideEffects` flag); boost-style `hashCombine` replacing XOR-of-shifts (type-design FOLD-NOW — kills trivial collision class on adjacent MirInstIds where `std::hash<uint32_t>` is identity); `LogEntry` collapsed to `vector<CseKey>` (type-design FOLD-NOW — `hadPrior` flag was structurally dead since inserts only run on lookup miss); shared `path_compress.hpp` helper extracted (simplifier FOLD-NOW — ~50 LOC dedup across CopyProp + CSE; CopyProp refactored to use it); 2 new tests (ResultTypeDiscriminatesKey + TransitiveChainResolves) — closes pr-test-analyzer's Important Gaps 1 + 3; isCommutative comment tightened (drops literal-expression statement). **AGNOSTICISM AUDIT: CLEAN** — substrate is MIR-vocabulary-only. **Plan 22 §0.1 OPT-tier matrix**: OPT5 → `🟦 c1 DONE (SimplifyCFG + LICM next)`. 182/182 → 183/183 ctest (+1 test_cse with 11 tests). | `ec7220b` | ✅ **CLOSED 2026-06-03** |
| OPT4 c3 | **SSA copy-prop — trivial Phi-collapse** | `src/opt/passes/copy_prop.{hpp,cpp}` + `PassId::CopyProp = 4` + release pipeline becomes `[Identity, ConstFold, Mem2Reg, CopyProp, Dce]`. **Substrate**: new `MirRebuildPolicy::substituteOldOperand(MirInstId oldOp) → MirInstId` hook (pre-rewrite, OLD-id space) for redirecting collapsed-Phi uses to their target before `rewriteOperand` maps old→new. All 6 rebuild-helper operand-resolution call sites wrap as `substituteOperand(rewriteOperand(substituteOldOperand(o)))`. Existing `substituteOperand` (post-rewrite, NEW-id space) tightened in docs — reserved for the future general operand-substitution variant (D-OPT-COPYPROP-BLOCK-CURSOR). **Shared `cloneGlobalsOrCarveOut` helper** extracted from ConstFold/Dce/Mem2Reg/CopyProp into `mir_rebuild_helper.{hpp,cpp}` — single-source-of-truth for the runtime-init globals carve-out + global-clone prelude (was 4× duplicated; now once). DCE keeps its own carve-out because its global-clone loop is filtered by the live-symbol BFS. **Algorithm**: worklist over all Phis in the function; for each Phi P compute distinct non-self incomings (after transitive resolution through the in-progress collapse map); if exactly 1 distinct value V, record `collapseMap[P] = V`. Reverse-use map (`phiUsers[inc.value] → [Phis using it]`) drives worklist re-evaluation when P collapses. **Path compression** post-worklist so every target is a non-collapsed value; **post-compression invariant assertion** fail-loud if any target still appears as a key (silent-failure CRITICAL closure). `unordered_map<MirInstId, MirInstId>` (strong-id keys; hash via `DSS_HASH_ID(MirInstId)` in `strong_ids.hpp`) — eliminates the manual `.v`/arenaTag round-trip that was a latent cross-arena bug surface. **Dead Phis stay**: collapsed Phis survive the rebuild (no `shouldEmit=false`); they're DCE-swept in the subsequent pipeline stage. **The 2 user-mandated rigor pins**: (a) post-compression chain invariant + fail-loud (closes silent-miscompile path via load-bearing path-compression contract); (b) explicit comment on `distinct.size() != 1` invariant excluding the size-0 mutual-Phi cycle case (P1↔P2 with no external in-edge — both filter to size 0 → leave uncollapsed for DCE). **Tests**: 8 unit tests (DiamondSameValue / DiamondDifferent / TransitiveChain / LoopHeaderSelfRef-collapses / LoopHeaderTwoDistinctIncomings-preserved / SingleIncoming / MultiFunctionModuleEachCollapsedIndependently / RuntimeInitGlobalsModuleEmitsXOptPassSkippedInfo). **Corpus**: new `examples/c-subset/phi_collapse_same_value/` (canonical Phi-collapse witness — both arms write the same value to `y`; exit 42; 2 differential arms = `mem2reg-then-copyprop` + `constfold-mem2reg-copyprop`). `copy_prop_across_join` gains `copyprop-after-mem2reg` arm (no-op idempotency on a non-collapsible 2-distinct-incoming Phi). **7-agent fold (FOLD-NOW items)**: post-compression invariant assertion (silent-failure CRITICAL); explicit cycle-case comment (silent-failure HIGH); strong-id map keys (type-design HIGH); shared `cloneGlobalsOrCarveOut` helper (~90 LOC saved across 3 passes; simplifier FOLD-NOW #1); 2 new tests (multi-function + runtime-init carve-out parity); rename `LoopHeaderPhiTwoDistinctNonSelfNotCollapsed` → `LoopHeaderPhiTwoDistinctIncomingsNotCollapsed` (test fixture has no self-ref; clarification); temporal-provenance + stale-future prose stripped from `phi_collapse_same_value/main.c` + `mir_rebuild_helper.cpp:132-133`. **AGNOSTICISM AUDIT: CLEAN** — substrate is MIR-vocabulary-only. **Plan 22 §0.1 OPT-tier matrix**: OPT4 → `🟦 c1+c2+c3 DONE (OPT5 CSE/GVN next)`. 180/180 → 182/182 ctest (+1 test_copy_prop with 8 tests, +1 phi_collapse_same_value example). | `f496aea` | ✅ **CLOSED 2026-06-03** |
| OPT4 c2 | **Mem2Reg — Cytron-Ferrante SSA construction** | First non-trivial promotion pass. `src/opt/passes/mem2reg.{hpp,cpp}` + `PassId::Mem2Reg = 3` + release pipeline becomes `[Identity, ConstFold, Mem2Reg, Dce]`. **Substrate additions**: `MirRebuildPolicy::onBlockBegin(oldB, newB, dst, rewrite, blockMap)` hook (closes `D-OPT-MIR-REBUILDER-ONBLOCKBEGIN-HOOK`); `MirFunctionRebuilder::{blockMap,rewriteMap}()` public accessors so a pass that inserts NEW SSA defs can resolve old→new ids when wiring phi incomings. `mir_dom.hpp` gains `mirDomTreeChildren` (idom inversion, O(V)) + `mirIteratedDominanceFrontier` (Cytron-Ferrante worklist over `mirDominanceFrontier`'s output). **Pass shape**: promotability gate (closes `D-OPT-MEM2REG-PROMOTABLE-ALLOCA-SCAN`) walks reachable blocks, classifies every alloca use; only `Load[op0]` / `Store[op1]` accepted, all other uses (Call, Gep, Phi value, Return, terminator operand, Bitcast, …) disqualify; scalar form only (operand count 0 = no array allocas); Volatile flag on alloca / Load / Store disqualifies (observable memory semantics). gaveUp-block gate refuses to promote any alloca whose def-blocks or IDF members touch a `dom.gaveUp` block (verifier-flagged CFG defect). Rename walk is iterative dom-tree DFS via explicit Visit/Leave frame stack (no recursion → no stack-overflow on deep CFGs). Per-alloca `vector<ReachingValue>` stacks with `OldInst` vs `Phi`-marker tagged-union; `loadReplacement_` records the reaching value at each Load; `phiIncomings_` records (reaching value, pred block) pairs for each inserted Phi. Phi-incoming completeness invariant: every PendingPhi's recorded incoming count must equal its block's reachable-pred count (fail loud on mismatch — silent miscompile if a path is unrepresented). **Differential-execution pin**: `examples/c-subset/copy_prop_across_join` gains `optimizedPipelines: [{label: "mem2reg-only", passes: ["Mem2Reg"]}]` — the OS-spawn diff-asserts byte-equal exit + stdout vs baseline (closes the runtime arm of the 4 user-mandated risk pins). **6 Mem2Reg unit tests + 1 dom test addition + 1 pipeline-loader assertion update**: SingleBlock / DiamondPhi / AddressTakenNotPromoted / Mixed (one promoted + one escaped) / **StructCfMarkersUnchanged** (byte-compares pre/post block markers — CFG safety pin) / ArrayAllocaNotPromoted / **LoopInductionVariablePromoted** (end-to-end loop-CFG promotion with 2 incomings at the header) / **AllocaReturnedAsPointerNotPromoted** (escape pin) / **VolatileAccessAllocaNotPromoted** (Volatile-disqualifies pin). `tests/mir/test_mir_dom.cpp` gains LoopCfgDominanceFrontier (closes `D-OPT-DOMFRONTIER-LOOP-TEST` — body's DF contains header on a back-edge; IDF({body})⊇{header}; dom-tree children of header = {body, exit}). **7-agent fold (FOLD-NOW items)**: Volatile gate on Alloca / Load / Store (silent-failure CRITICAL — promoting a Volatile slot erases observable semantics); gaveUp-block gate (silent-failure CRITICAL — `mirDominanceFrontier` skips gaveUp blocks so unguarded Mem2Reg under-inserts Phis → uninit-read silent miscompile); RPO-reachable restriction in analyze() (code-reviewer CRITICAL — selectBlocks=all blocks vs rename-DFS=dom-tree-children diverged on unreachable blocks → misleading abort); iterative rename DFS replacing recursive (silent-failure HIGH-5 — unbounded stack on deep CFGs); Phi-incoming-count invariant (silent-failure HIGH-6 — every inserted Phi must have exactly reachable-preds-of-block incomings, fail loud); dead `phiNewBlock_` field removed (simplifier + type-design convergence — written-never-read; LICM/CSE would otherwise inherit a phantom contract); dead `snapshotMarkers` method removed (test owns its helper); 3 new test pins (loop / Return-escape / Volatile); 2 comment-header rewrites (temporal-provenance violations in mir_rebuild_helper.hpp + test_mir_dom.cpp). **AGNOSTICISM AUDIT: CLEAN** — zero hardcoded language/CPU/format branches; substrate is MIR-vocabulary-only. **Plan 22 §0.1 OPT-tier matrix update**: OPT4 → `🟦 c2 DONE (c3=SSA copy-prop next)`. 180/180 ctest (was 179/179). | `2e89b83` | ✅ **CLOSED 2026-06-03** |
| OPT4 c1 | **Pre-condition refactors for Mem2Reg/CopyProp** | Pure refactor cycle. `mir_dom.hpp` extracted from `mir_verifier.cpp` (Cooper-Harvey-Kennedy idom + tri-state dominates + NEW `mirDominanceFrontier`); same precedent as `mir_cfg.hpp`. `mir_rebuild_helper.{hpp,cpp}` extracted from ConstFold+DCE: `MirRebuildPolicy` virtual base + `MirFunctionRebuilder` concrete class with 7-hook policy interface (`selectBlocks` / `shouldEmit` / `tryRewrite` / `substituteOperand` / `acceptPhiIncoming` / `onZeroPhiIncomings` / `recordTerminatorInRewrite`). ConstFold + DCE refactored to use the shared substrate via `ConstFoldPolicy` + `DcePolicy`. 3 new dominator-tree unit tests (diamond dominance + DF + trivial). **7-agent fold**: restored I_VerifierFailure for out-of-range successor edges (silent-failure CRITICAL — the extraction's docblock claimed `checkStructuralInvariants` caught them, but it didn't; added the check); `mirDominanceFrontier` skips `gaveUp` blocks + aborts loud on step-cap overflow (no silent under-reporting); `MirDomTree::gaveUp` switched from `vector<bool>` to `vector<uint8_t>`; `onZeroPhiIncomings` signature extended with `(oldPhi, oldBlock, oldFn, newPhi)` for debuggable diagnostics; `instructionsSkipped` counter moved from rebuilder to policy (proper layering — counting is policy concern); dead `<algorithm>` include + dead `MirOpcode op` param dropped; provenance + WHAT-comment cleanup per discipline. No new pass landed; Mem2Reg deferred to OPT4 c2 with proper focus (its Phi-insertion shape needs an `onBlockBegin` policy hook that's better designed alongside the consumer than speculatively now). | `256b970` | ✅ **CLOSED 2026-06-03** |
| OPT3 c1 | **DCE + CompileConfig threading + linkage assertion** | First MIR-mutating destructive pass. `PassId::Dce` + 3 reachability layers (inter-procedural live-symbol BFS + per-function CFG RPO + intra-block live-inst worklist). `CompileOptions` struct + `resolvePipelineName` table close D-OPT-COMPILE-OPTIONS-STRUCT + D-OPT1-PIPELINE-CONFIG-FROM-COMPILECONFIG. `release.pipeline.json` now reachable as `[Identity, ConstFold, Dce]`. `mir_cfg.hpp` extracted (verifier ↔ optimizer share `mirReversePostOrder`). 6 new DCE unit tests (happy path + negative-pin shape replicated + runtime-init carve-out emits `X_OptPassSkipped` Info) + 3 linkage pins (exported survives / local elided / binding preserved) + Phi-preservation pin in test_const_fold + 4 CompileOptions/resolvePipelineName trivial pins (defaults, debug/release map, OOR ordinal returns nullopt) + dce_negative_pin's `optimizedPipelines: [{label:"dce-only", passes:["Dce"]}]` arm. **7-agent fold (Critical+High items)**: DCE runtime-init carve-out now emits `X_OptPassSkipped` (closes silent-skip parity gap with ConstFold); LiveSymbolScanner dedupes per-function liveInsts so the rebuild reuses them (no second scan); Phi unreachable-predecessor crash fixed (`blockMap.at` → bounded skip + fail-loud if Phi ends with zero incomings); `resolvePipelineName` returns `optional<string_view>` with caller-side fail-loud on OOR ordinal; `kPipelineNameTable` `kPipelineNameTableInOrder()` swap-drift guard mirroring `kPassNameTable`; dead `fn_` + `symToGlobal_` fields removed; TRAP docblock corrected to match the actual unconditional+conditional shape; temporal cycle-tag phrasing trimmed. | `52c1380` | ✅ **CLOSED 2026-06-03** |
| OPT2 c1 | **ConstFold + JSON pipeline loader + differential-verify** | First MIR-mutating pass. PassId::ConstFold; JSON pipeline loader with 4 new diagnostic codes 0x2002–0x2005; D-OPT1-VERIFY-AFTER-EVERY-PASS live; OptResult shape; PassId single-source `kPassNameTable`; D-OPT1-DIFFERENTIAL-VERIFY-RUNNER with `ArmStatus` tri-state. + 2nd-order fold for 8 audit items (spawn-fail Poisoned / fs::file_size UINT64_MAX gate / X_OptPassSkipped Info / X_OptReturnFalseWithoutDiagnostic split). | `8bae225` + `1a2bed5` | ✅ **CLOSED 2026-06-03** |
| OPT1 c1 | **Optimizer substrate opening** | `src/opt/optimizer.{hpp,cpp}` + closed `PassId` enum (Identity only) + `kPassIdCount`/static_assert drift guard + `X_UnknownPassId=0x2001`. compile_pipeline step 3.5 wired default `{Identity}`. Pre-OPT1 hygiene: anchor-registry CI guard + behavioral encoder review + `symbol_attrs.hpp` (SymbolBinding/Visibility on MIR via `_pad` slot). 5 corpus negative pins: dce(100)/cfold(42)/cprop(10)/licm(68)/cse(58). | `5d29532` + `7d5932e` | ✅ **CLOSED 2026-06-03** |

**Upcoming (highest-priority anchored items, ordered by trigger):**

| # | Cycle | Headline | Trigger | Key anchors |
|---|-------|----------|---------|-------------|
| OPT4 c2 | **Mem2Reg** ✅ **CLOSED 2026-06-03** (commit `2e89b83`). | OPT4 c1 ✅ | `D-OPT-MIR-REBUILDER-ONBLOCKBEGIN-HOOK` ✅ CLOSED, `D-OPT-MEM2REG-PROMOTABLE-ALLOCA-SCAN` ✅ CLOSED, `D-OPT-DOMFRONTIER-LOOP-TEST` ✅ CLOSED |
| OPT4 c3 | **SSA copy-prop — trivial Phi-collapse** ✅ **CLOSED 2026-06-03** (commit `f496aea`). Implemented as Phi-collapse (worklist + path compression on OLD ids). Future general operand-substitution variant (D-OPT-COPYPROP-BLOCK-CURSOR) remains anchored for OPT5+. | OPT4 c2 ✅ | `D-OPT-COPYPROP-PHI-COLLAPSE-CORPUS` ✅ CLOSED, `D-OPT1-COPY-PROP-JOIN-PIN` ✅ runtime arm activated, `D-OPT-COPYPROP-BLOCK-CURSOR` deferred to OPT5+ |
| OPT5 c1 | **CSE / GVN** ✅ **CLOSED 2026-06-03** (commit `ec7220b`). Dom-tree-scoped value numbering; closes `D-OPT1-CSE-NONCOMMUTATIVE-PIN` runtime arm; `path_compress.hpp` shared helper extracted from CopyProp+CSE. | OPT4 ✅ | `D-OPT1-CSE-NONCOMMUTATIVE-PIN` ✅ runtime arm activated |
| OPT5+ c2 | **SimplifyCFG (branch-fold + jump-thread) + pipeline-level fixed-point loop** ✅ **CLOSED 2026-06-03** (commit `96bb941`). Branch-fold `CondBr(Const)`→`Br`; empty-block jump-thread; new `OptPipeline.maxIterations` engine reruns to convergence. **Closes `D-OPT-FIXED-POINT-LOOP` + `D-OPT1-PASS-RUN-MAX-ITER`**. NOT a linear OPT-N step — see the §0 `OPT5+` footnote. | OPT5 ✅ | `D-OPT-FIXED-POINT-LOOP` ✅ CLOSED, `D-OPT1-PASS-RUN-MAX-ITER` ✅ CLOSED |
| OPT6 c1 | **LICM** ✅ **CLOSED 2026-06-03** (commits `164a1ca` + `93362f1`). Natural-loop detection via back-edges; hoist pure invariants to unique non-back-edge preheader. New `MirRebuildPolicy::onBlockBeforeTerminator` hook + `mirNaturalLoops` helper. **2nd-look CRITICAL bugs closed**: `D-OPT6-LICM-NESTED-LOOP-DEDUP` + `D-OPT6-LICM-GAVEUP-BODY-FILTER`. | OPT4/5 ✅ + loop-nest substrate | `D-OPT1-LICM-CONDITIONAL-PIN` ✅ runtime arm activated; `D-OPT-MIR-REBUILDER-ONBLOCKBEFORETERMINATOR-HOOK` ✅ CLOSED |
| OPT5+ c3 | **General block-merge + StructCfMarker re-derivation (single-non-Linear case)** ✅ **CLOSED 2026-06-03** (commit `738413d`). Block-merge for `(P, B)` with single-pred-of-B + no-Phi + non-trampoline + ≤1 non-Linear marker per chain. New `MirRebuildPolicy::absorbSuccessor` hook + `mapOperand` operand-resolution helper. **Closes `D-OPT5-BLOCK-MERGE` + partial `D-OPT4-1`** (single-non-Linear); both-sides-non-Linear deferred at `D-OPT4-1-NON-LINEAR-MARKER-MERGE`. | OPT5+ c2 ✅ + OPT6 c1 ✅ | `D-OPT5-BLOCK-MERGE` ✅ CLOSED, `D-OPT4-1` ✅ partial, `D-OPT4-1-NON-LINEAR-MARKER-MERGE` deferred |
| OPT6+ | **LICM remainder** (independent of OPT7 — parallelizable) | Trap-safe hoist conservative-safe default RUNTIME-PINNED (cycle 11, `licm_trap_safe_hoist/`, regression-proven); aggressive hoist-when-safe still deferred to value-range/**OPT13**. Preheader-insertion needs Phi-merge substrate. NOT an OPT7 blocker. | OPT6 c1 ✅ + Arc A ✅ (10r `50a07ab`) | `D-OPT6-LICM-TRAP-SAFE-HOIST` (safe default pinned; aggressive needs value-range OPT8+), `D-OPT6-LICM-PREHEADER-INSERTION` |
| **PRE-OPT7 CHAIN** ✅ **COMPLETE 2026-06-05 (P1✅ P2✅ P3✅ P4✅) → OPT7 OPENED (user §B, supervised)** (decided 2026-06-04 — full cross-CU Weak-pin scope) | OPT7's §2.9 inline-legality (never inline a `Weak` callee; keep address-taken / externally-visible out-of-line bodies) is a silent-miscompile seam → guardrail #2 needs a genuine negative pin. The Weak hazard ("strong replaces weak **at link time**") is inherently cross-CU, so the real pin needs the full cross-CU stack. Ordered P1→P4 below; together P3+P4 also unblock `D-OPT7-1` so OPT7 is cross-CU from the start. | — | see §3.1 |
| P1 | **Front-end linkage specifiers** — c-subset `static`→`Local` + weak-decl→`Weak` (+ visibility); semantic analyzer attaches linkage to the user-function HIR node (today only FFI externs carry linkage; user funcs default `Global`). **Design-locked cycle 12** (most-complete: `static` + `__attribute__((weak/visibility))`); **BINDING SPECIFIERS LANDED cycle 13** (`static`→Local + `__attribute__((weak))`→Weak: source→HIR→MIR→DCE, regression-proven; visibility-string syntax deferred; unknown-specifier diagnostic ✅ CLOSED cycle 14 — §3.1). | OPT1 linkage substrate ✅ (`symbol_attrs.hpp`) | `D-CSUBSET-LINKAGE-SPECIFIERS` |
| P2 | ✅ **CLOSED cycle 13.** **HIR→MIR linkage mapping** — map HIR `FfiLinkage`/`FfiVisibility` + P1 user-fn linkage → MIR `SymbolBinding`/`SymbolVisibility` at `lowerToMir`; pin `Common`/`Internal` edges. **Lands the single-CU `static`-DCE runtime pin.** Corrects §2.9's "pinned in OPT1" overstatement (mapping was stubbed to `Global+Default`). | P1 | `D-OPT7-LINKAGE-HIR-TO-MIR-MAPPING` |
| P3 | ✅ **DONE in practice (cycle 19)** — CU6's "multiple TUs per image" is delivered by `Program::compileUnits` (N files → N CUs → one merged image) + proven by the runnable `cross_cu_call` (main.c + helper.c → one PE, exit 42). The plan-08 CU6 row is the formal v1.x status flip (stale); the CAPABILITY the OPT7 chain needs EXISTS + is proven. **CU6 — multi-CU compilation units** (plan 08) — multiple TUs per image. | P1 + P2 | plan 08 CU6 → unblocks `D-OPT7-1` |
| P4 | **LK11 — cross-CU linking** (plan 14) — real symbol-table merge + cross-CU resolution + **weak-vs-strong resolution** (strong def wins) + `K_CrossCu*` diagnostics. **Foundation D-LK4-3 ✅ landed 2026-06-04** (compound `(cuId, SymbolId)` linker index + N>1 `K_CrossCuMergeUnsupported` fail-loud); P4 = the symbol-table merge proper on top. **LK11a ✅ (cycle 16): the DEFINITION merge + weak-vs-strong landed → `LinkedImage::resolvedGlobalDefs` (name → winning def), the direct substrate the D-OPT7-WEAK-INLINE-NEGATIVE-PIN queries; reference resolution (extern → sibling def) ALSO landed; **merged bytes ALSO landed (cycle 18 — pre-merge + walker reuse)**; **the multi-CU driver + cross-CU CALL ALSO landed (cycle 19 — `Program::compileUnits` + CLI gcc-semantics dispatch + the rodata thunk slot + PE `.reloc` ASLR base relocations; `cross_cu_call` exit 42)**. **P4 is functionally COMPLETE for the OPT7 Weak-pin: `resolvedGlobalDefs` (weak-vs-strong) exists and is queryable.** LK11b remaining (does NOT block OPT7): ~~ELF/Mach-O exec thunk emission (D-LK11-ELF-MACHO-CROSSCU-THUNK-EMISSION)~~ ✅ closed c154 + extern-import dedup (still open). | P3 (CU6↔LK11 couple) | plan 14 LK11 → unblocks `D-OPT7-1` |
| OPT7 | 🟩 **CYCLE 1 DONE 2026-06-05 (user-supervised) — minimal inliner + §2.9 gate + Weak-pin LANDED.** `PassId::Inlining` + call-graph (DCE's symbol→func map) + a MINIMAL SSA splice (single-block LEAF callees — `Arg(i)`→call operand, `Return`→call result, no CFG merge; SSA-correct, MirVerifier-clean) + the §2.9 legality gate (`inlineLegalityGate`: refuses Weak [the correctness rule] / self-recursion / address-escape / multi-block / non-leaf / IntrinsicCall-leaf / out-of-range-arity; conservative-refuse default). **`D-OPT7-WEAK-INLINE-NEGATIVE-PIN` ✅ CLOSED** — runtime corpus `examples/c-subset/weak_inline_crosscu/` (2-CU weak `f`→7 + strong `f`→42, `optimizedPipelines:["Inlining"]`) exits 42 (gate refuses → linker strong-over-weak), RED-on-disable at 7. Required an emission-tier strong-over-weak completion in `mergeModules` (drop the shadowed weak body; `Linker.CrossCuStrongShadowsWeakEmitsOnlyStrongBody`). New `X_InlineMalformedCallSite=0x2008` (defensive fail-loud). NOT in the shipped release pipeline (no cost model yet — exercised via the explicit arm). 197/197; 3 reviews + self-audit (splice SSA-correct, gate complete, linker-fix drops only the loser). **CYCLE 2 DONE 2026-06-05 (user-supervised) — general multi-block-LEAF splice LANDED** (`D-OPT7-MULTIBLOCK-SPLICE` ✅): `inlineLegalityGate` lifted the `funcBlockCount != 1` refusal but keeps the leaf scan across ALL callee blocks; because the cycle-1 `tryRewrite` hook structurally cannot create blocks, a function with any multi-block target is rebuilt by a new `MultiBlockInliner` — it SPLITS the call-site block (a fresh CONTINUATION block holds after-Call insts), CLONES the callee CFG in RPO (fresh block+inst ids, shared callee→caller `local` map), rewrites each cloned `Return`→`Br(continuation)` collecting (value, cloned-pred) edges, joins them in a RETURN-MERGE PHI in the continuation (elided for a 1-return callee; void callee → no Phi), RE-DERIVES StructCfMarker to `Linear` for every cloned+continuation block (a clone of the callee's `EntryBlock`/`ExitBlock` would violate verifier entry/exit rules; `Linear` is parity-neutral), and redirects caller-`Phi` incomings via `blockExitMap_`. **Robustness gate** added: a leaf callee with NO returning path (every path ends `Unreachable`) is refused — else the continuation is predecessor-less → MirVerifier rejects → an otherwise-valid program becomes a build error under inlining. Proven by `examples/c-subset/multiblock_inline/` (`pick(x){if(x>0)return 7;return 9;}`; `pick(1)+pick(0)`=16; `optimizedPipelines:["Inlining"]` == baseline) + 12 `tests/opt/test_inlining.cpp` pins (incl. merge-Phi value↔pred pairing — `instBlock(value)==pred` per edge — RED-on-disable, and `NonReturningLeafCalleeIsNotInlined`). A callee carrying a `Phi` in any block is STILL conservatively refused → `D-OPT7-MULTIBLOCK-SPLICE-PHI` (sound; frontend-reachable via ternary `?:` / `&&` / `\|\|` → MIR Phi). **USER-DIRECTED OPT7 SEQUENCE (§B 2026-06-05 — "do all in a multi /dss-cycle; cross-CU first, then safety, then the rest"): CYCLE 3 ✅ DONE = cross-CU inlining (`D-OPT7-1` ✅, the cross-CU loop CLOSED across dss-cycles 24 [prereq] + 25 [`D-OPT7-CROSSCU-MIR-MERGE` merge] + 26 [the optimizer runs on the merged module → cross-CU calls inlined; `optimizeModule` chokepoint + the merged-optimize wiring; proven by `CrossCuCallIsInlinedOnMergedModule` + the `cross_cu_call` Inlining arm exit 42 + the `weak_inline_crosscu` Weak negative pin + the G4 sole-weak refusal]) → CYCLE 4 ✅ DONE (dss-cycle 27) — the "safety" slot was found premature as a standalone (leaf-only ⇒ recursion already refused) so it was DELIVERED AS general NON-LEAF inlining + SCC recursion safety: lifted the `Call`-leaf refusal (a callee with a regular `Call` is now inlined; splice unchanged — it already clones inner `Call`s) + a new `src/opt/analysis/call_graph_scc.{hpp,cpp}` (iterative Tarjan) and the gate now REFUSES inlining a call within a recursive call-graph SCC (`D-OPT7-INLINE-LEGALITY-GATE` recursion arm — generalizes self→mutual recursion A→B→A, SUBSUMES the old self-recursion check); termination bounded (one-level-per-run + `maxIterations` + SCC ⇒ recursion expansion = zero); non-leaf is correctness-preserving + OPT-IN (not in release.pipeline.json); proven by flipped `NonLeafCalleeIsInlined`/`MultiBlockNonLeafCalleeIsInlined` (callsInlined==2, RED-on-disable) + `MutualRecursiveCallIsNotInlined` + the standalone `test_call_graph_scc` + `examples/c-subset/non_leaf_inline` exit 41 == baseline → CYCLE 5 ✅ DONE (dss-cycle 28) = cost-model profitability + inline-threshold + ship-in-release: `D-OPT-COMPILE-OPTIONS-VARIANT`-equivalent landed as `OptPipeline.inlineThreshold` (uint32, config-driven via the pipeline JSON, default 50, bounds-checked [1, kMaxInlineThreshold] → X_PipelineMalformed) + the gate REFUSES inlining a callee whose total inst-count > the threshold (`>` strict so at-threshold inlines) + **`Inlining` now SHIPS in `release.pipeline.json`** (index 1, after Identity) — a real -O optimization, no longer opt-in-only; proven by `InlineCostModelRefusesLargeCallee` / `InlineCostModelBoundaryIsExclusiveUpper` / `InlineCostModelZeroThresholdRefusesAll` (all RED-on-disable) + `InlineThreshold{Zero,Overflow,NonInteger}Rejects` loader pins + `ShippedReleaseContainsInlining` → **CYCLE 6 ✅ DONE (dss-cycle 29) = IntrinsicCall inlining relaxation**: the `IntrinsicCall` refusal in `inlineLegalityGate` is REMOVED — a callee containing an `IntrinsicCall` now inlines (single-block + multi-block; SSA-correct via the splice's generic arm — the payload-carried intrinsic id is copied verbatim, operands remap via the local map); the frame-sensitive-intrinsic hazard (va_start / frameaddress must not be inlined) is deferred TRIGGER-GATED (`D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC`) behind the `NoShippedConstructLowersToIntrinsic` fail-loud tripwire (asserts the HIR intrinsic registry stays empty through real compiles — RED the day a frontend emits an intrinsic — so no inline-safety attribute is speculated now); proven by flipped `IntrinsicCalleeIsInlined` (value-threaded) + `MultiBlockIntrinsicCalleeIsInlined` + `VoidIntrinsicCalleeIsInlined` (RED-on-disable). **The user-directed OPT7 sequence (c3–c6) is COMPLETE.** **CYCLE 7 ✅ DONE 2026-06-13 (user-authorized, supervised §B cycle) — callee-Phi inline-splice (`D-OPT7-MULTIBLOCK-SPLICE-PHI` ✅ CLOSED):** the `inlineLegalityGate` callee-`Phi` refusal is DROPPED with NO shape gate; `MultiBlockInliner` now CLONES a callee `Phi` via a DEFERRED-incoming-flush (placeholder `addPhi` in the RPO clone loop; after the whole loop each incoming flushes — VALUE via the shared `local` map, PRED via `calleeBlockMap`, fail-loud `abort` on a pred-not-in-clone-map), the deferral LOAD-BEARING for a LOOP-phi back-edge whose value is defined later in RPO; the `emitCalleeInst` Phi arm is kept as a defensive guard; correctness comes from the remap (handles any incoming count / multiple phis / Arg-or-inst incomings / loop back-edges), fully source/target/linker-agnostic (routing keys only on `funcBlockCount != 1`). Proven by 6 RED-on-disable `tests/opt/test_inlining.cpp` pins (join+return-merge coexist, loop back-edge remap, arg-keyed corpus mirror, the verifier-CANNOT-catch value↔pred transposition [hand-built], the pred-mis-remap-IS-caught complement, AND a verifier-blind value↔pred SOLE-guard test running THROUGH the real `runInlining` — mutation-proven RED-on-transposition while verify() stays GREEN) + the `examples/c-subset/phi_inline` runtime witness (`pick(0)`→exit 9, transpose→7 fires the differential). Independent review CLEARS-THE-BAR; ctest 281→282. (D-OPT7-INLINE-LEGALITY-GATE stays OPEN — only the recursion + regular-`Call`-non-leaf + the SIZE/cost-bound + the IntrinsicCall arms closed; the still-open gate arms are the SOPHISTICATED cost model [call-site hotness / inlining-savings / growth-vs-benefit] + depth-bounded RECURSIVE inlining + escape-analysis relaxation — all deferred normal or trigger-gated.)** Each OPT7 cycle stays supervised (scope pre-approved by the §B directive; in-cycle §B gates remain live for any genuine architectural fork / pending-definition). **Inlining (interprocedural)** (`G-406`) — call-graph + **cross-function SSA splice** + recursion policy + StructCfMarker composition + `costModel` profitability + §2.9 legality gates. **Cross-CU-capable** (`D-OPT7-1` unblocked by P3+P4). **End-to-end Weak negative pin**: 2-CU (weak `f` in A, strong `f` in B, caller inlines) → gate refuses the weak inline; strong replaces weak at link; runtime proves correctness. In-cycle refactor: `D-OPT-COMPILE-OPTIONS-VARIANT` (inline-threshold field). | P1–P4 ✅ + user opens | `D-OPT7-1` (unblocked), `D-OPT7-WEAK-INLINE-NEGATIVE-PIN`, `D-OPT-COMPILE-OPTIONS-VARIANT` |

**Discipline**: each cycle's row links to the commit(s) where it landed. A cycle is CLOSED only when (a) the headline deliverable runs end-to-end, (b) the 7-agent review's HIGH/CRITICAL items are folded, (c) the deferred items the cycle anchors are recorded in §3.1, and (d) full ctest is green. The stepper is the single navigation surface: anyone reading "where are we?" reads this section.

---

## 1. Motivation

The optimizer is the **highest-leverage and the riskiest** layer of the whole compiler, for reasons specific to this project:

1. **The config-thesis multiplier is maximal at MIR.** Every source language is already lowered through HIR and every extension type resolved to the core lattice at the HIR→MIR boundary, so a single MIR pass benefits *every language × every target simultaneously*. One line of optimizer effort pays off across the entire compilation matrix — a leverage LLVM's per-language frontends never had. And because WASM bytecode and SPIR-V are MIR-downstream, they inherit every MIR pass for free; only transpilation (source→source, HIR-downstream) needs its own bounded tier (§2.5).

2. **It is where the "beat LLVM" claim is won or lost — and it is won by being *data*, not by being *optimal*.** Absolute optimality is undecidable for everyone, forever. The defensible, devastating claim is narrower and true: **our heuristics are a searchable space; LLVM's are frozen C++.** We can keep climbing per-target and per-workload where they are stuck with a global compromise. That advantage exists *only if the cost model and pipeline are configuration from the start*. This plan's PR1 makes that structural commitment.

3. **It is the sharpest silent-miscompile surface in the project.** A wrong encoding byte (plan 13) produces a different instruction; a wrong optimization produces a different *program* that still type-checks, links, and runs — just wrong. The assembler answered this with a round-trip oracle; the optimizer answers it with **verify-after-every-pass + differential execution against the unoptimized build**. No pass merges without both.

4. **It carries a DSS-specific hazard no textbook optimizer has:** WASM/SPIR-V codegen consume `StructCfMarker`s on MIR blocks directly (no Relooper). Any CFG-restructuring pass that doesn't preserve/repair those markers silently breaks the GPU and WASM backends. This is a first-class design constraint, enforced by a verifier rule (`X_StructCfMarkerBroken`).

**Hermetic invariant.** Like every other phase, the optimizer is self-contained C++ over JSON config — no external optimizer (no LLVM `opt`, no GCC), no link against a third-party pass library. The "hard part in the source code" is the universal pass algorithms; the per-target knowledge is JSON.

---

## 2. Design

### 2.1 Files (rev 1)

```
src/opt/
├── optimizer.{hpp,cpp}            # OPT1 — optimize(Mir, TargetSchema, OptPipeline, reporter) → Mir
│                                  #        + optimizeLir(Lir, ...) → Lir (target-aware tier)
├── opt_pipeline.{hpp,cpp}         # OPT1 — pass-pipeline-as-config loader + closed PassId vocabulary
├── mir_pass_util.{hpp,cpp}        # OPT1 — MIR analogue of lir_pass_util (build-once-freeze helpers)
├── analysis/
│   ├── mir_dominators.{hpp,cpp}   # OPT1 — hoisted from mir_verifier (Cooper-Harvey-Kennedy), reusable
│   ├── mir_use_list.{hpp,cpp}     # OPT1 — def→uses index (the workhorse for DCE/copy-prop/CSE)
│   └── mir_loop_forest.{hpp,cpp}  # OPT6 — natural loops from dominators + StructCfMarker cross-check
├── hir/
│   └── pass_transpile_clean.cpp   # OPT2b — structure-preserving HIR→HIR cleanup (transpile path only)
├── mir/
│   ├── pass_dce.cpp               # OPT2 — dead-code elimination (reads opcodeInfo.hasSideEffects)
│   ├── pass_const_fold.cpp        # OPT2 — SSA constant folding/propagation (reuses const_eval core)
│   ├── pass_copy_prop.cpp         # OPT2 — copy propagation
│   ├── pass_peephole.cpp          # OPT2 — algebraic identities as DATA (rewrite table)
│   ├── pass_gvn.cpp               # OPT3 — global value numbering / CSE over dominators
│   ├── pass_dse.cpp               # OPT3 — dead-store elimination
│   ├── pass_cfg_simplify.cpp      # OPT5+ — branch-fold + jump-thread (c2) + block-merge + marker re-derivation (c3); marker-safe
│   ├── pass_licm.cpp              # OPT6 — loop-invariant code motion
│   └── pass_inline.cpp            # OPT7 — inlining (profitability from cost model)
├── lir/
│   ├── pass_lir_peephole.cpp      # OPT5 — target-aware peephole (rewrite table over real opcodes)
│   ├── pass_coalesce.cpp          # OPT5 — register coalescing (cut the 2addr/isel mov traffic)
│   └── pass_schedule.cpp          # OPT19 — list scheduler over the per-target machine model
└── tune/
    └── autotuner.{hpp,cpp}        # OPT10 — corpus + search loop over cost-model/pipeline config (research)

src/opt/scalar_eval.{hpp,cpp}      # OPT2 — tier-neutral scalar arithmetic (extracted from const_eval; HIR+MIR share)
src/core/types/target_schema.*    # OPT1 — extend: costModel + machineModel + peepholeRules facets (bucket-1)
src/mir/mir.*                     # OPT1 — extend: SymbolBinding/SymbolVisibility on MirFunc/MirGlobal (§2.9)
src/dss-config/targets/*.target.json      # OPT1 — each target gains cost/machine/peephole numbers (hand-set v1)
src/dss-config/optimizer/mir-rewrites.rules.json  # OPT1/OPT2 — target-neutral MIR algebraic identities, as DATA
src/dss-config/pipelines/*.pipeline.json  # OPT1 — pass order per opt-level + per-output-path
                                          #        (-O0/-O1/-O2 native; transpile-readable; …), as DATA
```

No `src/opt/x86/` or `src/opt/arm64/` directories, ever. No `if (schema.name() == ...)`. Tuning differences live in JSON numbers; the pass code is target-blind (§2.4).

### 2.2 The pass model

Every pass obeys the **build-once-freeze** discipline already proven at LIR: a pass reads an immutable input module and **builds a fresh one** via the builder — never mutates in place. `mir_pass_util` hoists the tier-invariant machinery (diagnostic emission, block-ref remapping, terminator dispatch) exactly as `lir_pass_util` does for LIR.

Uniform signature, mirroring the existing backend passes:

```cpp
// MIR tier (target-neutral)
[[nodiscard]] MirPassResult runPass(PassId, Mir const&, MirAnalyses&, OptContext const&, DiagnosticReporter&);
// LIR tier (target-aware — also takes TargetSchema for the cost/machine model)
[[nodiscard]] LirPassResult runPass(PassId, Lir const&, TargetSchema const&, LirAnalyses&, OptContext const&, DiagnosticReporter&);
```

**Verify-after-every-pass** runs **unconditionally, in every build mode** (not debug/CI-only): after each pass the tier verifier (`MirVerifier` / `verifyLir`) re-runs; a failure is `X_VerifyAfterPassFailed` naming the offending pass. The structural verify is O(module) — cheap relative to the optimization it guards — so gating it behind build mode would let a *release* build ship a miscompile unchecked, contradicting the plan's #1 risk. Only the expensive **differential-execution** check (rebuild + run the corpus, §5) is reserved for CI. This is the optimizer's equivalent of the assembler's round-trip oracle — the gate that makes silent miscompiles loud.

### 2.3 Analysis substrate

Analyses are computed once per pipeline run and invalidated/recomputed on demand:
- **Dominator tree** — hoisted out of `MirVerifier` (which already computes it via Cooper-Harvey-Kennedy) into `analysis/mir_dominators` so DCE/GVN/LICM share one implementation.
- **Use-list (def→uses)** — the workhorse index; copy-prop, DCE, and CSE are all use-list walks.
- **Loop forest** (OPT6) — natural loops from back-edges + dominators, cross-checked against `StructCfMarker::LoopHeader/Latch/Exit` (the markers are a free correctness oracle here).

Analyses are pure functions of a frozen module (no mutation), so caching is trivial and an autotuner can re-run a pipeline deterministically.

### 2.4 Heuristics-as-data — the cost & machine models (the autotuner foundation)

This is the structural commitment that distinguishes this plan. **Three** new **bucket-1** JSON facets on `TargetSchema`:

- **`costModel`** — per-opcode cost / latency / register-pressure weight; profitability constants (inline threshold, unroll factor cap, spill cost). Read by isel-quality, peephole, LICM, inlining, scheduling. **Rows are indexed by opcode ordinal** (parallel to `opcodes[]`, the `relocationKindIndex` discipline); `validate()` requires full opcode coverage. The register-pressure / spill fields are **meaningful only when `abiModel == RegisterMachine`** — `validate()` rejects them on `OperandStack`/`ResultId` targets, so the abiModel gate is *data-validated*, never a pass-time `if (abiModel==...)` branch.
- **`machineModel`** — issue width, execution ports, per-opcode port assignment + latency. **Indexed by opcode ordinal**, full-coverage-validated (same as `costModel`), so the list scheduler (OPT8) reads it by ordinal and never switches on mnemonic strings.
- **`peepholeRules`** — the LIR rewrite table *as DATA*: a JSON list of `{match, replacement, guard}` over the target's own opcode mnemonics/encoding rows, consumed by ONE universal matcher in `pass_lir_peephole.cpp` (OPT5). This is the carrier that keeps target-aware peephole **bucket-2-over-bucket-1** instead of hardcoded per-arch C++ patterns — the same posture as the assembler's `encoding` facet.

The **MIR** peephole's algebraic-identity rules are target-neutral, so they do NOT live on `TargetSchema` — they live in a tier-neutral `src/dss-config/optimizer/mir-rewrites.rules.json` keyed on the closed `MirOpcode` mnemonics, consumed by the same universal matcher. ("as DATA" is structural for both tiers — searchable by the OPT10 autotuner, never a hardcoded `if (op==Add && rhs==0)` table.)

And a **pipeline-as-config** vocabulary (one closed `PassId` per *algorithm* — never a target-specialized variant like `PassId::PeepholeX86`; target differences live entirely in the rule/cost/machine data above):

- **`*.pipeline.json`** — an ordered list of `PassId`s per opt-level (`-O0` = empty/verify-only, `-O1`, `-O2`). `PassId` is a closed enum (the *passes* are fixed C++ algorithms); their *order and selection* are data.

The **three-bucket test applied to optimization** (per [`ZZ`](./ZZ-final-goal.md) §2): a pass is **bucket-2** (a universal algorithm — value numbering, list scheduling, coalescing) reading **bucket-1** vocabulary (opcode table, cost model, machine model, pipeline order). The forbidden **bucket-3** shape is `if (target.name() == "x86_64") threshold = 225;`. The cost/machine numbers being *data* is precisely what keeps every pass bucket-2 — and what makes the search space exist.

> **Why now and not later (answered directive).** Retrofitting frozen C++ constants into config after the passes exist means re-threading every profitability call site. Declaring the facets in PR1 — even with hand-set default numbers — costs almost nothing and is the only way the OPT10 autotuner is reachable. v1 ships sensible hand-tuned defaults *in JSON*; the tuner later searches the same JSON.

### 2.5 The layering map + per-output-path policy

Optimization leverage is maximized by running each transform at the tier where the most information still exists *and* the transform is still legal. Crucially, the output paths leave the pipeline at **different altitudes** — native goes all the way to LIR; **WASM bytecode and SPIR-V bypass LIR (they are MIR-downstream); and transpilation bypasses MIR entirely (it is HIR-downstream)** — so each tier serves a different set of outputs:

| Tier | Runs here | Output paths served | Constraint |
|---|---|---|---|
| **HIR — semantic const-eval** (✅ shipped) | array lengths / enum values / global inits via `const_eval` (plan 12.5) | all | runs at semantic time, not an opt pass |
| **HIR — transpile cleanup** (NEW, OPT2b) | structure-preserving HIR→HIR folds: literal const-fold, provably-dead-branch prune, redundant-cast/expr cleanup | **transpilation (source→source) only** — the path that never reaches MIR | **opt-IN + readability-bounded**: aggressive opts are *forbidden* here — they destroy the readable target source that IS the artifact |
| **MIR — full scalar/redundancy/CF** | DCE, const-fold/prop (SCCP), copy-prop, peephole, GVN/CSE, DSE, CFG-simplify, LICM, inlining | **native exec + WASM bytecode (plan 18) + SPIR-V (plan 17)** — every MIR-downstream backend, *for free* | target-neutral ⇒ one impl × every lang × every MIR-downstream target. The multiplier. |
| **LIR — target-aware** | peephole on real instructions, **register coalescing**, address-mode folding, scheduling | **native exec only** (WASM/SPIR-V bypass LIR) | needs real opcodes / register file / machine costs that exist only post-isel |
| **per-target (data)** | the cost & machine *numbers* the LIR (and any future WASM/SPIR-V) passes consume | native (+ MIR-downstream if they grow a cost model) | pure data; the algorithms are universal |

**The key consequence (the non-native paths are NOT left unoptimized):** WASM and SPIR-V **inherit the entire MIR optimizer for free** — one MIR pass improves native, WASM, and GPU simultaneously, the config-thesis multiplier at its strongest. Transpilation is the genuine special case: it never reaches MIR, so it gets its own HIR cleanup tier — and there, optimization is deliberately *bounded*, because the artifact is human-readable source whose value is structural fidelity to the input.

**Optimization policy is therefore a property of the output path, declared as config — never a C++ branch.** It selects a named pipeline (reusing OPT1's pipeline-as-data vocabulary, §2.4), one per output kind:

```json
// on the artifact profile (plan 06), defaulting per language, overridable by the transpile map
// (plan 10) — deliberately NOT on .target.json (policy must not couple to target identity)
"optimization": {
  "optimizeTranspilation": false,      // the simple knob — false ⇒ emit faithful source
  "pipeline": "transpile-readable"     // richer form: name any *.pipeline.json (sugar above expands to this)
}
```

- `optimizeTranspilation: false` ⇒ no HIR cleanup; the transpiler emits structurally-faithful target source.
- `true` ⇒ run the `transpile-readable` pipeline — a *bounded* HIR pass set (literal folding, dead-branch pruning, redundant-cast removal) that **excludes** any pass altering recognizable shape (no inlining, no CFG flattening, no GVN).
- Richer policies ("fold constants but keep all functions", "strip dead code only") are just additional named pipelines — **no engine change, by construction** (heuristics-as-data, §2.4).

The optimizer enforces that a transpile pipeline may name *only* readability-safe HIR-tier passes — a tier guard firing `X_UnsupportedPassForTier` (§2.10) if a native/MIR pass is requested for the transpile path. The transpile-cleanup passes themselves dispatch on `HirKind` (the closed+registry vocabulary, same as the HR8 lowering engine) — **never on `tree.schema()` / source-language identity**; an extension kind they can't fold is left untouched, not language-branched. Native and MIR-downstream outputs select aggressive pipelines (`-O2`, etc.); the one mechanism covers all three.

### 2.6 Constant folding: HIR vs MIR (reuse decision)

`const_eval` (plan 12.5) is HIR-side and reads HIR shape; it stays the owner of *source-constant* folding. MIR needs **propagation** of values that become constant only after other passes, flowed across the CFG (sparse conditional constant propagation). The MIR fold pass operates on `MirInstId` operands and reuses the scalar arithmetic core (integer/float/cast/compare). Because that core today reads HIR shape, **OPT2 extracts it into a tier-neutral `scalar_eval` unit** shared by HIR and MIR fold — **committed, not conditional**: the cross-tier reuse is certain, so there is no "decide later"; the long-term solution is the extraction (Open Question #1, resolved).

### 2.7 StructCfMarker preservation (DSS-specific constraint)

Any pass in `pass_cfg_simplify` / `pass_licm` / `pass_inline` that adds, removes, or merges blocks **must** maintain the `StructCfMarker` invariants the WASM/SPIR-V backends depend on. Enforced two ways: (a) the existing `MirVerifier::checkStructCfMarkers` runs in the verify-after-pass gate; (b) a dedicated `X_StructCfMarkerBroken` diagnostic when a restructuring pass leaves a marked region structurally inconsistent (e.g. an orphaned `LoopHeader` without its `LoopLatch`). CFG passes that cannot prove marker-preservation are restricted to marker-neutral transforms until OPT4's marker-repair logic lands.

### 2.8 The autotuner arc (OPT10 — research frontier)

The endgame, scoped here so it isn't a floating aspiration:
- **Corpus** — a representative body of programs (the c-subset corpus + self-host source + synthetic kernels) with a defined objective (size / cycles / a chosen Pareto axis).
- **Measurement loop** — builds with a candidate `costModel`+`pipeline` config, measures the objective. Ground truth leans on assets already built: the **round-trip encoding oracle** (plan 13) and the **bit-identical self-host** (ZZ §self-host) keep the loop from fooling itself.
- **Search** — over the JSON config space (random/evolutionary first; learned cost model later). Output is a *new `.target.json` cost block* — a generated artifact, diffable and version-controlled.

This is explicitly a research bet (§6): the substrate (config-shaped heuristics) is v1; the *tuner closing the gap on real code* is a hypothesis to be proven on the corpus, not assumed.

### 2.9 Linkage & symbol liveness — the linker-agnostic guarantee

Three passes touch *symbols*, not just SSA values — DCE (of whole functions/globals), inlining, and dead-store elimination — and all three are silent-miscompile seams if they ignore **linkage**. MIR today carries only a `SymbolId` per function/global; it has thrown linkage away. **OPT1 closes this as a committed deliverable (not a deferral):** it threads a **format-neutral** linkage attribute onto every MIR function/global, reusing the *already format-blind* `SymbolBinding{Local,Global,Weak}` + `SymbolVisibility{Default,Hidden,Protected,Internal}` vocabulary from the linker substrate ([`14`](./14-linker-plan%20-%20tbd.md) LK4, `object_format_schema.hpp`). The HIR→MIR boundary is *designed* to map the HIR `FfiLinkage`/`FfiVisibility` onto these enums (including the `Common`/`Internal` edge cases). **Status correction (2026-06-04):** OPT1 landed the MIR-side attribute slot + `isExternallyVisible` predicate, but the mapping itself is **stubbed** — `lowerToMir` defaults every user function/global to `Global+Default` (verified: no `FfiLinkage→binding` mapping in `src/mir/lowering`; c-subset has no `static`/`weak` to map). Sound-but-conservative for DCE (nothing wrongly eliminated), but the inline-legality gates stay **unexercised** until a real linkage source lands. Closing the mapping is pre-OPT7 step P2 (`D-OPT7-LINKAGE-HIR-TO-MIR-MAPPING`). **This is precisely how the optimizer stays linker-agnostic: a pass reads `binding == Global` / `visibility != Hidden`, never `if (format == Elf)`.**

The liveness/legality rules these enums gate:

- **DCE liveness roots** — whole-symbol DCE may eliminate a function/global *only* when it is `Local` binding **and** `Hidden`/`Internal` visibility **and** has no in-module use **and** is not address-taken (`GlobalAddr`). Externally-visible symbols (`Global`/`Weak` binding, or `Default`/`Protected` visibility), the declared entry point, and any address-taken symbol are **always live roots** — even with zero local uses. (`hasSideEffects` governs *instruction*-level DCE; it says nothing about whole-symbol reachability — independent gates.)
- **Inline legality** — never inline a `Weak` callee (a strong definition may replace it at link time; inlining bakes in the wrong body). When inlining an externally-visible or address-taken callee, its out-of-line definition stays a DCE root (it is *not* "now unused"). Cross-CU inlining is `D-OPT7-1` (architecture chosen — see below).
- **★ Inline legality, rule 2b — a SOURCE DIRECTIVE, added 2026-07-28 (TF-C78, `D-CSUBSET-NOINLINE-PER-FUNCTION-SINK`):** never inline a callee the source declared `__attribute__((noinline))`. It rides a per-function `MirFunc.noInline` bit (one byte out of the struct's existing `_pad` — `sizeof(MirFunc)` stays 24) fed from the C-side attribute through the config's `attributeSemantics.effects` `noInline` verb, and the refusal sits immediately beside the Weak one in `src/opt/passes/inlining.cpp`. ★ **It is categorically unlike every other rule in this section, and the distinction is worth keeping: the others are the optimizer protecting itself from an UNSOUND splice; this one is the optimizer OBEYING an explicit directive.** It is therefore unconditional — no cost-model arm, no scope escape hatch — because the directive has runtime consequences the optimizer cannot see (sqlite spells `SQLITE_NOINLINE` to BOUND STACK DEPTH on recursive paths). ★★ **AND IT IS ALL-OR-NOTHING ACROSS THE `addFunction` CHAIN, which is the durable lesson for any future per-function MIR fact:** the bit must be re-emitted at every site that CREATES a `MirFunc` from another one — the shared rebuild substrate (`opt/passes/mir_rebuild_helper.cpp`, under EVERY pass), the inliner's own caller rebuild, the cross-CU merge, and the MIR text round-trip — because MEASURED, deleting the refusal and dropping only the rebuild propagation produce **byte-identical breakage**: a half-landed flag and no flag at all are indistinguishable in the emitted code. Each hop therefore carries its own pin; the end-to-end test cannot say which hop failed. Language-agnostic by construction: MIR carries a bool, the attribute NAME lives in the language config, and no pass branches on a source language.

#### Cross-CU inlining architecture — §B decision (2026-06-05, user-supervised)

`D-OPT7-1` = **Design A: whole-program MIR merge (LTO model), "keep linker merge + share resolver" flavor.** The optimizer is per-CU today (each CU's MIR is built → optimized → lowered → discarded; no cross-CU MIR coexistence) and all cross-CU symbol resolution lives at the **assembled tier** (the cycles 15–19 linker substrate; MIR has no names). The chosen architecture, with **no duplicated merge logic**:
1. **One shared, tier-neutral resolver** — extract the weak-vs-strong winner-selection (`name → winning (cuId, SymbolId)`; strong-shadows-weak / all-weak-lowest-key) out of the linker's `resolveCrossCuSymbols` into a **pure function** consumed by BOTH the assembled-tier linker merge AND the new MIR-tier merge. The merge *mechanics* differ per representation (MIR symbol/const/global remap vs machine-code/reloc remap); the *policy* is shared once.
2. **A MIR-tier whole-program merge** (the single cross-CU merge for compiled-together programs): build all N CU MIRs first (retaining each CU's `SemanticModel` for names), then merge into ONE module — unified symbol space, const/global pool merge, drop shadowed-weak functions, and rewire cross-CU `extern` calls into **intra-module direct calls**.
3. **The existing inliner runs on the merged module UNCHANGED** — post-merge, cross-CU calls are intra-module, so the cycle-1/2 call-graph + §2.9 gate + splice just work. Cross-CU inlining "falls out" of the merge; the Weak rule still guards a weak def that survives merge (a strong sibling may appear at a *final* link with external objects).
4. **Lower the one merged module → the existing single-CU linker path** (one module in → no assembled cross-CU merge runs for this path).
5. **The assembled-tier cross-CU merge + thunk-slot path STAYS** — reserved as the linker's general object-merge capability for **future separate compilation** (`.o` linking, where no MIR is available) — refactored to call the SAME shared resolver. For compiled-together programs the thunk path is dormant-but-justified (pinned: `D-OPT7-CROSSCU-THUNK-RESERVED-FOR-SEPARATE-COMPILATION`).

**Interner unification — §B decision (2026-06-05, user-supervised): Option A (re-intern at merge time)** — **(implemented as a fresh host, 2026-06-05).** The merged `Mir` lowers with ONE `TypeInterner`, but each CU owns its own (every `TypeId` is interner-relative + owner-stamped by `CompilationUnitId`; cross-interner access is release-fatal). Option A topologically re-interns each CU's reachable `TypeId`s into a single host `TypeLattice` (bottom-up walk via the existing public `kind()`/`operands()`/`scalars()`/`name()`/`extensionKind()` accessors + the deterministic builders), building per-CU `oldTypeId→newTypeId` remaps. **Implementation note (2026-06-05): the driver creates a FRESH host `TypeLattice`** (owned by the merged module, seeded with CU0's `CompilationUnitId` + source language) and re-interns ALL CUs *including CU0* into it — functionally equivalent to hosting in CU0's interner in-place (still Option A "re-intern at merge"), but it avoids moving CU0's lattice out of its `SemanticModel`, keeping every model intact for the merge's name lookups (so CU0's types are re-interned through the same `reinternType` walker rather than passed through unchanged). Chosen over Option B (shared interner from `analyze()`) because A is **consistent with the separate-compilation future** the "keep linker merge" decision preserves — separately-compiled `.o`s are interned at different times, so re-intern at merge is required regardless; B (shared-from-`analyze`) only serves the compiled-together case + is a 30+-call-site analysis-tier restructure that separate compilation would supersede. The re-intern primitive (`reinternType`) was built in c25.

**Staging (multi-cycle, boundaries calibrated by the plan-lock review):** c24 = pipeline refactor (split `buildAssembledModule` into build-MIR / lower-MIR; re-sequence `compileOneTarget` to build-all-MIRs-then-lower-all; retain `SemanticModel`s in a `CuMirModule`) — behavior-preserving, zero-regression. **c25 ✅ DONE 2026-06-05 = the MIR-tier merge substrate** (the merge landed — `reinternType` + `mergeCuMirs`; behavior-preserving — `cross_cu_call` re-proven via merge + a DIRECT call, exit 42; single-CU byte-identical; `D-OPT7-CROSSCU-MIR-MERGE` ✅). **c26 ✅ DONE 2026-06-05 = cross-CU inlining on the merged module + the cross-CU Weak negative pin** (the existing inliner runs on the merged module UNCHANGED — `optimizeModule` chokepoint + the merged-optimize wiring in the N>1 driver; 2-CU weak `f`→7 / strong `f`→42; merge resolves strong-wins; caller inlines the strong → exit 42; RED-on-disable at 7). **The cross-CU inlining loop is CLOSED (`D-OPT7-1` ✅, dss-cycles 24+25+26).** c27 ✅ DONE = general NON-LEAF inlining + SCC recursion safety (`call_graph_scc` Tarjan; recursive-SCC refusal). **c28 ✅ DONE = inline COST MODEL + ship-in-release** — `OptPipeline.inlineThreshold` (config-driven via the pipeline JSON, default 50) + the gate refuses a callee whose inst-count > threshold; **`Inlining` now SHIPS in `release.pipeline.json`** (a real -O optimization, no longer opt-in-only — bounded by the size threshold). **As of 2026-06-13 (D15 cycle C) this composition COMPILES AND RUNS control-flow programs** (recursion + loops) under `--config=release`, not merely config-eligible: the 2026-06-12 composition bug (`D-OPT-RELEASE-PIPELINE-CONTROL-FLOW-COMPOSITION` ✅ + `D-OPT2-REWRITE-MAP-COMPLETENESS` ✅) is fixed — see the registry rows. **c29 ✅ DONE 2026-06-05 = IntrinsicCall inlining relaxation** — the `IntrinsicCall` refusal is removed; an IntrinsicCall-bearing callee inlines (single-block + multi-block; SSA-correct via the splice's payload-copy); the frame-sensitive-intrinsic hazard is deferred trigger-gated (`D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC`) behind the `NoShippedConstructLowersToIntrinsic` fail-loud registry-empty tripwire. **The user-directed OPT7 sequence (c3–c6) is COMPLETE.** Each cycle was user-supervised.
- **DSE escape guard** — a store is *not* dead if its target may be observed externally: a store through a pointer derived from a `GlobalAddr` of a `Default`/`Protected`-visible global, or through a pointer escaped via `Call`, survives. DSE needs a may-escape/visibility test — it cannot lean on `hasSideEffects` the way DCE does (its whole job is removing side-effecting stores it proves redundant).

Violations are loud, not silent: `X_LiveSymbolEliminated` and `X_IllegalInlineOfWeak` (§2.10) fire at the offending pass. There is no ATOMIC axis on the MIR memory opcodes today (a volatile axis exists via `MirInstFlags::Volatile`); until one exists (D-OPT4-2), value-based branch folding (OPT4) is bounded to *pure SSA* conditions and never treats a memory-derived `Load` as invariant.

### 2.10 Diagnostics

New family **`X_` at `0x2xxx`** (claims a free nibble per [`00`](./00-compiler-implementation-plan%20-%20tbd.md) §0.3; the PR1 author updates that registry table + `parse_diagnostic.cpp`'s prefix switch in the same commit):

| Code | Name | Meaning |
|---|---|---|
| `0x2001` | `X_PassPipelineMalformed` | `*.pipeline.json` references an unknown `PassId`, is cyclic, or names a pass for the wrong tier. |
| `0x2002` | `X_CostModelInvalid` | `costModel`/`machineModel` facet fails validation (missing opcode row, negative latency, etc.). |
| `0x2003` | `X_VerifyAfterPassFailed` | A pass produced a module the tier verifier rejects — names the offending pass. The silent-miscompile gate. |
| `0x2004` | `X_StructCfMarkerBroken` | A CFG-restructuring pass left `StructCfMarker`s inconsistent (would break WASM/SPIR-V). |
| `0x2005` | `X_UnsupportedPassForTier` | Pipeline asks a MIR pass to run at LIR or vice versa (or a shape-altering pass on a transpile path). |
| `0x2006` | `X_LiveSymbolEliminated` | A pass would eliminate/orphan a symbol that is an externally-visible / address-taken / entry-point liveness root (§2.9). |
| `0x2007` | `X_IllegalInlineOfWeak` | Inlining attempted on a `Weak`-bound callee, whose definition may be link-time-replaced (§2.9). |

`0x3xxx` stays reserved (a future split, e.g. a dedicated autotuner-diagnostics family, if OPT10 needs its own band).

---

## 3. PR breakdown

| PR  | Title | Scope |
|-----|-------|-------|
| OPT1 | Optimizer substrate + heuristics-as-data | **The foundation, substrate-tier (5-agent review).** `src/opt/optimizer.{hpp,cpp}` with `optimize(Mir, TargetSchema, OptPipeline, reporter) → Mir`; `opt_pipeline` loader + closed `PassId` enum; `mir_pass_util` (build-once-freeze helpers mirrored from `lir_pass_util`); `analysis/mir_dominators` (hoisted from `MirVerifier`) + `analysis/mir_use_list`. **The data commitment:** `costModel` + `machineModel` + `peepholeRules` facets on `TargetSchema` + the tier-neutral `mir-rewrites.rules.json` (loader + `validate()` + accessors, same discipline as `relocations[]`/`registers[]`; opcode-ordinal-indexed + full-coverage-validated; register/spill fields gated to `abiModel==RegisterMachine`); `*.pipeline.json` vocabulary with `-O0/-O1/-O2` defaults shipped for x86_64 + arm64. **The linkage commitment (linker-agnostic guarantee, §2.9):** thread format-neutral `SymbolBinding`/`SymbolVisibility` onto MIR functions/globals + pin the HIR `FfiLinkage`/`FfiVisibility`→enum mapping at the HIR→MIR boundary, so DCE/inline/DSE have a linkage to honor. **Unconditional** verify-after-every-pass harness (all build modes). `X_` family at `0x2xxx` + plan 00 §0.3 update. **Per-output-path policy:** the `optimization` config block (§2.5) selects a named pipeline per output kind (native `-O2` / WASM-MIR / `transpile-readable`); `optimizeTranspilation` is sugar over it; a tier guard (`X_UnsupportedPassForTier`) rejects a pass named for the wrong tier. No passes yet beyond a no-op identity pass that exercises the pipeline + verify gate end-to-end. |
| OPT2 | MIR scalar opts (the high-multiplier core) | **The "easy 70%", feature-tier.** `pass_dce` (instruction-level via `opcodeInfo.hasSideEffects`; **whole-symbol DCE roots liveness on the §2.9 linkage attribute** — externally-visible / address-taken / entry symbols always survive), `pass_const_fold` (SSA fold/propagation; **OPT2 extracts the scalar arithmetic into a tier-neutral `scalar_eval` unit** shared by HIR + MIR fold, §2.6 — committed, not conditional), `pass_copy_prop` (pure-SSA, the one genuinely linkage-neutral pass), `pass_peephole` (algebraic identities as a DATA rewrite table → `mir-rewrites.rules.json`, §2.4). Differential-execution tests: optimized vs unoptimized c-subset corpus produce identical results; verify-after-pass green. This is the first real codegen-quality win and it lands across every target at once. |
| OPT2b | HIR transpile cleanup tier | **The non-native readability tier, feature-tier.** `src/opt/hir/pass_transpile_clean.cpp` — a *bounded*, structure-preserving HIR→HIR pass set (literal const-fold, provably-dead-branch prune, redundant-cast/expr cleanup) for the transpilation path that never reaches MIR (§2.5). Ships the `transpile-readable` pipeline + the tier guard that forbids shape-altering passes on a transpile output. **Gated on [`10-source-translation-plan`](./10-source-translation-plan%20-%20tbd.md)** (the HIR→target-CST path must exist to optimize). WASM bytecode (plan 18) + SPIR-V (plan 17) need NOTHING here — they inherit OPT2's MIR passes directly. |
| OPT3 | MIR redundancy elimination | `pass_gvn` (global value numbering / CSE over the dominator tree) + `pass_dse` (dead-store elimination — removes a store only after a **may-escape/visibility test** per §2.9; stores to externally-visible globals or escaped pointers are observable and survive). Anchored: requires use-list (OPT1) + dominators (OPT1). |
| SimplifyCFG | MIR control-flow simplification | `pass_cfg_simplify` — unreachable-block elimination, block merging, jump threading, branch folding — **with StructCfMarker repair** (§2.7). The marker-repair logic is the load-bearing piece (D-OPT4-1). **Recurring pass** (not one-shot): implemented once after CSE/GVN (§0 `OPT5+`), then listed repeatedly in `*.pipeline.json` + run to fixed point with ConstFold/DCE. *(Label de-integered: this row previously read "OPT4", which collided with §0's as-built OPT4 = copy-prop. NOTE: this §3 PR-breakdown uses design-era OPT-N labels that diverge from §0's as-built sequence throughout — §0's cycle log is the live numbering surface; a full §3 renumber is a separate cleanup.)* |
| OPT5 | LIR target-aware opts | `pass_lir_peephole` (rewrite table over real opcodes — cuts redundant movs / setup) + `pass_coalesce` (register coalescing — directly reduces the mov traffic from 2addr-legalize + naive isel) + address-mode folding. First consumer of the `costModel` facet. |
| OPT6 | Loop optimization | `analysis/mir_loop_forest` (natural loops, marker-cross-checked) + `pass_licm`. Unrolling anchored separately (profitability from `costModel`). |
| OPT7 | Inlining (interprocedural) | Call-graph construction + `pass_inline` with a **data-driven** profitability threshold (from `costModel`, not a C++ constant). **Inline legality gates on the §2.9 linkage attribute:** never inline a `Weak` callee; an externally-visible / address-taken callee keeps its out-of-line body (not DCE'd). Couples with [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) for cross-CU visibility limits. **Pre-OPT7 chain (decided 2026-06-04, full cross-CU Weak-pin scope):** P1 front-end linkage specifiers → P2 HIR→MIR linkage mapping → P3 CU6 → P4 LK11 (weak-vs-strong resolution); P3+P4 unblock `D-OPT7-1` so OPT7 is cross-CU from the start. The genuine Weak negative pin is end-to-end 2-CU (strong replaces weak at link) — `D-OPT7-WEAK-INLINE-NEGATIVE-PIN`. See §0.1 Upcoming + §3.1. |
| OPT19 | Instruction scheduling | `pass_lir_schedule` — list scheduler over the per-target `machineModel` (ports + latencies as JSON). Pure bucket-2: one scheduler, every target's numbers. |
| OPT9 | Vectorization (frontier) | SLP + loop vectorization. Legality via a dependence-analysis framework (universal); profitability via `costModel`. The deep end — explicitly research-grade. |
| OPT10 | The autotuner (research arc) | Corpus + measurement loop (leveraging the round-trip oracle + self-host as ground truth) + search over `costModel`/`pipeline` config; optional learned cost model. Output = generated `.target.json` cost blocks. The "climb past frozen hand-tuning" endgame. |

Substrate tier (5-agent review) for **OPT1** (the pass engine + the heuristics-as-data facets — the contract everything else builds on). Feature tier for OPT2+.

**v1 = OPT1 + OPT2.** OPT3–OPT10 are anchored deferrals (§3.1), each with an owner cycle + trigger — not a silent backlog.

---

## 3.1 Deferred-items registry (OPT)

Mirrors plan 12 §3.1 / plan 13 §3.1 / plan 14 §3.1. Every deferred item names the specific future PR/cycle that closes it + a gate condition. Struck through when the closing commit lands.

> **The deferral rows that were listed here have moved into the anchor registries.** The open ones are in `.plans/_deferred-anchor-registry-production.md` and the closed ones in `.plans/_deferred-anchor-registry-done.md`, under `D-OPT-*`, `D-OPT1-*`, `D-OPT6-*`, `D-OPT2-*`, and moreand more. A row whose id this plan spelled in a form no registry could hold carries its former spelling in its `Cross-refs` cell. Read the registry, never this document, for what is still open.


<!-- §B 2026-06-05 (user-supervised): OPT7 c4 "recursion/SCC safety" was found PREMATURE — the inliner is leaf-only and `inlining.cpp` documents "a leaf can't recurse", so recursion is already refused by the leaf gate, and c5 (cost-model)/c6 (IntrinsicCall-relax) don't reintroduce it; a recursion policy has no consumer until GENERAL NON-LEAF inlining lands. User chose to **pursue general non-leaf inlining NOW** (Option 3) — land non-leaf inlining + the recursion-safety policy together (recursion safety gets a real consumer). Design lean: lift the `Call`-leaf refusal + SCC-based recursion refusal (refuse inlining a call within a recursive call-graph cycle; cleaner than depth-tracking; non-recursive DAG depth bounded by the pipeline `maxIterations` + the future c5 cost model). cycle 27 ✅ DONE 2026-06-05: lifted Call-leaf + call_graph_scc Tarjan + recursive-SCC refusal; non-leaf is correctness-preserving + opt-in; next = OPT7 c5 cost-model+ship. -->

---

## 4. Open questions

| # | Question | Default if unanswered |
|---|---|---|
| 1 | ~~Reuse `const_eval` directly, or extract its scalar core?~~ | **Resolved:** OPT2 extracts the scalar arithmetic into a tier-neutral `scalar_eval` unit shared by HIR + MIR fold. The cross-tier reuse is certain, so this is committed in OPT2 — not a conditional "if it bites" deferral. |
| 2 | Does `costModel` live as a facet on `TargetSchema`, or as a separate `*.cost.json` file? | Facet on `TargetSchema` (one target = one file; matches `registers`/`relocations` precedent). Revisit if the autotuner wants to swap cost blocks without touching the rest of the schema. |
| 3 | Opt-level mapping — fixed `-O0/-O1/-O2` pipelines, or fully free-form named pipelines? | Ship the three standard levels as named `*.pipeline.json`; the format already allows arbitrary named pipelines for the autotuner. |
| 4 | Where does SSA-destruction / phi-lowering interact with the optimizer vs regalloc? | Phi-lowering stays in the LIR lowering/regalloc path (plan 12); the optimizer operates on phi-bearing SSA MIR and never destructs it. |
| 5 | Autotuner objective — single scalar (cycles) or multi-objective Pareto? | Single objective (cycles) for the first OPT10 loop; Pareto is D-OPT10-2. |
| 6 | Where does the `optimization` policy block live — `.lang.json`, artifact profile (plan 06), or transpile `.map.json` (plan 10)? | Artifact profile (plan 06) owns it (opt policy is a property of *what you produce*), defaulting per-language, overridable by the transpile map. |

---

## 5. Acceptance criteria

- [ ] **OPT1**: `optimize()` runs a configured pipeline end-to-end; the verify-after-every-pass gate is active; `costModel`+`machineModel`+`*.pipeline.json` load + `validate()` with fail-loud diagnostics; x86_64 + arm64 ship default cost/pipeline data. Zero passes regressed the existing ctest suite.
- [ ] **OPT2**: DCE + const-fold + copy-prop + peephole each have unit tests; **differential execution** — the c-subset corpus, optimized vs unoptimized, produces byte-identical runtime results; every pass leaves MIR that re-verifies.
- [ ] **Three-bucket compliance**: no `if (schema.name() == ...)` / no per-target optimizer C++ / no `src/opt/<arch>/` directory. All tuning differences are JSON.
- [ ] **Self-host unaffected**: the bit-identical self-host (ZZ) still reproduces with the optimizer in the pipeline (optimization is deterministic + verified).
- [ ] **StructCfMarker safety**: any CFG-restructuring pass passes `checkStructCfMarkers` post-transform; WASM/SPIR-V codegen (when present) consumes optimized MIR unbroken.
- [ ] **Output-path policy (OPT2b)**: `optimizeTranspilation: false` ⇒ transpiled output is structurally faithful (no cleanup); `true` ⇒ the `transpile-readable` pipeline runs and the tier guard rejects any shape-altering pass; WASM/SPIR-V output is verified to inherit the MIR pipeline with no transpile-tier involvement.
- [ ] **Linkage / linker-agnostic (OPT1, §2.9)**: MIR functions/globals carry format-neutral `SymbolBinding`/`SymbolVisibility`; DCE roots liveness on it — a library CU with an exported-but-locally-unused function **retains that symbol in the linked image** (asserted on the **symbol table**, not runtime output, since a self-contained run would not catch it); inlining never touches `Weak` callees. **Zero `if (format==...)` in any pass.**
- [ ] **Verify is unconditional**: the structural verify-after-pass runs in release builds too; only differential-execution is CI-gated. Differential execution runs on the **host-native** target in v1 (cross-compiled arm64/WASM/SPIR-V differential is anchored, not silently assumed).

---

## 6. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| **Silent miscompile** (a wrong opt changes program meaning, still type-checks/links/runs) | High | Critical | Verify-after-every-pass + **differential execution** (optimized vs unoptimized must match) is the merge gate — the optimizer's equivalent of the assembler's round-trip oracle. No pass merges without both. |
| **Linkage-blind symbol DCE/inline silently breaks the link** (drops an exported symbol, inlines a weak callee) | High | Critical | OPT1 threads format-neutral `SymbolBinding`/`SymbolVisibility` onto MIR (§2.9); DCE/inline/DSE root liveness/legality on it; `X_LiveSymbolEliminated` / `X_IllegalInlineOfWeak` fire loud; acceptance asserts an exported-unused symbol survives in the symbol table. The vocabulary is the linker's own format-blind enums — no `if (format==...)` anywhere. |
| **StructCfMarker breakage** silently corrupts WASM/SPIR-V output | Medium | High | `checkStructCfMarkers` in the verify gate + dedicated `X_StructCfMarkerBroken`; CFG passes restricted to marker-neutral transforms until OPT4 marker-repair lands. |
| **The autotuner doesn't close the gap** (config search underperforms LLVM's hand-tuning on real code) | Medium | Medium | OPT10 is explicitly a research bet, not a v1 commitment. The substrate (heuristics-as-data) has standalone value (per-target tuning, reproducibility) even if the learned tuner underwhelms. Prove on the corpus before claiming. |
| **Heuristics-as-data adds validation surface** (bad cost numbers → bad code, not a crash) | Medium | Medium | `validate()` rules on cost/machine facets (no negative latency, every opcode covered); differential execution catches behavior changes; cost errors degrade *speed*, never *correctness* (correctness is the verifier's job, independent of cost). |
| **Identity-branch drift under real ISAs** (a target's quirk tempts `if (arch==...)` in a pass) | Medium | High | The OPT1 5-agent review establishes the bucket-2 contract; every later pass review checks it. A quirk that can't be expressed as cost/machine *data* is an honest deferral (a new facet field), never a branch. |
| **Compile-time regression** from recompute-on-demand analysis | Low | Medium | D-OPT1-2 anchors incremental analysis; v1 module sizes don't need it. |
| **Over-optimizing transpile output destroys readability** (the artifact's whole value) | Medium | High | Transpile optimization is opt-IN (`optimizeTranspilation` defaults false) + tier-guarded to a readability-bounded HIR pass menu; shape-altering passes (inline / CFG-flatten / GVN) are structurally barred from transpile pipelines (`X_UnsupportedPassForTier`). |

---

## 7. Sequencing

```
12-mir-lir (ML8 ✅) ──┐
12.5-const-eval (✅) ──┤
                      ├─►  [project gate: LK cross-platform closure]  ─►  OPT1 (substrate + heuristics-as-data)
14-linker (LK6 ✅) ───┘                                                        │
                                                                              ▼
                                                                         OPT2 (DCE / const-fold / copy-prop / peephole)   ◄── v1 ends here
                                                                              │
                          ┌───────────────────┬───────────────────┬──────────┴───────────┐
                          ▼                   ▼                   ▼                        ▼
                    OPT3 (GVN/DSE)     OPT4 (CFG-simplify)   OPT5 (LIR peephole/      (analyses feed all)
                                        + marker-repair)         coalescing)
                          │                                       │
                          └───────────────┬───────────────────────┘
                                          ▼
                              OPT6 (loops/LICM)  ─►  OPT7 (inlining)  ─►  OPT19 (scheduling)
                                          │
                                          ▼
                                     OPT9 (vectorization — frontier)
                                          │
                                          ▼
                                     OPT10 (autotuner — the endgame)
```

OPT2b (HIR transpile cleanup) hangs off OPT1's policy/pipeline mechanism but is **gated on plan 10** (the transpile path must exist); it is independent of OPT2's MIR tier and lands whenever transpilation does. OPT1 gates everything (the pass engine + the data facets). OPT2 is the first quality win and closes the native/MIR-downstream v1 scope. OPT3/OPT4/OPT5 are parallel after OPT2 (independent passes sharing the OPT1 analyses). OPT6→OPT8 build on the loop forest + machine model. OPT9/OPT10 are the research frontier. Per directive, the whole arc enters the [`00`](./00-compiler-implementation-plan%20-%20tbd.md) stepper **after** the cross-platform-compilation portion closes — "runs everywhere" before "runs fast everywhere."
