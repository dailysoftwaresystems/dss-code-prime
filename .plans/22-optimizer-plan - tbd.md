# In-tree Optimizer — Sub-Plan (22)

> Owns the **optimization layer**: a multi-tier set of transformation/analysis passes over HIR, MIR, and LIR, plus the per-target cost/machine models that parameterize them. ONE engine of universal algorithms (bucket-2) reading JSON-declared vocabulary (bucket-1) — there is **no per-language and no per-target optimizer C++**. Adding a target's tuning = drop numbers in its `.target.json`; adding a language costs nothing (everything is already neutralized at the HIR→MIR boundary).
>
> **Thesis extension (the load-bearing bet of this plan).** Optimization *heuristics* — the cost model, the pass-pipeline order, the profitability thresholds — are **data, not code**, from the first PR. That is what turns tuning from a decade of hand-work into a *searchable space*: the same property that lets a vendor ship a `.target.json` lets the optimizer eventually **tune itself** against a corpus. A frozen-C++ heuristic kills that endgame on arrival; a JSON heuristic preserves it for nearly free. This plan therefore treats "heuristics-as-data" as a non-negotiable substrate decision, not a future refactor.

## 0. Status (snapshot)

| | |
|---|---|
| Status        | ⏳ **READY-TO-OPEN 2026-06-03 (rev 1).** v1 ships the mandatory scalar subset; the full multi-tier roadmap + the autotuner research arc are scoped here and anchored as deferred cycles. **Maximal scope by directive** — HIR/MIR/LIR/per-target passes AND the learned-heuristics frontier all live in this one plan. **All gate conditions met**: (a) file-emission proven 2026-06-02 (`int42.c`, `hello_puts`, `hello_writefile`, `hello_printf` on Windows x86_64); (b) 14-example corpus closed 2026-06-03 (sum_loop, max_of_3, factorial, fibonacci, cmp_all_signed_conds, nested_loop, early_return, countdown_sum, multi_function, recursive_factorial, two_helpers, helper_before_main, loop_invariant, cse_candidate) provides distinct CFG/arithmetic shapes for differential verification of every OPT1 transform. |
| Predecessors  | ✅ [`12-mir-lir-plan`](./12-mir-lir-plan%20-%20ok.md) (MIR SSA-over-CFG + dominators-in-verifier + the `lir_pass_util` build-once-freeze pass pattern — ML1–ML8 closed). ✅ [`12.5-const-eval-plan`](./12.5-const-eval-plan%20-%20ok.md) (shared scalar const-eval engine — the MIR fold pass reuses its arithmetic core). ✅ [`14-linker-plan`](./14-linker-plan%20-%20tbd.md) (LK1–LK10 closed + RUNNABLE 2026-06-02 — file emission + D-LK10-ENTRY Stage 1 + D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY). ✅ Step 13.5 cycle 1+2 (2026-06-03 — D-CSUBSET-WHILE-LOOP-SUBSTRATE + 14 runnable corpus examples providing differential-verification gates). ✅ **D-OPT1-SYMBOL-BINDING-VISIBILITY-THREAD** (2026-06-03 — `SymbolBinding` / `SymbolVisibility` lifted from `link/object_format_schema.hpp` to `core/types/symbol_attrs.hpp`; threaded onto `MirFunc` + `MirGlobal` POD via the existing 4-byte pad slot; `isExternallyVisible(binding, visibility)` is the DCE-protect predicate the optimizer's DCE pass MUST consult before deleting any function/global. c-subset front-end defaults Global+Default for every user function/global — the C-without-`static` convention — so DCE today preserves everything. Closes plan-22 §2.9 prerequisite for OPT1's DCE step). **All upstream gates met; OPT1 unblocked.** |
| Project gate  | ✅ **GATE MET 2026-06-02** (file emission) + ✅ **CORPUS MET 2026-06-03** (14 runnable c-subset examples). **Prior gate-status note** (2026-05-30, since closed): the linker then produced only in-memory `LinkedImage::bytes` and intra-module relocs (LK6 c1) — no on-disk executable file. **Closure path**: LK10 cycle 2 file emission + D-LK10-ENTRY Stage 1 entry trampoline + D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY Win64 frame-bias closure landed runnable on-disk binaries; step 13.5 cycle 1+2 closed the while-loop / multi-fn / setcc-width substrate + the 14-example corpus. Rationale honored: "it runs everywhere" (file that loads + runs ✅) is in before making it run *fast* everywhere. |
| Successors    | Feeds the same backend it always did — [`13`](./13-assembler-plan%20-%20tbd.md) / [`14`](./14-linker-plan%20-%20tbd.md) consume optimized MIR/LIR transparently. [`17`](./17-shader-gpu-plan%20-%20tbd.md) (SPIR-V) + [`18`](./18-wasm-plan%20-%20tbd.md) share the MIR optimizer verbatim (the config-thesis multiplier — one pass, every backend). |
| Scope         | **Bounded for v1; roadmap maximal.** v1 mandatory: OPT1 (substrate + heuristics-as-data) + OPT2 (DCE / const-fold / copy-prop). v1.x–v2: OPT3–OPT8 (redundancy, control-flow, LIR peephole/coalescing, loops, inlining, scheduling). Research frontier: OPT9 (vectorization), OPT10 (the autotuner). Every non-v1 item is an anchored D-OPT deferral, not a silent gap. |

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
│   ├── pass_cfg_simplify.cpp      # OPT4 — unreachable-block / block-merge / jump-thread (marker-safe)
│   ├── pass_licm.cpp              # OPT6 — loop-invariant code motion
│   └── pass_inline.cpp            # OPT7 — inlining (profitability from cost model)
├── lir/
│   ├── pass_lir_peephole.cpp      # OPT5 — target-aware peephole (rewrite table over real opcodes)
│   ├── pass_coalesce.cpp          # OPT5 — register coalescing (cut the 2addr/isel mov traffic)
│   └── pass_schedule.cpp          # OPT8 — list scheduler over the per-target machine model
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

