#pragma once

#include "core/export.hpp"
#include "core/substrate/arena_container.hpp"    // Tree's node storage is an ArenaContainer
#include "core/types/diagnostic_reporter.hpp"   // unique_ptr<DiagnosticReporter> needs the complete type
#include "core/types/grammar_schema.hpp"        // shared_ptr<GrammarSchema const>
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <string_view>
#include <vector>

namespace dss::substrate {

// Reproduce Tree's exact diagnostic wording so the existing death tests
// (e.g. "NodeAttribute bound to TreeId=… got NodeId from TreeId=…",
// "Tree::node_: NodeId out of range") match byte-for-byte after the arena
// substrate generalization. Declared before the ArenaContainer<Node, …> use
// in TreeData below so the specialization is always the one chosen.
template <>
struct ArenaNames<NodeId, TreeId> {
    static constexpr char const* attribute = "NodeAttribute";
    static constexpr char const* element   = "NodeId";
    static constexpr char const* tag       = "TreeId";
    static constexpr char const* access    = "Tree::node_";
};

} // namespace dss::substrate

namespace dss {

// (RuleInterner is now a `using` alias defined in rule_id.hpp; included
//  transitively via grammar_schema.hpp above.)

namespace detail {

// Movable POD that TreeBuilder hands to Tree's constructor. Keeping this in
// detail/ means consumers can't fabricate one casually — only the builder
// (and unit tests deliberately importing the detail header) produces one.
//
// Hand-fabricated test trees may leave `schema` and `diagnostics` null;
// the builder path always sets both. Tree's accessors abort with a clear
// message when a null-set member is dereferenced (`tree.schema()` /
// `tree.diagnostics()`), so misuse is loud rather than silent.
struct DSS_EXPORT TreeData {
    std::shared_ptr<SourceBuffer>        source;
    std::shared_ptr<RuleInterner const>  rules;
    std::shared_ptr<GrammarSchema const> schema;          // null only in hand-built test trees
    // Node storage + the tree's identity tag + the cross-tree access guard,
    // all carried by the generalized arena substrate (SP1). The tree's `id`
    // is `arena.id()`.
    substrate::ArenaContainer<Node, NodeId, TreeId> arena;
    std::vector<NodeId>                  childIndex;      // flat child table
    std::unique_ptr<DiagnosticReporter>  diagnostics;     // null only in hand-built test trees
    NodeId                               root = InvalidNode;
};

} // namespace detail

// Immutable, schema-validated tree. Constructed exclusively via
// `Tree(detail::TreeData&&)` from `TreeBuilder::finish()` (or, in tests, from
// a hand-fabricated TreeData).
//
// All read accessors are const and thread-safe; multiple cursors over the
// same Tree from different threads need no synchronization. (Per-attribute
// side-tables — see NodeAttribute<T> — are the only mutable companion state.)
class DSS_EXPORT Tree {
public:
    // Arena interface consumed by substrate::ArenaAttribute<Tree, T> (the type
    // behind NodeAttribute<T>): the element id type and the arena's tag type.
    using IdType  = NodeId;
    using TagType = TreeId;

    explicit Tree(detail::TreeData&& data);
    ~Tree();    // out-of-line to permit unique_ptr<incomplete-type>

    Tree(Tree const&)            = delete;
    Tree& operator=(Tree const&) = delete;
    Tree(Tree&&) noexcept;
    Tree& operator=(Tree&&) noexcept;

    // ── identity ──
    [[nodiscard]] TreeId                     id()        const noexcept;
    [[nodiscard]] SourceBuffer const&        source()    const noexcept;
    // The owning `shared_ptr` to this tree's source buffer. Needed to
    // populate a `BufferRegistry` (BufferId -> shared_ptr<SourceBuffer>)
    // for positioned diagnostic rendering (plan 06 V2-4 Part A) without
    // copying the buffer text. Unlike `source()` (which dereferences and
    // requires non-null), this returns the raw pointer — it MAY be null
    // for a hand-fabricated `TreeData`; callers that cannot guarantee a
    // builder-produced tree should check before use.
    [[nodiscard]] std::shared_ptr<SourceBuffer> sourceShared() const noexcept;
    [[nodiscard]] RuleInterner const&        rules()     const noexcept;
    [[nodiscard]] NodeId                     root()      const noexcept;
    [[nodiscard]] std::size_t                nodeCount() const noexcept;

    // Aborts (release-fatal) if no schema / no reporter attached.
    // Hand-fabricated trees in tests may build without either; the builder
    // path always attaches both. Callers that want to probe rather than
    // dereference use `hasSchema()` / `hasDiagnostics()`.
    [[nodiscard]] GrammarSchema const&       schema()         const noexcept;
    [[nodiscard]] DiagnosticReporter const&  diagnostics()    const noexcept;
    [[nodiscard]] bool                       hasSchema()      const noexcept;
    [[nodiscard]] bool                       hasDiagnostics() const noexcept;

    // Rewrite the (buffer, span) of every diagnostic this tree owns through
    // `fn`. Used by the FC13 C-preprocessor pipeline to remap diagnostics
    // off the synthesized buffer (which IS this tree's `source()`) back onto
    // the real header/main file via the preprocessor's line-map, so a
    // diagnostic originating in an included header is ATTRIBUTED to that
    // header. This touches only diagnostic source-attribution metadata, not
    // the tree's node structure -- the tree stays structurally immutable.
    // No-op on a hand-built tree with no reporter.
    template <class F>
    void remapDiagnostics(F&& fn) {
        if (data_.diagnostics) data_.diagnostics->remapBuffers(std::forward<F>(fn));
    }

    // ── universal per-node accessors ──
    [[nodiscard]] NodeKind    kind(NodeId id)  const;
    [[nodiscard]] NodeFlags   flags(NodeId id) const;
    [[nodiscard]] SourceSpan  span(NodeId id)  const;
    [[nodiscard]] NodeId      parent(NodeId id) const;

    // Release-mode bounds-checked: a corrupt firstChild/childCount that overruns
    // the child-index vector aborts in release (not UB).
    [[nodiscard]] std::span<NodeId const> children(NodeId id) const;

    [[nodiscard]] std::string_view        text(NodeId id) const;

    // ── discriminant-asserting accessors ──
    // Debug-asserts on wrong kind; in release, returns the stored value
    // (which is meaningless if the discriminant is wrong, but won't crash).
    [[nodiscard]] RuleId        rule(NodeId id)      const;   // requires Internal
    [[nodiscard]] SchemaTokenId tokenKind(NodeId id) const;   // requires Token
    [[nodiscard]] std::optional<DiagnosticIndex> diagnostic(NodeId id) const;

    // ── cursors ──
    //
    // CST cursor visits every node (incl. EmptySpace leaves); AST cursor
    // skips by NodeFlags::EmptySpace. Both start positioned at the root.
    // On an empty tree (root == InvalidNode), both methods return a
    // cursor whose current() is InvalidNode; movement methods will fail
    // cleanly without dereferencing past the arena.
    [[nodiscard]] class TreeCursor cursor()    const;
    [[nodiscard]] class TreeCursor astCursor() const;

private:
    // Bounds-check `id` against the arena. Aborts on invalid id (release-mode).
    [[nodiscard]] detail::Node const& node_(NodeId id) const;

    detail::TreeData data_;
};

} // namespace dss
