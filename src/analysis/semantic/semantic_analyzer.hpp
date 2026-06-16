#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"
#include "core/types/aggregate_layout.hpp"
#include "core/types/data_model.hpp"

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
[[nodiscard]] DSS_EXPORT SemanticModel
analyze(std::shared_ptr<CompilationUnit const> cu,
        DataModel dataModel = DataModel::Lp64,
        std::optional<AggregateLayoutParams> aggregateLayout = std::nullopt);

} // namespace dss
