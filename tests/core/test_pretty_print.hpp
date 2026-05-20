#pragma once

#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "core/types/tree_visitor.hpp"

#include <string>

namespace dss::tests {

// Walk the tree in AST mode emitting `rule:<name>` for Internal nodes and
// `tok:"<text>"` for visible Token leaves. Two spaces per depth level.
// Error/Missing/Synthetic flags are intentionally NOT surfaced — broken-path
// tests verify them via a separate flag walk over `tree.flags(id)`.
//
// The output is the structural fingerprint that string-equality assertions
// in happy-path tests compare against. The format is deliberately stable so
// new test files can adopt it without invention.
inline std::string prettyPrint(Tree const& t) {
    std::string out;
    if (!t.root().valid()) return out;
    walkPreOrder(TreeCursor{t, t.root(), CursorMode::Ast},
                 [&](TreeCursor const& c) {
        const int d = c.depth();
        for (int i = 0; i < d; ++i) out += "  ";
        const auto id = c.current();
        if (t.kind(id) == NodeKind::Internal) {
            out += "rule:";
            out += t.rules().name(t.rule(id));
        } else {
            out += "tok:\"";
            out += t.text(id);
            out += '"';
        }
        out += '\n';
    });
    return out;
}

} // namespace dss::tests
