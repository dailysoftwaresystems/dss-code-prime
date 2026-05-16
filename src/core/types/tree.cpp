#include "core/types/tree.hpp"

#include "core/types/diagnostic_reporter.hpp"   // complete-type for unique_ptr dtor
#include "core/types/grammar_schema.hpp"        // complete-type for shared_ptr (defensive)
#include "core/types/rule_id.hpp"
#include "core/types/tree_cursor.hpp"           // for Tree::cursor() / astCursor()

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dss {

namespace {

// Release-mode-fatal "this can't happen" guard. Used for invariants that, if
// violated, mean memory corruption or builder bug. Aborts the process so the
// stack trace pinpoints the call site.
[[noreturn]] void treeFatal(char const* what) {
    std::fputs("dss::Tree fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

Tree::Tree(detail::TreeData&& data) : data_(std::move(data)) {
    // Either the tree has nodes (and thus a valid root) or it is the empty tree.
    if (!data_.nodes.empty() && !data_.root.valid()) {
        treeFatal("Tree constructed with nodes but no valid root");
    }
}

Tree::~Tree() = default;
Tree::Tree(Tree&&) noexcept = default;
Tree& Tree::operator=(Tree&&) noexcept = default;

// ── identity ──

TreeId Tree::id() const noexcept { return data_.id; }

SourceBuffer const& Tree::source() const noexcept {
    // Pre-condition: TreeData::source must be non-null. The builder ensures it;
    // hand-fabricated TreeData in tests should set it.
    return *data_.source;
}

RuleInterner const& Tree::rules() const noexcept {
    return *data_.rules;
}

GrammarSchema const& Tree::schema() const noexcept {
    // Tests may fabricate a Tree without a schema (schema is null until T4
    // wires it through TreeBuilder). Calling schema() on such a tree is a
    // misuse; we abort rather than dereference null silently.
    if (!data_.schema) {
        treeFatal("Tree::schema: no GrammarSchema attached (tree built without one)");
    }
    return *data_.schema;
}

DiagnosticReporter const& Tree::diagnostics() const noexcept {
    if (!data_.diagnostics) {
        treeFatal("Tree::diagnostics: no DiagnosticReporter attached (tree built without one)");
    }
    return *data_.diagnostics;
}

bool Tree::hasSchema()      const noexcept { return static_cast<bool>(data_.schema); }
bool Tree::hasDiagnostics() const noexcept { return static_cast<bool>(data_.diagnostics); }

NodeId Tree::root() const noexcept { return data_.root; }

std::size_t Tree::nodeCount() const noexcept { return data_.nodes.size(); }

// ── universal accessors ──

detail::Node const& Tree::node_(NodeId id) const {
    if (!id.valid() || id.v >= data_.nodes.size()) {
        treeFatal("Tree::node_: NodeId out of range");
    }
    return data_.nodes[id.v];
}

NodeKind   Tree::kind(NodeId id)   const { return node_(id).kind; }
NodeFlags  Tree::flags(NodeId id)  const { return node_(id).flags; }
SourceSpan Tree::span(NodeId id)   const { return node_(id).span; }
NodeId     Tree::parent(NodeId id) const { return node_(id).parent; }

std::span<NodeId const> Tree::children(NodeId id) const {
    auto const& n = node_(id);
    if (n.childCount == 0) return {};

    // Release-mode bounds check: a corrupt firstChild/childCount must not
    // produce a span past the end of the child-index vector.
    const std::size_t first = n.firstChild;
    const std::size_t count = n.childCount;
    if (first > data_.childIndex.size() || first + count > data_.childIndex.size()) {
        treeFatal("Tree::children: child range exceeds child-index table");
    }
    return std::span<NodeId const>{data_.childIndex.data() + first, count};
}

std::string_view Tree::text(NodeId id) const {
    return data_.source->slice(node_(id).span);
}

// ── discriminant-asserting ──

RuleId Tree::rule(NodeId id) const {
    auto const& n = node_(id);
    assert(n.kind == NodeKind::Internal && "Tree::rule on non-Internal node");
    return n.rule;
}

SchemaTokenId Tree::tokenKind(NodeId id) const {
    auto const& n = node_(id);
    assert(n.kind == NodeKind::Token && "Tree::tokenKind on non-Token node");
    return n.tokenKind;
}

std::optional<DiagnosticIndex> Tree::diagnostic(NodeId id) const {
    auto const& n = node_(id);
    if (!n.diagnostic.valid()) return std::nullopt;
    return n.diagnostic;
}

// ── cursors ──────────────────────────────────────────────────────────────

TreeCursor Tree::cursor() const {
    return TreeCursor{*this, data_.root, CursorMode::Cst};
}

TreeCursor Tree::astCursor() const {
    return TreeCursor{*this, data_.root, CursorMode::Ast};
}

} // namespace dss
