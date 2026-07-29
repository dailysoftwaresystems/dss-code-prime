#pragma once

// ★★ TF-C85: declaration OPTIMIZER-OPT-OUT side-table value. Attached per-node
// via `HirAttribute<NoOptimizeAttr>` to a NATIVE FUNCTION declaration HIR node
// whose bound symbol sat inside an MSVC `#pragma optimize("", off)` region.
// Populated by CST→HIR lowering from the bound symbol's
// `SymbolRecord.isNoOptimize`; read at HIR→MIR lowering and stamped onto
// `MirFunc.noOptimize`, which the optimizer's TWO function-rebuild seams consult.
//
// ★ ITS PROVENANCE DIFFERS FROM ITS TWO NEIGHBOURS, AND THAT IS THE INTERESTING
// PART. `NoInlineAttr` and `AlwaysInlineAttr` come from an ATTRIBUTE written on
// the declaration. This one comes from a LEXICALLY SCOPED PREPROCESSOR REGION:
// the preprocessor stamps every token it emits inside the region, and the
// semantic tier asks "was this declaration's leftmost EMITTED token stamped".
// Per-token rather than per-byte-range for the same reason `#pragma pack` is —
// a function definition arriving from a macro replacement list carries the
// `#define` line's span, which is nowhere near the region containing its
// INVOCATION, so a range lookup would answer "optimize" for exactly the case the
// author was trying to control. Everything from `SymbolRecord` downward is the
// `NoInlineAttr` route verbatim.
//
// ★ WHAT IT PROMISES, STATED NARROWLY — AND WHAT IT MUST NEVER BE SOLD AS.
// A function carrying this flag is rebuilt VERBATIM by every optimizer pass:
// the shared `MirFunctionRebuilder` swaps the pass's policy for an identity
// policy, so no fold, no promotion, no CFG edit and no hoist is applied to it,
// and the inliner neither splices into it nor splices it into anyone.
//   * It changes PERFORMANCE, never BEHAVIOR. Every DSS optimizer pass is
//     semantics-preserving, so a function compiled with the flag and the same
//     function compiled without it must agree observably. That is what makes the
//     conservative resolution of a contested token (see
//     `PreprocessResult::pragmaNoOptimizeByOffset`) sound.
//   * ★ IT IS **NOT** A FLOATING-POINT FIX. MSVC's own motivating use — sqlite's
//     `ext/misc/totype.c`, which disables optimization around a double
//     round-trip-equality test — targets x87 EXCESS PRECISION. MEASURED, that
//     hazard structurally cannot occur in this tree: `const_fold.cpp`'s fold maps
//     are integer-only (no FAdd/FMul/FDiv/FCmp/SIToFP arms), there is no
//     reassociation pass, there is no FMA or fast-math anywhere, and `double` is
//     SSE2 at exactly 64 bits (x87 is F80-only). So this sink is FAITHFULNESS to
//     a real MSVC contract that also unblocks the pe64 corpus leg — it is not
//     repairing a live miscompile, and any comment, test name or report implying
//     otherwise is a false claim.
//
// ★ THE DEFAULT DIRECTION. A declaration outside every region defaults to
// `noOptimize = false` — optimize normally, which is exactly the pre-TF-C85
// behavior for every function that ever existed. A LOST `true` degrades to "the
// function got optimized after all": still a correct program, so this flag is in
// `AlwaysInlineAttr`'s risk class rather than `NoInlineAttr`'s. It is threaded
// through every `MirFunc` creation/copy/rebuild/serialize path anyway, for the
// reason TF-C81 MEASURED: a half-landed flag and no flag are indistinguishable
// in the emitted binary, so only a dedicated per-hop pin can tell them apart.
//
// ★ INDEPENDENT OF BOTH INLINE FLAGS. There is no contradiction to diagnose:
// `noOptimize` with `noinline` is redundant but coherent, and `noOptimize` with
// `always_inline` is a genuine conflict of intent that the inliner resolves
// conservatively in ONE direction — the no-optimize refusal is checked first, so
// a no-optimize callee is not spliced even when it asks to be. Recording a loud
// error would refuse a program MSVC compiles.

namespace dss {

struct NoOptimizeAttr {
    bool isNoOptimize = false;
};

} // namespace dss
