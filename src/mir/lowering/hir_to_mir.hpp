#pragma once

#include "core/export.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/hir_literal_pool.hpp"
#include "mir/mir.hpp"

#include <memory>
#include <vector>

// HIR → MIR lowering (plan 12 / ML2). Public entry: takes a frozen HIR
// module (+ its literal pool) plus the CU's TypeInterner (for type-driven
// opcode selection), walks the HIR's top-level declarations, and produces a
// frozen `Mir` module. Per the plan, MIR is SSA over CFG with structured-CF
// markers preserved — but ML2's first cycle covers ONLY straight-line code:
// a function with params + literals + integer arithmetic + return. Control
// flow (If/While/For/Switch), Call, MemberAccess, etc. are deliberately
// fail-loud-deferred to subsequent ML2 cycles via `H_UnsupportedLoweringForKind`
// in the diagnostic-reporter — same discipline as HR8's lowering engine.

namespace dss {

// Output of ML2 lowering. Move-only; the `Mir` is the frozen module the
// optimizer + assembler consume. `ok` mirrors HR8's delta-from-before
// error count, so prior reporter diagnostics don't taint it.
struct DSS_EXPORT HirToMirResult {
    Mir  mir;
    // Extern symbol descriptors extracted from HIR's
    // `ExternFunction` nodes during the pre-pass (LK6 cycle 2d —
    // D-LK6-6 closure). Each row's `mangledName` + `libraryPath`
    // come from the per-node `HirAttribute<FfiMetadata>` side-
    // table the caller passes in. **Fail-loud contract** (the
    // post-fold review verified the stance against the silent-
    // failure rule): any `ExternFunction` whose FfiMap entry is
    // missing — OR whose entry carries an empty `mangledName` or
    // empty `importLibrary` — produces an `H_UnsupportedLowering
    // ForKind` diagnostic anchored at the HIR node's source span,
    // and the row is NOT pushed. This surfaces the metadata
    // problem at the IR tier that has source-span context rather
    // than deferring to the linker (where the span has been
    // lost). The vector is propagated forward through
    // `MirToLirResult.externImports` → `assemble()` →
    // `AssembledModule.externImports`.
    std::vector<ExternImport> externImports;
    bool ok = true;
};

// Caller-supplied policy carried INTO MIR lowering. Currently one field —
// the MIR-globals const-evaluation `allowFloat` knob, threaded from the
// owning language schema's `hirLowering.globalsConstEval.allowFloat`
// (plan 12.5 §0.2 D3, closed). Defaults match v1's all-IEEE corpus so an
// absent config gives identical behaviour to CE5. The CU integration
// reads the schema(s) and passes the resolved policy in; multi-language
// CUs conservatively AND each schema's knob (any `false` ⇒ module-wide
// `false`) until per-Global routing lands in plan 20.
struct DSS_EXPORT MirLoweringConfig {
    bool globalsAllowFloat = true;
    // D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING: threaded from the
    // source-language schema's
    // `semantics.pointerAliasing.strictAliasingOnDistinctTypes`. When
    // true, `lowerToMir` calls `MirBuilder::setAliasingMode(StrictTBAA)`
    // before `finish()` — CSE/LICM Load admission then admits Rule 6
    // (distinct primitive pointees). Default false (sound — every
    // CSE/LICM admission stays conservative without opt-in).
    bool strictAliasingOnDistinctTypes = false;

    // D-OPT-MIR-ALIAS-CHAR-EXCEPTION-OVERRIDE: per-source-language
    // C99 §6.5 ¶7 character-type-alias-all opt-in. Threaded from
    // `semantics.pointerAliasing.charTypesAliasAll`. Default `true`
    // matches C/C++/Objective-C; a Rust frontend or strict-typed DSL
    // would set false. Lowered to `Mir::charTypesAliasAll()` so the
    // MIR-tier alias predicate (`mirMayAlias` Rule 5) reads it without
    // language identity branches.
    bool charTypesAliasAll = true;

    // FC6: the active target's aggregate-layout params + the format's data model,
    // so the HIR→MIR lowering can fold a `HirKind::SizeOf` to its type's byte size
    // via the `type_layout` engine. `aggregateLayoutLoaded` is false when the
    // target declared no `aggregateLayout` block — a `sizeof` then fails loud at
    // the lowering site (never a guessed size). `dataModel` supplies the pointer
    // width the layout engine needs.
    AggregateLayoutParams aggregateLayout{};
    bool                  aggregateLayoutLoaded = false;
    DataModel             dataModel = DataModel::Lp64;

    // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): the active CC's by-value aggregate
    // classification strategy + max-register-bytes, threaded from the resolved
    // `TargetCallingConvention` (the §B-locked HIR→MIR boundary, mirroring how
    // `dataModel` is threaded). `aggregateClassification == None` (default) ⇒ the
    // by-value guard stays FAIL-LOUD (no CC / unsupported strategy). When the
    // strategy is implemented (`aggregateAbiImplemented`), HIR→MIR classifies a
    // struct arg/return via the `aggregate_abi` engine and synthesizes pieces /
    // a hidden sret pointer. `aggregateSretViaHiddenArg` = the CC has NO
    // indirect-result register (SysV/Win64 ⇒ sret ptr is a hidden first INTEGER
    // arg; AAPCS64's x8 path is false, C3).
    AggregateClassKind aggregateClassification   = AggregateClassKind::None;
    std::uint16_t      aggregateMaxRegBytes       = 0;
    bool               aggregateSretViaHiddenArg  = true;

    // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG): the active CC's `slotAligned` flag.
    // When false (SysV/AAPCS64 — INDEPENDENT counters) the param `Arg` payload is
    // the PER-CLASS physical register ordinal (GPR/FPR counted separately); when
    // true (Win64 — SLOT-ALIGNED) it is the FLAT shared slot index. HIR→MIR emits
    // each scalar param + each struct-piece `Arg` with the matching monotonic
    // counter, so a multi-register struct param lands in consecutive arg
    // registers and the lir_callconv per-class/flat lookup resolves it. (This
    // also fixes the latent mixed-class `D-ML7-2.10`: a scalar param's payload is
    // now its per-class index, not the param index.)
    bool               argSlotAligned             = false;
};

// Lower the frozen `hir` module to MIR. `literals` is the HirLiteralPool
// that owns the decoded values for HIR `Literal` nodes (ML2 copies the
// entries it lowers into the new MirLiteralPool). `interner` is the CU's
// type interner — ML2 reads from it to pick signed-vs-unsigned MIR opcodes
// and to decode FnSig results, AND mints new pointer types for addressable-
// local `Alloca` results (lvalue-via-alloca model). Same convention as HIR
// lowering, which also takes the interner by non-const reference.
// Diagnostics (H_UnsupportedLoweringForKind for not-yet-supported HirKinds)
// are emitted into `reporter`.
[[nodiscard]] DSS_EXPORT HirToMirResult
lowerToMir(Hir const&               hir,
           HirLiteralPool const&    literals,
           TypeInterner&            interner,
           DiagnosticReporter&      reporter,
           HirSourceMap const*      sourceMap = nullptr,
           MirLoweringConfig const& config    = {},
           // FFI side-table — populated by the CST→HIR lowerer
           // (or by FF5 binary ingestion) with one entry per
           // `HirKind::ExternFunction` / `ExternGlobal` node.
           // Optional: if nullptr the lowering fails loud on
           // every encountered `ExternFunction` with
           // `H_UnsupportedLoweringForKind` (the post-fold review
           // moved the fail-loud surface here so the diagnostic
           // anchors at the HIR node's source span rather than at
           // the linker). Same fail-loud applies if a per-node
           // entry is missing OR its `mangledName` /
           // `importLibrary` is empty. Modules with no extern
           // declarations are unaffected by the parameter and
           // produce an empty `HirToMirResult.externImports`.
           // (LK6 cycle 2d — D-LK6-6 closure.)
           HirFfiMap const*         ffiMap    = nullptr,
           // D-CSUBSET-LINKAGE-SPECIFIERS / D-OPT7-LINKAGE-HIR-TO-MIR-MAPPING
           // (pre-OPT7 P2): native-declaration linkage side-table, populated by
           // the CST→HIR lowerer from the language's `linkageSpecifiers` facet.
           // Optional: if nullptr (or a decl carries no entry) the
           // function/global defaults to (Global, Default) — externally visible
           // — exactly the pre-linkage behavior. Read here to stamp
           // MirFunc/MirGlobal binding+visibility, the input the optimizer's
           // DCE-protect predicate `isExternallyVisible()` consults.
           HirLinkageMap const*     linkageMap = nullptr);

} // namespace dss
