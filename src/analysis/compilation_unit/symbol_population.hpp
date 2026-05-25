#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/unit_attribute.hpp"
#include "core/export.hpp"
#include "core/types/strong_ids.hpp"

// Minimal declaration-symbol population — a CU3 placeholder for the phase-#8
// symbol table. It exercises `UnitAttribute<SymbolId>` end-to-end across every
// tree in a CompilationUnit without doing any real semantic analysis.
//
// What it does: walk each tree; for every functionDecl / varDecl node, mint a
// fresh CU-scoped SymbolId and bind it to that declaration's name node.
//
// What it deliberately does NOT do (all phase #8): scope resolution, name
// lookup, shadowing/redeclaration handling, type association, or deduplication
// of references to a declaration. Every declaration *site* simply gets a
// distinct SymbolId; uses are not linked to declarations here.

namespace dss {

[[nodiscard]] DSS_EXPORT UnitAttribute<SymbolId>
populateDeclarationSymbols(CompilationUnit const& cu);

} // namespace dss
