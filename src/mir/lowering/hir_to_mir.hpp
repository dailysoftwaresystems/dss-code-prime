#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "hir/hir.hpp"
#include "hir/hir_attrs.hpp"
#include "hir/lowering/hir_literal_pool.hpp"
#include "mir/mir.hpp"

#include <memory>

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
    bool ok = true;
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
lowerToMir(Hir const&             hir,
           HirLiteralPool const&  literals,
           TypeInterner&          interner,
           DiagnosticReporter&    reporter,
           HirSourceMap const*    sourceMap = nullptr);

} // namespace dss
