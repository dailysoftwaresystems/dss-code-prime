#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"
#include "core/types/object_format_kind.hpp"   // ObjectFormatKind (per-target availability gate)

#include <memory>
#include <optional>

// Phase #8 entry point: drive symbol/scope/type analysis over a built
// CompilationUnit and return a SemanticModel. The implementation is
// the language-agnostic `SchemaDrivenSemantics` engine — it reads the
// schema's `SemanticConfig` and never branches on schema.name() (the
// "universal compiler" thesis: per-language behavior comes from the
// config alone).
//
// The shared_ptr keeps the CU at a stable address — the model's
// `UnitAttribute<T>` side-tables hold raw `Tree*` and would dangle if
// the CU were destroyed early.

namespace dss {

// FC3 c1: `dataModel` is the ACTIVE FORMAT's declared width triple
// (plan 23 — the format schema's REQUIRED `dataModel` field), threaded
// by the driver (`buildCuMir` passes `format.dataModel()`); semantic
// analysis is per-(CU × target) so the model is always in scope there.
// It drives every `coreByDataModel` override (builtinTypes /
// typeSpecifiers), the integer-literal ladder, and the shipped-lib
// descriptor `signatureByDataModel` resolution. The returned
// SemanticModel CARRIES the model (`SemanticModel::dataModel()`) so the
// HIR lowering reads the SAME value by construction — the two tiers can
// never diverge.
//
// The default (LP64) serves DIRECT-API callers (unit tests, LSP):
// LP64 is the identity model for every shipped config (each row's BASE
// `core` is its LP64 value; overrides key the other models), so a
// default-call analyzes exactly as the pre-FC3 signature did. The
// format JSON itself has NO default — a missing `dataModel` there is a
// load reject. ILP32 is declared-only: selecting it fails loud
// (S_UnsupportedDataModel) rather than running an untested width path.
// FC6 deferral-close: `aggregateLayout` is the active target's aggregate-layout
// params (`target.aggregateLayout()`, gated by `aggregateLayoutLoaded()`),
// passed as `std::optional` — `nullopt` ⇒ the target declared no block, so a
// `sizeof` in an array-dimension const-expression fails loud rather than folding
// a wrong size. Direct-API callers (unit tests, LSP) default to `nullopt`: they
// analyze exactly as before, and an array-dim `sizeof` simply does not fold.
// FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): `vaListStrategy` is the active CC's va_list
// lowering strategy (`cc.vaListLayout->strategy`), threaded from the SAME resolved
// CC the pipeline reads for `aggregateLayout`. It selects the injected `va_list`
// TYPE so `sizeof(va_list)` is right (SysV `__va_list_tag[1]`=24B vs Win64
// `char*`=8B) — a wrong size mis-sizes the `ap` local and corrupts the stack.
// `nullopt` (direct-API callers, or a CC with no variadic-callee ABI) ⇒ the
// SysV-family `__va_list_tag[1]` default (back-compat: tests analyze exactly as
// before). An `Aapcs64DualCursor` strategy fails loud at injection (FC12c).
[[nodiscard]] DSS_EXPORT SemanticModel
analyze(std::shared_ptr<CompilationUnit const> cu,
        DataModel dataModel = DataModel::Lp64,
        std::optional<AggregateLayoutParams> aggregateLayout = std::nullopt,
        std::optional<VaListStrategy> vaListStrategy = std::nullopt,
        // The active target's object-format — gates per-target shipped-header
        // availability (a POSIX `<sys/time.h>` is unavailable for windows-pe).
        // `nullopt` (direct-API / LSP / test callers) ⇒ NO availability gate (every
        // recorded descriptor is read, as before). (D-SHIPPED-HEADER-PER-TARGET-AVAILABILITY)
        std::optional<ObjectFormatKind> activeFormat = std::nullopt);

} // namespace dss
