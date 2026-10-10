#pragma once

// Declaration mutability side-table value. Attached per-node via
// `HirAttribute<MutabilityAttr>` to a NATIVE declaration HIR node (a Global, a
// block-scope VarDecl, a static UnnamedObject) that carried a source-level CONST
// qualifier — C `const int g = 5;` → `isConst = true`. Populated by CST→HIR
// lowering from the bound symbol's `SymbolRecord.isConst` (set by the semantic
// phase from the language's `constMarker` token). Two readers at HIR→MIR
// lowering: a Global's (and a static unnamed object's) bit is stamped onto
// `MirGlobal.isConst`, which the assembler's section-selection
// (`lowerMirGlobalsToDataItems`) consults to route an initialized global to
// read-only `.rodata` (const) vs writable `.data` (mutable); and ANY node's bit
// decides whether a static initializer may READ that object's value (the
// language's `constObjectRead` form — P69, lane `cs`, for a block-scope object:
// D-C-A-BLOCK-SCOPE-CONST-OBJECTS-VALUE-IN-A-STATIC-INITIALIZER-IS-REFUSED). A
// VarDecl is never PLACED by this bit — it stays a stack slot.
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
