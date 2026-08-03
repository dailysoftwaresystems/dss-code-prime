#pragma once

// TF-C81 (D-CSUBSET-ALWAYSINLINE): declaration inliner-cost-model-BYPASS
// side-table value. Attached per-node via `HirAttribute<AlwaysInlineAttr>` to a
// NATIVE FUNCTION declaration HIR node whose bound symbol carried a source-level
// `always_inline` attribute — GNU `__attribute__((always_inline))` /
// `[[gnu::always_inline]]` (there is no C11/C23 standard spelling). Populated by
// CST→HIR lowering from the bound symbol's `SymbolRecord.isAlwaysInline` (which
// the semantic phase folded from the language's `attributeSemantics.effects`
// `alwaysInline` verb, gated on the declared type being a FnSig and OR-merged
// across the proto/definition pair); read at HIR→MIR lowering and stamped onto
// `MirFunc.alwaysInline`, which the optimizer's inlining pass consults in its
// §2.9 legality gate — where it SUPPRESSES the rule-6 size threshold.
//
// The exact `NoInlineAttr` discipline, one axis over: its own side-table because
// "inline this regardless of size" is not linkage and not the noinline axis, it
// rides the SAME declaration node as `LinkageAttr`/`NoInlineAttr` so HIR→MIR
// reads all three at one place, and it stays sparse (only annotated declarations
// are recorded).
//
// ★ WHAT THIS ATTRIBUTE DOES **NOT** PROMISE, SAID PLAINLY, BECAUSE THE NAME
// OVERPROMISES. It removes the inliner's PROFITABILITY veto and nothing else:
//   * Every CORRECTNESS refusal in the §2.9 gate still wins over it — a Weak
//     callee, a same-SCC (recursive) call, an address-escaped callee, a callee
//     with no returning path, an arity/type mismatch at the call. Those rules
//     prevent miscompiles and unbounded unrolling; no source annotation licenses
//     either. DSS refuses SILENTLY in those cases (GCC errors; clang warns) —
//     a conservative divergence: the program still compiles and is correct, it
//     merely keeps a call the author hoped to remove.
//   * A pipeline with no `Inlining` pass has no cost model to bypass, so the
//     attribute is VACUOUS there. The shipped `debug` pipeline is `Identity`
//     only, so `always_inline` changes nothing under `--config=debug`. GCC and
//     clang honour it at `-O0` because their inliner always runs; DSS's does
//     not. This is stated in the `c-subset.lang.json` effects row too, so the
//     config the user reads makes exactly the claim the code implements.
//
// ★ THE DEFAULT DIRECTION IS THE **SAFE** ONE HERE — THE OPPOSITE OF
// `NoInlineAttr`, AND THE CONTRAST IS THE POINT. A declaration with no attribute
// defaults to `alwaysInline = false`, and a LOST `true` degrades to "the cost
// model applies after all" — a missed optimization, never a wrong program. That
// makes this flag strictly less dangerous than its sibling. It is nevertheless
// threaded through every `MirFunc` creation, copy, rebuild and serialization
// path — and the reason is SHARPER here than it was for `NoInlineAttr`, not
// weaker. MEASURED: dropping the flag at the `mir_rebuild_helper` hop leaves the
// end-to-end shipped-release pin GREEN, because `Inlining` runs first in
// iteration 1 and an `alwaysInline` flag only has to survive to the FIRST
// inlining opportunity (whereas a `noInline` flag must keep refusing on every
// iteration, which is why TF-C78's equivalent hop DID break end-to-end). So the
// end-to-end test is BLIND to that hop and the dedicated propagation pin
// (`MirRebuildHelper.RebuildFunctionPreservesAlwaysInline`) is the only thing
// that can catch it. The bit still matters wherever iteration 1 does not finish
// the job — a callee that becomes inlinable only after an earlier pass
// simplifies its caller, or a cross-CU module merged post-optimization.
//
// ★ MUTUALLY EXCLUSIVE WITH `NoInlineAttr` at the SOURCE tier — a function
// carrying both is a loud `S_ConflictingInlineAttributes`, so semantic analysis
// never hands HIR a declaration in both maps. Should a MIR module nonetheless
// arrive with both bits set (hand-built MIR, or `.dssir` text parsed straight
// into the tier), the inliner's rule ORDER decides it conservatively: the
// noinline REFUSAL is checked before the threshold bypass, so refusal wins —
// which is also, MEASURED, exactly what clang does with the same contradiction.

namespace dss {

struct AlwaysInlineAttr {
    bool isAlwaysInline = false;
};

} // namespace dss
