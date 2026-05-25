#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"

#include <string>

namespace dss {

class GrammarSchema;  // for registerSchemaTypeExtensions

// The per-CU type state: a canonicalizing TypeInterner + an extension
// TypeRegistry, both scoped to one CompilationUnit by id. Owned by the
// consumer (phase #8) and bound to a CompilationUnit — the CompilationUnit
// itself stays immutable (the NodeAttribute / UnitAttribute pattern), so the
// CU1 post-finish freeze invariant is preserved. Move-only.
class TypeLattice {
public:
    explicit TypeLattice(CompilationUnitId owner, std::string sourceLanguage = {})
        : interner_(owner), registry_(std::move(sourceLanguage)) {}

    [[nodiscard]] CompilationUnitId   owner()    const noexcept { return interner_.owner(); }
    [[nodiscard]] TypeInterner&       interner()       noexcept { return interner_; }
    [[nodiscard]] TypeInterner const& interner() const noexcept { return interner_; }
    [[nodiscard]] TypeRegistry&       registry()       noexcept { return registry_; }
    [[nodiscard]] TypeRegistry const& registry() const noexcept { return registry_; }

private:
    TypeInterner interner_;
    TypeRegistry registry_;
};

// Register every extension a schema declares (its `typeExtensions[]`) into
// `registry`. The consumer wires this when building a CU's lattice.
DSS_EXPORT void registerSchemaTypeExtensions(TypeRegistry& registry, GrammarSchema const& schema);

} // namespace dss
