#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"

#include <memory>

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

[[nodiscard]] DSS_EXPORT SemanticModel
analyze(std::shared_ptr<CompilationUnit const> cu);

} // namespace dss
