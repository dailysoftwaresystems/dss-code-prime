#pragma once

// MIR-tier function INLINING (OPT7 — the first interprocedural pass).
//
// **Scope (OPT7 cycle 1 — LOCKED minimal slice)**: inline a DIRECT
// call to a SINGLE-BLOCK LEAF callee whose body splices LINEARLY into
// the caller's block at the call site (no CFG merge — the simplest SSA
// splice). Everything else is DEFERRED to a later OPT7 cycle and is
// NOT built here (subsequent cycles LANDED most of these — see below):
//   - the inline COST MODEL: a SIZE-based bloat bound — `OptPipeline::
//     inlineThreshold` (default 50, config-driven via the pipeline JSON)
//     — LANDED in OPT7 cycle 28; the gate now refuses a callee whose
//     instruction-count exceeds the threshold, and `Inlining` SHIPS in
//     `release.pipeline.json` behind that bound. The SOPHISTICATED cost
//     model (call-site hotness, growth-vs-benefit) remains deferred
//     (D-OPT7-INLINE-LEGALITY-GATE);
//   - general MULTI-BLOCK callee splice + StructCfMarker composition
//     (D-OPT7-MULTIBLOCK-SPLICE — LANDED cycle 2);
//   - recursion INLINING (cycle 1 REFUSED self-recursion; cycle 3
//     generalized to refuse all recursive cycles via the call-graph SCC
//     gate; depth-bounded recursive inlining remains deferred);
//   - cross-CU inlining (D-OPT7-1 — LANDED cycles 25/26 via the
//     whole-program MIR merge).
//
// **§2.9 LEGALITY GATE** (the CORRECTNESS deliverable). A direct call
// to callee F is inlined ONLY IF ALL of the following hold; otherwise
// the call is conservatively REFUSED (left exactly as-is — never a
// silent miscompile):
//   1. F resolves to a DEFINED MirFunc in THIS module (the callee
//      operand is a `GlobalAddr` whose SymbolId maps to a function).
//   2. `funcBinding(F) != SymbolBinding::Weak` — **THE correctness
//      rule**. A Weak definition may be REPLACED by a strong definition
//      of the same name at link time; inlining F's weak body would bake
//      in the wrong one (a silent miscompile). This is the rule the
//      D-OPT7-WEAK-INLINE-NEGATIVE-PIN corpus proves end-to-end.
//   3. The call is NOT self-recursive: `funcSymbol(F) !=
//      funcSymbol(caller)` (mutual-recursion / depth policy deferred —
//      at minimum, never inline a call whose callee is the caller).
//   4. F's address does NOT escape: every live `GlobalAddr(F.symbol)`
//      in the module is used ONLY as operand[0] of a Call. A taken
//      function pointer means an indirect call could reach F, so the
//      out-of-line body must be preserved AND we refuse to inline
//      (conservative).
//   5. F's body is SPLICE-ELIGIBLE. The cycle-1 minimal slice (a
//      SINGLE-BLOCK LEAF) has since been generalized: multi-block
//      callees inline via the CFG-clone + return-merge-Phi machinery
//      (cycle 2); a callee containing a regular `Call` (non-leaf) is
//      admitted (cycle 3); a callee containing an `IntrinsicCall` is
//      admitted (cycle 6); a callee containing a `Phi` is admitted
//      (cycle 7 — cloned via a deferred-incoming-flush: value remapped
//      via the shared `local` map, pred via the callee-block clone map;
//      D-OPT7-MULTIBLOCK-SPLICE-PHI). What REMAINS refused: a callee with
//      NO returning path, a recursive-cycle call (the call-graph SCC
//      gate, rule 3), and a callee whose instruction-count exceeds the
//      cost bound (cycle 28).
//      The IntrinsicCall admission carries a frame-sensitivity caveat —
//      a frame-sensitive intrinsic (va_start / frameaddress / setjmp-
//      class) must NOT be inlined — but no shipped frontend emits any
//      intrinsic today, so blanket admission is correct for the current
//      model; per-intrinsic inline-safety gating is trigger-gated to the
//      first frame-sensitive intrinsic — D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC.
//
// **NEVER DELETE a callee body in this pass.** OPT7 inlines call
// SITES only. A now-dead callee is removed by a LATER DCE pass, which
// already preserves externally-visible roots via
// `isExternallyVisible`. "Keep address-taken / externally-visible
// bodies" is therefore satisfied by simply not deleting them here.
//
// **SPLICE MECHANICS (single-block leaf — SSA-preserving)**. A Call is
// a non-terminator, value-producing opcode, so it flows through the
// shared `MirFunctionRebuilder`'s `tryRewrite` hook. When the policy
// recognizes an eligible Call, instead of copying the Call verbatim it:
//   (a) walks the callee's single block in order, copying each
//       NON-Arg / NON-terminator instruction into the caller's CURRENT
//       block via a LOCAL `calleeOld → callerNew` map. Leaves
//       (`Const` / `GlobalAddr`) re-emit through their dedicated
//       builders; other ops re-emit via `addInst` with operands mapped
//       through the local map (and `Arg(i)` → the call's actual
//       argument, i.e. the caller-NEW value of the Call's operand
//       [1 + i]);
//   (b) returns the callee `Return`'s mapped value as the Call's
//       result (or, for a void callee with a bare `Return`, the Call
//       had no result and nothing downstream reads it).
// Because the callee block is already in def-before-use order and has
// no control flow, splicing it linearly at the call site preserves
// SSA: every spliced def precedes its uses, and the threaded return
// value is the single live exit. The engine's
// D-OPT1-VERIFY-AFTER-EVERY-PASS hook re-runs `MirVerifier` on the
// rebuilt module, so any splice that broke an invariant is a build
// break, not a runtime miscompile.
//
// **Fail-loud**: a call selected for inlining whose argument count
// does not match the callee's Arg-parameter count is a structural MIR
// violation (HIR→MIR pairs args 1:1 with the signature) — it emits
// `X_InlineMalformedCallSite` and the pass returns ok=false rather
// than splicing a wrong-arity body.
//
// **Agnostic**: the pass reads only MIR opcodes + `SymbolBinding` +
// the call/callee structure. No source-language, target-CPU, or
// object-format identity branch — inlining is a universal MIR→MIR
// rewrite.
//
// **Runtime-init globals carve-out**: same shape as the other MIR-tier
// passes via `cloneGlobalsOrCarveOut`.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace dss::opt::passes {

