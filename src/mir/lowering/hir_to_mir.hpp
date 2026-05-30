#pragma once

#include "core/export.hpp"
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
           HirFfiMap const*         ffiMap    = nullptr);

} // namespace dss
