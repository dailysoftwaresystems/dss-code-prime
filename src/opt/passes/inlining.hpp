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
//      admitted (cycle 6). What REMAINS refused: a callee `Phi`
//      (D-OPT7-MULTIBLOCK-SPLICE-PHI), a callee with NO returning path,
//      a recursive-cycle call (the call-graph SCC gate, rule 3), and a
//      callee whose instruction-count exceeds the cost bound (cycle 28).
//      The IntrinsicCall admission carries a frame-sensitivity caveat —
//      a frame-sensitive intrinsic (va_start / frameaddress / setjmp-
//      class) must NOT be inlined — but no shipped frontend emits any
//      intrinsic today, so blanket admission is correct for the current
//      model; per-intrinsic inline-safety gating is trigger-gated to the
//      first frame-sensitive intrinsic — D-OPT7-INLINE-FRAME-SENSITIVE-
//      INTRINSIC.
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

namespace dss::opt::passes {

struct InliningResult {
    bool        ok            = false;
    std::size_t callsInlined  = 0;
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
[[nodiscard]] DSS_EXPORT InliningResult
runInlining(Mir& mir, TypeInterner const& interner,
            DiagnosticReporter& reporter, std::uint32_t inlineThreshold);

} // namespace dss::opt::passes