struct InliningResult {
    bool        ok             = false;
    std::size_t callsInlined   = 0;
    // Call sites that passed EVERY correctness and per-callee rule and were
    // refused ONLY by the per-caller cumulative growth budget. Purely an
    // observation channel — the engine does not branch on it — but it is the
    // difference between "the budget is doing nothing" and "the budget is
    // doing everything", which is not otherwise visible from outside.
    std::size_t callsBudgeted  = 0;
};

// ★★★ THE PER-CALLER CUMULATIVE GROWTH LEDGER.
//
// The Inlining pass is STATELESS across invocations — the engine calls
// `runInlining` afresh on every fixpoint iteration. That is exactly why the
// growth bound could not live inside the pass: a budget recomputed against
// each iteration's ENTRY size compounds, `(1 + g)^iterations`, and the
// iteration cap goes straight back to being the real bound. This ledger is
// the memory that makes the budget CUMULATIVE: it records each caller's
// ORIGINAL instruction count — the size it had the first time the pass saw
// it in this `optimize()` call — so every later iteration measures growth
// against the SAME fixed origin, and each caller's ceiling never moves.
//
// ⇒ total module size is bounded by SUM over callers of (original +
// allowance) no matter how many iterations run. That, and not the iteration
// cap, is what makes the fixpoint terminate.
//
// KEYED BY `SymbolId`, and the choice is load-bearing on both sides:
//   * it SURVIVES a rebuild. Every pass reconstructs the module through
//     `MirBuilder`, so `MirFuncId` ordinals shift the moment DCE deletes a
//     dead function — but `addFunction` carries the function's SymbolId
//     through unchanged, so the symbol is the one handle that means the same
//     function on both sides of a rebuild.
//   * it is UNIQUE IN A MERGED MODULE. SymbolId is documented as CU-scoped,
//     which would be fatal here at the program stage (103 CUs, colliding
//     ids, two functions sharing one budget). ✔MEASURED that the merge closes
//     this: `mergeCuMirs`'s `SymbolAllocator` mints one merged id per
//     distinct symbol and ABORTS on a collision, so within any module the
//     optimizer ever sees, symbol identity IS function identity. It is the
//     same handle the pass already trusts for the self-recursion rule.
//
// ONE LEDGER PER `optimize()` CALL. The unit and program stages each get a
// fresh one, so a function's origin at the program stage is its
// post-unit-stage size. That is deliberate and matches how an LTO re-summary
// behaves: each stage gets its own budget rather than one stage's spending
// silently mortgaging the other's.
class InlineGrowthLedger {
public:
    // The caller's ORIGINAL instruction count. The FIRST call for a symbol
    // records `currentInsts` and returns it; every later call returns the
    // recorded value and IGNORES `currentInsts`. Idempotent by construction —
    // there is no way to spell "re-baseline this caller", because that is the
    // bug this class exists to prevent.
    [[nodiscard]] std::uint32_t
    originalInsts(SymbolId caller, std::uint32_t currentInsts) {
        auto const [it, inserted] = original_.try_emplace(caller.v, currentInsts);
        (void)inserted;
        return it->second;
    }

    [[nodiscard]] std::size_t trackedCallers() const noexcept {
        return original_.size();
    }

private:
    std::unordered_map<std::uint32_t, std::uint32_t> original_;
};

