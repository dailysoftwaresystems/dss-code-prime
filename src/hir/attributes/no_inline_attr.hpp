#pragma once

// TF-C78 (D-CSUBSET-NOINLINE): declaration inliner-opt-out side-table value.
// Attached per-node via `HirAttribute<NoInlineAttr>` to a NATIVE FUNCTION
// declaration HIR node whose bound symbol carried a source-level `noinline`
// attribute — GNU `__attribute__((noinline))` / `[[gnu::noinline]]` (there is no
// C11/C23 standard spelling). Populated by CST→HIR lowering from the bound
// symbol's `SymbolRecord.isNoInline` (which the semantic phase folded from the
// language's `attributeSemantics.effects` `noInline` verb, gated on the declared
// type being a FnSig and OR-merged across the proto/definition pair); read at
// HIR→MIR lowering and stamped onto `MirFunc.noInline`, which the optimizer's
// inlining pass consults in its §2.9 legality gate — the sibling of the
// `funcBinding == Weak` refusal, checked at the same site.
//
// The exact `ThreadLocalAttr` / `MutabilityAttr` discipline (those structs'
// comments are the authority): its own side-table because "do not inline this"
// is NOT linkage — a `noinline` function keeps whatever binding and visibility
// it was declared with, and `static`/`weak`/`hidden` are orthogonal to it. It
// rides the SAME declaration node as `LinkageAttr` so HIR→MIR reads both at one
// place, but it is a separate axis and a separate map.
//
// ★ THE DEFAULT DIRECTION IS THE UNSAFE ONE, WHICH IS WHY THIS EXISTS AT ALL.
// A declaration node with NO attribute defaults to `noInline = false` — freely
// inlinable, the correct reading of an un-annotated C function. But unlike the
// sibling maps, absence here is NOT a conservative fallback: a LOST `true` does
// not degrade to a missed optimization or a loud verifier failure, it silently
// produces the exact transformation the programmer wrote the attribute to
// forbid. sqlite spells `SQLITE_NOINLINE` to BOUND STACK DEPTH on recursive
// paths, so the failure mode is a deeper-frames runtime regression that no test
// of the compiled output's VALUE would catch. That asymmetry is the reason the
// flag is threaded through every `MirFunc` creation, copy, rebuild and
// serialization path rather than only the lowering that mints it.
//
// The side-table stays sparse: only annotated declarations are recorded.

namespace dss {

struct NoInlineAttr {
    bool isNoInline = false;
};

} // namespace dss
