#include "analysis/compilation_unit/symbol_population.hpp"

#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_views.hpp"
#include "core/types/tree_visitor.hpp"

#include <cstdint>

namespace dss {

UnitAttribute<SymbolId> populateDeclarationSymbols(CompilationUnit const& cu) {
    UnitAttribute<SymbolId> symbols{cu};

    // CU-scoped monotonic counter; 0 is reserved for InvalidSymbol.
    std::uint32_t next = 1;
    // Bind only when the declaration's name slot is actually an Identifier
    // token. The View nameNode() accessors are positional and unchecked, so
    // an error-recovery tree can yield a Missing/non-identifier (or absent)
    // name node; binding a SymbolId there would be confidently-wrong data.
    // Skipping it is the intended tolerance for this CU3 placeholder — phase
    // #8's real symbol table will diagnose name-less declarations.
    auto bind = [&](Tree const& tree, NodeId name) {
        if (IdentifierView::from(tree, name)) symbols.set(name, SymbolId{next++});
    };

    for (Tree const& tree : cu.trees()) {
        walkPreOrder(tree, [&](TreeCursor const& cursor) {
            NodeId const id = cursor.current();
            if (auto fn = FunctionDeclView::from(tree, id)) {
                bind(tree, fn->nameNode());
            } else if (auto var = VarDeclView::from(tree, id)) {
                bind(tree, var->nameNode());
            }
        });
    }

    return symbols;
}

} // namespace dss