// `inlineThreshold` is the size-based COST bound (OPT7 cycle 28): a
// callee is inlined ONLY IF its instruction-count is `<= inlineThreshold`
// (counted across ALL blocks during the legality gate's body scan); a
// larger callee is conservatively REFUSED. The production caller threads
// `OptPipeline::inlineThreshold` (config-driven via the pipeline JSON);
// tests pass `opt::kMaxInlineThreshold` (a permissive value) to inline the
// tiny fixtures, or a value BETWEEN two callee sizes to exercise the
// refusal boundary. A threshold of 0 is impossible from the loader (it
// rejects 0) but, if constructed programmatically, refuses everything
// (fail-safe). FAIL-SAFE: a threshold below the smallest callee refuses
// all inlining; nothing miscompiles.
// `maintainMarkers` (default true = the developer/test posture): re-stamp every
// block's StructCfMarker from the canonical CFG derivation after the splice.
// Markers feed ONLY the verifier (no optimization decision or codegen reads
// them; pass rebuilds copy them through verbatim), so a pipeline that does NOT
// verify after every pass (`OptPipeline::verifyEveryPass == false`, the release
// posture) passes false here and the optimizer re-derives ONCE after the whole
// pipeline instead — the whole-module derivation was ~91% of this pass's cost
// on SQLite (the D-OPT1-VERIFY-FREQUENCY-CONFIG posture split, applied to
// marker maintenance).
// ── THE PRODUCTION ENTRY POINT ────────────────────────────────────────
//
// `callerGrowthPercent` + `ledger` together are the per-caller CUMULATIVE
// growth budget (`opt::kDefaultInlineCallerGrowthPercent` carries the whole
// argument for why it is per-caller and not per-module). A caller may grow,
// across every invocation that shares this `ledger`, by at most
//
//     allowance = max(original * callerGrowthPercent / 100, inlineThreshold)
//
// instructions over its ORIGINAL size, and a call site whose callee does not
// fit in what is left is REFUSED — conservatively, exactly like every other
// gate refusal: the Call stays, the program compiles, nothing miscompiles.
//
// The `inlineThreshold` FLOOR is not slack for its own sake. Without it the
// per-callee threshold would be a lie for small callers — a 4-instruction
// wrapper could never absorb the 30-instruction helper the threshold says is
// inlinable — and wrappers are where inlining pays most.
//
// ORDER-INDEPENDENCE, which is the property that made this per-caller: each
// caller's ceiling is a function of its OWN original size and nothing else,
// so no caller can consume another's budget and the result does not depend
// on which function the walk reaches first. Within a caller the budget is
// spent in the module's own block-then-instruction order, which is a fixed
// property of the MIR rather than of the scheduler. A module-wide counter
// would have neither property — and at the unit stage, where CUs are
// optimized CONCURRENTLY, it would not even be deterministic.
//
// `always_inline` (TF-C81) WAIVES the budget, as it already waives
// `inlineThreshold` — the attribute overrides profitability vetoes and this
// is one. Such a splice still CHARGES the budget, so it reduces what
// ordinary sites in the same caller may take; charged but never refused.
[[nodiscard]] DSS_EXPORT InliningResult
runInlining(Mir& mir, TypeInterner const& interner,
            DiagnosticReporter& reporter, std::uint32_t inlineThreshold,
            std::uint32_t callerGrowthPercent, InlineGrowthLedger& ledger,
            bool maintainMarkers = true);

// ── THE SINGLE-INVOCATION ENTRY POINT ─────────────────────────────────
//
// Exactly equivalent to constructing a fresh `InlineGrowthLedger`, running
// ONE invocation against it at `opt::kDefaultInlineCallerGrowthPercent`, and
// discarding it. For a caller that runs the pass ONCE — every hand-built
// test fixture — that is not an approximation of the production semantics,
// it IS the production semantics: with one invocation, "size at pass entry"
// and "original size" are the same number.
//
// ⚠ NOT FOR A PIPELINE. A caller that invokes the pass repeatedly and reaches
// for this overload gets a budget that RE-BASELINES every iteration, which
// compounds to `(1 + g)^iterations` and hands the iteration cap back its role
// as the real growth bound — the precise defect
// `D-OPT-INLINING-FIXPOINT-TRUNCATES-BEFORE-CONVERGING` records. The engine
// has exactly one call site and it uses the ledger overload above;
// `Optimizer.InlineGrowthBudgetSpansFixpointIterations` in tests/opt fails if
// that is ever swapped.
[[nodiscard]] DSS_EXPORT InliningResult
runInlining(Mir& mir, TypeInterner const& interner,
            DiagnosticReporter& reporter, std::uint32_t inlineThreshold,
            bool maintainMarkers = true);

} // namespace dss::opt::passes
