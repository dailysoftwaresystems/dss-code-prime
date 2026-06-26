#pragma once

// c21 (D-CSUBSET-VOLATILE-QUALIFIER): access-volatility side-table value.
// Attached per-node via `HirAttribute<VolatileAttr>` to a USER-LEVEL ACCESS HIR
// node — an object `Ref` (load/store of a `volatile` object/global), a `Ref`
// that is an assignment TARGET, a struct/union `MemberAccess` (a `volatile`
// field), or a `VarDecl`/`Global` whose object is `volatile` (its initializing
// store). Populated by CST→HIR lowering from the bound symbol's
// `SymbolRecord.isVolatile` (set by the semantic phase from the language's
// `volatileMarker` token); read at HIR→MIR lowering and OR'd into the access's
// `MirInstFlags::Volatile` on the emitted Load/Store.
//
// CRITICAL distinction from `MutabilityAttr` (which is ALSO populated from a
// per-symbol bool): mutability is keyed on the DECLARATION node (Global) and
// flows ONLY to `.rodata`-vs-`.data` SECTION SELECTION — it never touches a
// Load/Store. Volatility is keyed on the ACCESS node and flows to the Load/Store
// FLAG, which the optimizer's already-Volatile-aware passes (DCE side-effect
// root, CSE skip, Mem2Reg skip, LICM skip) consult so a volatile access is
// never elided / cached / reordered. The threading is genuinely NEW plumbing —
// the const precedent does NOT cover it.
//
// A node with NO attribute defaults to `isVolatile = false` — a plain memory
// access the optimizer may freely transform. The side-table stays sparse: only
// volatile accesses are annotated. Defaulting to non-volatile is the
// PERFORMANCE-safe default and CORRECT because a non-volatile access genuinely
// has no ordering/elision constraints; the only risk is a MISSED volatile
// access (a silent miscompile), which the exhaustive threading across every
// user Load/Store emit site closes.

namespace dss {

struct VolatileAttr {
    bool isVolatile = false;
};

} // namespace dss
