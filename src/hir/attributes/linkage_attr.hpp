#pragma once

#include "core/types/section_kind.hpp"  // StaticInitSchedule
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
    // ★★ P69 (lane `cs`) — WHICH KIND OF WEAK DEFINITION, read ONLY when `binding`
    // is `Weak`: a default any other definition of the name replaces
    // (`Overridable` — `__attribute__((weak))`, `#pragma weak`), or one of several
    // interchangeable copies of which the link keeps one (`SelectAny` —
    // `selectany` in either spelling). A second axis beside the binding, never a
    // fourth binding value (`core/types/symbol_attrs.hpp`).
    //
    // The value-initialized state is NEITHER enumerator, on purpose: it is what an
    // attribute holds when its binding is not weak, and what a weak one would hold
    // if a producer forgot to say — and `HirVerifier` refuses exactly that, a weak
    // linkage attribute that names no kind. The one producer in this tier is the
    // specifier fold (`linkageFrom`), which copies the kind from the language
    // document's `linkageSpecifiers` entry; the loader requires the key beside
    // every weak binding, so the fold always has it.
    WeakDefinitionKind weakKind{};
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
    // `c.lang.json`), so `binding != Global` already means "a binding was
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
    // ★★ D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN: this
    // declaration's place in the program's static-initializer schedule, read from
    // its symbol's `SymbolRecord::staticInit` by `recordLinkage` and carried to
    // `MirFunc::staticInit`.
    //
    // ★★ WHY IT RIDES *THIS* SIDE-TABLE, STATED PLAINLY BECAUSE IT IS NOT
    // OBVIOUSLY LINKAGE. Every other per-function attribute fact (`noInline`,
    // `alwaysInline`, `noSanitizeThread`, `noOptimize`) gets its OWN
    // `HirAttribute<>` map and its own `lowerHirToMir` parameter — and each of
    // those parameters is passed from `src/program/compile_pipeline.cpp`, which
    // this lane does not own. A new map would therefore have landed with nothing
    // passing it, i.e. a defaulted `nullptr` and a feature that is dead in the
    // shipped pipeline while every unit test that constructs the map by hand goes
    // green: a half-landed flag, which is indistinguishable in the OUTPUT from no
    // flag at all. `linkageMap` is ALREADY threaded, so the fact travels for free.
    //
    // ★ AND THE FIT IS BETTER THAN "IT WAS AVAILABLE". What this side-table
    // carries is how a declaration participates in the IMAGE rather than in the
    // program text — binding, visibility, and now whether the symbol's address is
    // filed in a table some runtime walks. All three are answered from the
    // declaration's specifiers and all three are consumed by the object writer,
    // not by any expression.
    //
    // Empty ⇒ an ordinary function, so the side-table stays sparse in the sense
    // `recordLinkage` means: absence is the correct default.
    StaticInitSchedule staticInit{};
    // ★★ P69 (lane `cs`) — THIS GLOBAL IS A TENTATIVE DEFINITION THE UNIT NEVER
    // COMPLETED (C 6.9.2p2: a file-scope object declared without an initializer
    // and without `extern`, for which the unit holds no definition with one). The
    // semantic tier knows it (`SymbolRecord::isTentativeDefinition`); below this
    // tier nothing could tell `int x;` from `int x = 0;`, both are a zero-filled
    // definition — so two units each holding `int x;` were refused as two strong
    // definitions where the references link them (a common on the formats that
    // have one). Carried here so the merge can fold two of them and a relocatable
    // can write a common.
    //
    // It rides this side-table for the reason `staticInit` does: it is a fact
    // about how the declaration takes part in the IMAGE, the map is already
    // threaded to HIR→MIR, and a new map would land with nothing passing it.
    // False ⇒ an ordinary definition (or not a definition at all), so the table
    // stays sparse.
    bool tentative = false;
};

} // namespace dss
