#pragma once

#include "core/export.hpp"
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
#include <string_view>
#include <vector>

namespace dss {

class RuleInterner;

namespace detail {

// Movable POD that TreeBuilder hands to Tree's constructor. Keeping this in
// detail/ means consumers can't fabricate one casually — only the builder
// (and unit tests deliberately importing the detail header) produces one.
//
// CP1 leaves `schema` and `diagnostics` null; CP2 wires them up. Tree's
// accessors stay null-safe so trees built without those work in tests.
struct DSS_EXPORT TreeData {
    std::shared_ptr<SourceBuffer>        source;
    std::shared_ptr<RuleInterner const>  rules;
    std::shared_ptr<GrammarSchema const> schema;          // optional in CP1
    std::vector<Node>                    nodes;           // arena; index = NodeId.v
    std::vector<NodeId>                  childIndex;      // flat child table
    std::unique_ptr<DiagnosticReporter>  diagnostics;     // optional in CP1
    NodeId                               root = InvalidNode;
    TreeId                               id   = InvalidTree;
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
    explicit Tree(detail::TreeData&& data);
    ~Tree();    // out-of-line to permit unique_ptr<incomplete-type>

    Tree(Tree const&)            = delete;
    Tree& operator=(Tree const&) = delete;
    Tree(Tree&&) noexcept;
    Tree& operator=(Tree&&) noexcept;

    // ── identity ──
    [[nodiscard]] TreeId               id()        const noexcept;
    [[nodiscard]] SourceBuffer const&  source()    const noexcept;
    [[nodiscard]] RuleInterner const&  rules()     const noexcept;
    [[nodiscard]] NodeId               root()      const noexcept;
    [[nodiscard]] std::size_t          nodeCount() const noexcept;

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

private:
    // Bounds-check `id` against the arena. Aborts on invalid id (release-mode).
    [[nodiscard]] detail::Node const& node_(NodeId id) const;

    detail::TreeData data_;
};

} // namespace dss
