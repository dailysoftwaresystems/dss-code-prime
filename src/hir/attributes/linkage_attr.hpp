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
};

} // namespace dss