Three passes touch *symbols*, not just SSA values — DCE (of whole functions/globals), inlining, and dead-store elimination — and all three are silent-miscompile seams if they ignore **linkage**. MIR today carries only a `SymbolId` per function/global; it has thrown linkage away. **OPT1 closes this as a committed deliverable (not a deferral):** it threads a **format-neutral** linkage attribute onto every MIR function/global, reusing the *already format-blind* `SymbolBinding{Local,Global,Weak}` + `SymbolVisibility{Default,Hidden,Protected,Internal}` vocabulary from the linker substrate ([`14`](./14-linker-plan%20-%20tbd.md) LK4, `object_format_schema.hpp`). The HIR→MIR boundary maps the HIR `FfiLinkage`/`FfiVisibility` onto these enums (the mapping — including the `Common`/`Internal` edge cases — is pinned in OPT1, never an unhandled silent arm). **This is precisely how the optimizer stays linker-agnostic: a pass reads `binding == Global` / `visibility != Hidden`, never `if (format == Elf)`.**

The liveness/legality rules these enums gate:

- **DCE liveness roots** — whole-symbol DCE may eliminate a function/global *only* when it is `Local` binding **and** `Hidden`/`Internal` visibility **and** has no in-module use **and** is not address-taken (`GlobalAddr`). Externally-visible symbols (`Global`/`Weak` binding, or `Default`/`Protected` visibility), the declared entry point, and any address-taken symbol are **always live roots** — even with zero local uses. (`hasSideEffects` governs *instruction*-level DCE; it says nothing about whole-symbol reachability — independent gates.)
- **Inline legality** — never inline a `Weak` callee (a strong definition may replace it at link time; inlining bakes in the wrong body). When inlining an externally-visible or address-taken callee, its out-of-line definition stays a DCE root (it is *not* "now unused"). Cross-CU inlining is separately blocked on LK11/CU6 (D-OPT7-1).
- **DSE escape guard** — a store is *not* dead if its target may be observed externally: a store through a pointer derived from a `GlobalAddr` of a `Default`/`Protected`-visible global, or through a pointer escaped via `Call`, survives. DSE needs a may-escape/visibility test — it cannot lean on `hasSideEffects` the way DCE does (its whole job is removing side-effecting stores it proves redundant).

