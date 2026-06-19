#pragma once

// Declaration mutability side-table value. Attached per-node via
// `HirAttribute<MutabilityAttr>` to a NATIVE declaration HIR node (Global) that
// carried a source-level CONST qualifier — C `const int g = 5;` → `isConst =
// true`. Populated by CST→HIR lowering from the bound symbol's
// `SymbolRecord.isConst` (set by the semantic phase from the language's
// `constMarker` token); read at HIR→MIR lowering and stamped onto the
// `MirGlobal.isConst` bit, which the assembler's section-selection
// (`lowerMirGlobalsToDataItems`) consults to route an initialized global to
// read-only `.rodata` (const) vs writable `.data` (mutable).
//
// Deliberately distinct from `LinkageAttr` (binding/visibility): const-ness is
// NOT linkage — a `static const` global is BOTH internally-bound AND read-only,
// two orthogonal axes. Keeping mutability in its own side-table lets each map
// stay sparse on its own axis (a mutable extern global annotates neither).
//
// A declaration node with NO attribute defaults to `isConst = false` — MUTABLE,
// the conservative writable default (a const requires an initializer and a
// source-level qualifier, so absence ⇒ mutable). So the side-table stays
// sparse: only const-qualified decls are annotated, and absence is the correct
// writable default. (Routing a wrongly-defaulted mutable global to `.rodata`
// would re-introduce the store-crash this side-table exists to fix; defaulting
// to writable fails safe.)

namespace dss {

struct MutabilityAttr {
    bool isConst = false;
};

} // namespace dss
