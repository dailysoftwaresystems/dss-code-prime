#pragma once

#include "core/types/symbol_attrs.hpp"  // SymbolBinding, SymbolVisibility

// Declaration linkage side-table value (HR5). Attached per-node via
// `HirAttribute<LinkageAttr>` to a NATIVE declaration HIR node (Function /
// Global) that carried a source-level linkage specifier — C `static` → internal
// (`Local`) binding; `__attribute__((weak))` → `Weak`; `__attribute__((
// visibility("hidden")))` → narrowed visibility. Populated by CST→HIR lowering
// from the language's `linkageSpecifiers` facet; read at HIR→MIR lowering and
// stamped onto the `MirFunc`/`MirGlobal` binding+visibility, which the
// optimizer's DCE-protect predicate `isExternallyVisible()` consults.
//
// Deliberately distinct from `FfiMetadata`: that carries FOREIGN (extern /
// imported) symbol linkage + import-library routing; this carries a native
// declaration's OWN linkage. It uses the agnostic `SymbolBinding` /
// `SymbolVisibility` vocabulary (which — unlike `FfiLinkage`'s Strong/Weak/Common
// — includes `Local`, exactly what `static` needs). No `Hir` dependency on
// purpose: consumers bind it as `HirAttribute<LinkageAttr>`.
//
// A declaration node with NO attribute defaults to (`Global`, `Default`) —
// externally visible, the C convention for a non-`static` file-scope
// declaration. So the side-table stays sparse: only specifier-bearing decls are
// annotated, and absence is the correct externally-visible default.

namespace dss {

struct LinkageAttr {
    SymbolBinding    binding    = SymbolBinding::Global;
    SymbolVisibility visibility = SymbolVisibility::Default;
    // ★★ FOLD-SCRATCH, NOT A RECORDED FACT (TF-C93,
    // D-CSUBSET-LINKAGE-SPECIFIER-CONFLICT-SILENT-LAST-WINS, visibility half).
    // True once a `linkageSpecifiers` row has EXPLICITLY set `visibility` during
    // one declaration's fold. MEANINGLESS the moment the attribute is recorded
    // in `HirLinkageMap`: no consumer reads it (HIR→MIR reads only `.binding` /
    // `.visibility`), `hir_text`/`mir_text` never round-trip it, and
    // `recordLinkage`'s sparseness test ignores it.
    //
    // ★ WHY IT IS A STRUCT FIELD AND NOT A LOCAL IN `linkageFrom`. The
    // `binding` axis needs no such bit because `SymbolBinding::Global` is
    // UNWRITABLE from config (MEASURED: zero `"binding": "global"` rows in
    // `c-subset.lang.json`), so `binding != Global` already means "a binding was
    // specified". `SymbolVisibility::Default = 0` is BOTH the unspecified
    // sentinel AND a writable config value (`visibility:default`, added TF-C92),
    // so `visibility != Default` CANNOT distinguish the two — and a guard keyed
    // on it fires on `hidden`→`default` while SILENTLY FOLDING `default`→`hidden`.
    // The bit must therefore survive `declaratorLinkage`'s SEED boundary
    // (`cst_to_hir.cpp` seeds one declarator's trailing fold with the shared
    // prefix's already-folded `LinkageAttr`), which a local cannot do.
    //
    // ★ It cannot leak BETWEEN declarators: `declaratorLinkage` takes its `base`
    // BY VALUE, so each declarator folds onto its own copy of the prefix.
    bool             visibilitySpecified = false;
};

} // namespace dss