Violations are loud, not silent: `X_LiveSymbolEliminated` and `X_IllegalInlineOfWeak` (§2.10) fire at the offending pass. There is no volatile/atomic axis on the MIR memory opcodes today; until one exists (D-OPT4-2), value-based branch folding (OPT4) is bounded to *pure SSA* conditions and never treats a memory-derived `Load` as invariant.

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
| OPT4 | MIR control-flow simplification | `pass_cfg_simplify` — unreachable-block elimination, block merging, jump threading, branch folding — **with StructCfMarker repair** (§2.7). The marker-repair logic is the load-bearing piece (D-OPT4-1). |
| OPT5 | LIR target-aware opts | `pass_lir_peephole` (rewrite table over real opcodes — cuts redundant movs / setup) + `pass_coalesce` (register coalescing — directly reduces the mov traffic from 2addr-legalize + naive isel) + address-mode folding. First consumer of the `costModel` facet. |
| OPT6 | Loop optimization | `analysis/mir_loop_forest` (natural loops, marker-cross-checked) + `pass_licm`. Unrolling anchored separately (profitability from `costModel`). |
| OPT7 | Inlining (interprocedural) | Call-graph construction + `pass_inline` with a **data-driven** profitability threshold (from `costModel`, not a C++ constant). **Inline legality gates on the §2.9 linkage attribute:** never inline a `Weak` callee; an externally-visible / address-taken callee keeps its out-of-line body (not DCE'd). Couples with [`11-ffi-plan`](./11-ffi-plan%20-%20tbd.md) for cross-CU visibility limits. |
| OPT8 | Instruction scheduling | `pass_lir_schedule` — list scheduler over the per-target `machineModel` (ports + latencies as JSON). Pure bucket-2: one scheduler, every target's numbers. |
| OPT9 | Vectorization (frontier) | SLP + loop vectorization. Legality via a dependence-analysis framework (universal); profitability via `costModel`. The deep end — explicitly research-grade. |
| OPT10 | The autotuner (research arc) | Corpus + measurement loop (leveraging the round-trip oracle + self-host as ground truth) + search over `costModel`/`pipeline` config; optional learned cost model. Output = generated `.target.json` cost blocks. The "climb past frozen hand-tuning" endgame. |

Substrate tier (5-agent review) for **OPT1** (the pass engine + the heuristics-as-data facets — the contract everything else builds on). Feature tier for OPT2+.

**v1 = OPT1 + OPT2.** OPT3–OPT10 are anchored deferrals (§3.1), each with an owner cycle + trigger — not a silent backlog.

---

## 3.1 Deferred-items registry (OPT)

Mirrors plan 12 §3.1 / plan 13 §3.1 / plan 14 §3.1. Every deferred item names the specific future PR/cycle that closes it + a gate condition. Struck through when the closing commit lands.

| # | Deferred item | Why deferred (not a silent gap) | Owner / closure | Trigger |
|---|---|---|---|---|
| D-OPT1-DCE-NEGATIVE-PIN | **`examples/c-subset/dce_negative_pin/` differentially verifies DCE never deletes a live store.** Live `x = 100` survives the branch-not-taken arm; exit 100. A DCE that elides the first store returns 0 (or garbage). The corpus pin lights up the moment differential verification wires (D-OPT1-DIFFERENTIAL-VERIFY-RUNNER). | **OPT2's DCE pass cycle.** | First DCE rewrite landing. |
| D-OPT1-CONST-FOLD-MIR-PIN | **`examples/c-subset/const_fold_inside_expr/` pins MIR-tier const-fold + live-edge preservation.** Foldable `5 + 3` inside `x*2 + a` (global live edge); exit 42. A fold that drops the live edge returns 16. | **OPT2's const-fold pass cycle.** | First const-fold rewrite landing. |
| D-OPT1-COPY-PROP-JOIN-PIN | **`examples/c-subset/copy_prop_across_join/` pins copy-prop respects phi joins.** `b = a; ...; a = 999; return b` with the copy on a single arm; exit 10. A phi-blind copy-prop returns 999. | **OPT2's copy-prop pass cycle.** | First copy-prop rewrite landing. |
| D-OPT1-LICM-CONDITIONAL-PIN | **`examples/c-subset/licm_conditional_mutation/` pins LICM respects conditional mutations inside the loop.** `a * b` looks invariant but `a` is conditionally incremented mid-loop; exit 68. A naive LICM hoisting `a * b` returns 60 (visibly wrong — same as the positive loop_invariant pin). | **OPT6's LICM pass cycle.** | First LICM rewrite landing. |
| D-OPT1-CSE-NONCOMMUTATIVE-PIN | **`examples/c-subset/cse_noncommutative/` pins CSE respects operator non-commutativity.** `(a - b) * 2 + (b - a) * 3 + 60`; exit 58. A CSE that merges `(a - b)` and `(b - a)` returns 70 (12-point exit-code distance — bisectable). | **OPT3's CSE/GVN pass cycle.** | First CSE/GVN rewrite landing. |
| D-CSUBSET-CMP-COND-CORPUS | **`examples/c-subset/cmp_all_signed_conds/` exercises all 6 signed `TargetCondCode` arms in one binary.** With `a=5, b=7`: bit-packed `r = 0 + 2 + 4 + 8 + 0 + 0 = 14` uniquely identifies which arms branched correctly. A single wrong nibble in `condCodeEncoding[]` flips one bit; the bug names itself in the exit-code diff vs `exitCode: 14`. Stresses BlockRel32 per-function patch list across multiple sequential branches AND the jcc compound encoding (`0F 8x rel32; E9 rel32`) under repetition. Becomes a future OPT1 (13.6) differential-verification gate: const-fold's `if (5 < 7)` rewrite hits all 6 signed arms in ONE differential run. | **OPT2 const-fold cycle** (differential-verify hit) AND/OR ARM64 cond-code table landing (cross-CPU pin). | First const-fold rewrite OR D-AS3-COND-CODE-ARM64 closure. |
| D-CSUBSET-UNSIGNED-CMP-CORPUS | **Unsigned cond-code companion to D-CSUBSET-CMP-COND-CORPUS.** Mirrors the signed witness across the 4 unsigned arms (Ult/Ule/Ugt/Uge) once c-subset gains `unsigned` type support. Same bit-packed witness shape; exit-code-bisectable. | **c-subset `unsigned` type cycle.** | First c-subset unsigned-comparison binary. |
| D-OPT1-DIFFERENTIAL-VERIFY-RUNNER | **Examples_runner extension** — add optional `optimizedPipelines: []` array to `expected.json` schema + a 2nd compile-run inside `RunFromManifest` that invokes the optimizer pipeline before running the binary and asserts the SAME `exitCode` + `expectedStdout`. The 5 corpus negative pins (above) are PREPARED for this — each documents the buggy-OPT exit code so a differential test surfaces a regression bisectably. ~15 LOC change; lands with OPT2's first real pass so OPT2 ships with differential verification day-one (test-analyzer NOW criticality 7). | **OPT2 cycle 1** (alongside the first non-Identity pass). | First non-Identity pipeline landing. |
| D-OPT2-DCE-LINKAGE-SYMTAB-ASSERTION | **Symbol-table assertion for DCE's linkage contract.** The behavioral pin (`dce_negative_pin`) catches DCE eliding a *live* store. The orthogonal bug — DCE deleting an *exported-but-internally-unused* symbol — is invisible in a self-contained run because `main` still exits correctly while the dropped symbol leaves no runtime trace. Needs: (a) a corpus example with an externally-visible symbol (non-static helper, or `D-LANG-EXPORT`-marked global) that has no internal use, AND (b) a test-side assertion that the emitted binary's symbol table CONTAINS the symbol after the DCE pass. Substrate is already in place: `isExternallyVisible(binding, visibility)` predicate on `core/types/symbol_attrs.hpp` + `binding`/`visibility` threaded onto `MirFunc` + `MirGlobal`. DCE pass MUST consult the predicate before deletion; this anchor pins the test side. | **OPT2 DCE pass cycle** alongside the behavioral corpus pin. | First DCE rewrite landing. |
| D-OPT1-PIPELINE-VALUE-TYPE | **`OptPipeline` → value-type with validating factory** when `vector<PassId>` grows to `vector<PassRun>` (D-OPT1-PASS-RUN-MAX-ITER). `static OptPipeline::create(string name, vector<PassRun> runs) → expected<OptPipeline, Diagnostic>` validating (a) non-empty, (b) no contradictory duplicate (same PassId with conflicting maxIterations adjacent), (c) name non-whitespace. | **D-OPT1-PASS-RUN-MAX-ITER closure.** | First fixed-point pass landing. |
| D-OPT1-OPT-RESULT-SHAPE | **`bool` return → `OptResult { bool ok; size_t passesRun; size_t passesMutated; bool fixedPointReached; }`** when `passesMutated == 0` becomes a meaningful signal (the differential-verify harness can short-circuit unchanged optimizer runs). | **OPT2's first mutating pass.** | First MIR mutation by an optimizer pass. |
| D-OPT1-PASS-DUP-POLICY | **Explicit decision on `pipeline.passes = {ConstFold, ConstFold}`.** Today: silently legal (just runs twice). Likely correct (chained const-fold can converge fixed-point); make it explicit at OPT2 alongside maxIterations. | **OPT2.** | First multi-instance pipeline. |
| D-OPT1-X-UNKNOWNPASSID-UNIT-PIN | **Unit test that constructs `OptPipeline{{ PassId(99) }}` and asserts `X_UnknownPassId` fires.** Trivially addressable today, but the guard's REAL trigger is "future PassId enum drift" — a single-arm `PassId(99)` is a tautology in cycle 1. Anchor to OPT2 where the second enumerator (`ConstFold`) lands and the negative-pin becomes meaningful. | **OPT2 cycle 1.** | Second PassId enumerator. |
| D-OPT1-RETURN-FALSE-CONTRACT-UNIT-PIN | **Unit test for the entryErrorCount snapshot guard.** Needs a mock pass that returns false silently — either friending the test or exposing `runPass` via a header. Both add substrate surface for a guard that only fires on future regression. | **OPT2 cycle 1** alongside the first real pass + its test fixture. | First real pass's test landing. |
| D-OPT1-MIR-UNIT-TESTS | **MIR-tier unit tests for `optimize()`** — "construct Mir, run optimize, assert byte-equal" + post-pass MIR-equality helper for negative-pin pre/post comparison. | **OPT2.** | First MIR mutation requiring direct MIR-equality assertion. |
| D-OPT1-PIPELINE-FROM-CONFIG | **Pipeline name → PassId list comes from JSON.** Cycle 1 hardcodes `OptPipeline{ "default", { Identity }}` in `compile_pipeline.cpp`. Per plan 22 §2.5 (per-output-path policy), the pipeline name comes from the artifact-profile config; the loader resolves name → PassId list from `src/dss-config/pipelines/*.pipeline.json`. | **OPT2's first real pass** (when more than `Identity` exists to put in a pipeline). | First non-Identity pass landing. |
| D-OPT1-RETURN-FALSE-DIAGNOSTIC-CONTRACT | **`optimize()` returning false MUST be paired with at least one new error in the reporter.** Cycle 1 enforces this via a snapshot+assertion belt-and-suspenders in `optimizer.cpp::optimize`: entryErrorCount captures `reporter.errorCount()` on entry; any false-return path that didn't bump errorCount triggers an emit-then-return — preventing future failure paths from being silently swallowed by `tierClean(reporter, optEntry)`. | **First non-Identity pass** that introduces a new false-return path. | New PassId enumerator whose `runPass` arm can fail. |
| D-OPT1-PASS-RUN-MAX-ITER | **`vector<PassId>` will grow to `vector<PassRun>` when a fixed-point pass needs per-pass iteration count.** OPT2's copy-prop iterates over phi-webs until stable; the `maxIterations` cap belongs to the pass invocation, not the pipeline. `PassRun { PassId id; uint16_t maxIterations = 1; }` is the obvious shape. | **OPT2 copy-prop** (first fixed-point pass). | First pass that needs to iterate until stable. |
| D-OPT1-PIPELINE-BUDGET | **Pipeline-level time/instruction-count budget** — OPT10 autotuner concern, not OPT1+OPT2. | **OPT10**. | OPT10 search loop. |
| D-OPT1-PIPELINE-MAX-ITERATIONS | **Pipeline-level fixed-point cap** — when the WHOLE pipeline is run to fixed point (some compilers do this for `-O3+`). | **An OPT-x cycle** where pipeline-fixed-point matters. | First whole-pipeline-fixed-point requirement. |
| D-OPT1-PASS-ID-STABILITY | **PassId ordinal stability contract.** OPT1 cycle 1 (2026-06-03) ships `enum class PassId : uint8_t { Identity = 0 }`. Ordinals are part of the pipeline-as-config wire contract — pipelines reference passes by NAME (`optPassIdFromName`) but the ordinals here are internal. Adding a pass APPENDS to the end; never renumbers (renumbering would silently re-bind shipped pipelines to the wrong pass). | **Every future cycle that adds a PassId enumerator.** | New pass landing in `enum class PassId`. |
| D-OPT1-VERIFY-AFTER-EVERY-PASS | **MIR verifier runs after every pass under ALL build modes** (not Debug-only — plan 22 §3 PR1 "unconditional" directive). Cycle 1's no-op identity pass cannot break MIR invariants, so the verifier slot is documented but not yet active. When OPT2's first real pass lands, the verifier call lands in `optimizer.cpp::optimize`'s per-pass loop. | **OPT2's first real pass** (const-fold / DCE / copy-prop — whichever lands first). | First non-Identity pass in the dispatcher. |
| D-OPT1-0 | **CLI `--config=release` wiring slot already live** (LK10 cycle 3, 2026-06-01). `src/program/cli_args.{hpp,cpp}` exposes `CompileConfig {Debug=0, Release=1}` enum parsed from `--config=<value>`. Today the value is stored on `CliArgs` and threaded into the pipeline as a no-op (default `Debug`); when OPT1 ships the substrate, the pass-pipeline selection reads `CompileConfig` (or the eventual single source of truth) to pick Release-pipeline vs Debug-pipeline. The CLI surface is stable in advance of plan 22; no driver-tier churn needed when OPT1+ land. | **OPT1 substrate PR** — first pass-pipeline that branches on Release vs Debug. | OPT1 ships. |
| D-OPT1-1 | **`costModel`/`machineModel` numbers are hand-set, not measured.** v1 ships plausible defaults in JSON; they are not empirically tuned. | **OPT10** (autotuner) replaces hand-set numbers with searched ones. The *shape* is correct from OPT1; only the values are provisional. | First autotuner corpus run. |
| D-OPT1-2 | **Analysis invalidation is recompute-on-demand, not incremental.** OPT1 recomputes dominators/use-list per pass that needs them — *correct*, just not yet incremental. Fine for v1 module sizes. (Not a workaround: the recompute path is the long-term-correct baseline; incremental update only refines compile-time.) | **The OPT10 corpus cycle** — the first place large-module compile time is actually measured; incremental analysis lands there as one substrate PR (a named landing spot, not a floating "when it matters"). | First module where pass-pipeline recompute dominates compile time. |
| D-OPT2-1 | **SCCP (sparse conditional constant propagation) full lattice** — OPT2 ships straight fold + propagation; the conditional-branch-aware lattice (folding values proven constant only on a taken edge) is stronger. | **OPT3** (alongside GVN — both are value-lattice passes sharing machinery). | First test where a constant is provable only via branch conditions. |
| D-OPT2b-1 | **Richer transpile optimization profiles** beyond on/off `transpile-readable` — e.g. "fold constants but keep all functions", "strip dead code, preserve doc-comments-as-attributes". | **An OPT2b follow-up** — each profile is a new named `*.pipeline.json`, no engine change (heuristics-as-data). | First transpile consumer asking for partial cleanup. |
| D-OPT5-1 | **WASM/SPIR-V-specific peephole + cost tier** — MIR-downstream backends inherit all MIR opts, but a value-stack (WASM) / result-id (SPIR-V) peephole over their own emitted form could squeeze further; needs a small per-backend cost model. | **A cycle in [`17`](./17-shader-gpu-plan%20-%20tbd.md) / [`18`](./18-wasm-plan%20-%20tbd.md)** when their byte output is profiled. | First WASM/SPIR-V output where MIR-level opt leaves obvious local waste. |
| D-OPT4-1 | **StructCfMarker repair on CFG restructuring** — jump-threading/block-merge must recompute markers, not just preserve trivially. The repair algorithm (re-derive marked regions from the post-transform CFG + dominators) is the hard part of OPT4. | **OPT4** core scope (this is OPT4's central deliverable, called out so the 5-agent review inherits it). | First CFG-simplify pass that merges a marked block. |
| D-OPT4-2 | **No volatile/atomic memory axis on the MIR memory opcodes.** Until one exists, value-based branch folding / LICM treat only *pure SSA* conditions as invariant and never hoist/fold a memory-derived `Load` (a correct, conservative bound — not a workaround). The axis itself (a flag on `Load`/`Store`, threaded from a HIR memory attribute) is a real future need. | **The cycle that first lowers a `volatile`/atomic-bearing source language** — a HIR memory-attribute → MIR opcode-flag thread. Genuinely blocked: no v1 source language exposes `volatile`/atomics. | First `volatile`/atomic in a source program. |
| D-OPT6-1 | **Loop unrolling + unswitching** — OPT6 ships loop forest + LICM; unrolling (profitability-gated by `costModel`) and unswitching follow. | **An OPT6 follow-up cycle** when the loop forest is proven on real loops. | First loop benchmark where LICM alone underperforms. |
| D-OPT7-1 | **Cross-CU inlining** — OPT7's call-graph is single-CU (matches the v1 single-CU-per-image contract, plan 14 §2.12). Cross-module inlining needs the CU-merge that [`14`](./14-linker-plan%20-%20tbd.md) LK11 + [`08`](./08-compilation-unit-plan%20-%20tbd.md) CU6 provide. | **Paired with LK11 / CU6** (cross-CU linking). | First multi-CU image with a hot cross-module call. |
| D-OPT9-1 | **Dependence-analysis framework** — vectorization legality needs array/memory dependence testing (GCD/Banerjee-class). Substantial standalone analysis. | **OPT9** core scope. | First vectorization test. |
| D-OPT10-1 | **Learned cost model** — OPT10 starts with random/evolutionary search over the existing JSON facets; a learned (ML) cost model that predicts the objective is a further step. | **An OPT10 follow-up** once the search loop + corpus demonstrate the JSON-search baseline closes some of the gap. | First evidence the config-search baseline beats hand-tuned defaults. |
| D-OPT10-2 | **Objective definition + Pareto handling** — "beat LLVM" requires a chosen objective (size/cycles/energy) and a policy for trade-offs. Single-objective first. | **OPT10** design cycle. Open Question #5. | Autotuner design start. |

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
                              OPT6 (loops/LICM)  ─►  OPT7 (inlining)  ─►  OPT8 (scheduling)
                                          │
                                          ▼
                                     OPT9 (vectorization — frontier)
                                          │
                                          ▼
                                     OPT10 (autotuner — the endgame)
```

OPT2b (HIR transpile cleanup) hangs off OPT1's policy/pipeline mechanism but is **gated on plan 10** (the transpile path must exist); it is independent of OPT2's MIR tier and lands whenever transpilation does. OPT1 gates everything (the pass engine + the data facets). OPT2 is the first quality win and closes the native/MIR-downstream v1 scope. OPT3/OPT4/OPT5 are parallel after OPT2 (independent passes sharing the OPT1 analyses). OPT6→OPT8 build on the loop forest + machine model. OPT9/OPT10 are the research frontier. Per directive, the whole arc enters the [`00`](./00-compiler-implementation-plan%20-%20tbd.md) stepper **after** the cross-platform-compilation portion closes — "runs everywhere" before "runs fast everywhere."
