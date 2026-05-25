#pragma once

#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Test-only helper for crafting trees that `TreeBuilder` can't produce —
// e.g. trees containing pre-flagged Missing nodes, Synthetic leaves, or
// EmptySpace-flagged Internal wrappers.
//
// `TreeBuilder` is the normal way to build trees; reach for this helper
// only when a test specifically needs a shape the builder cannot generate.
//
// Usage:
//
//   RawTreeBuilder rb{"x", "<inline>"};
//   const auto rootRule = rb.internRule("root");
//   rb.addNode(/*kind=*/ NodeKind::Internal, /*rule=*/ rootRule,
//              /*span=*/ SourceSpan::of(0, 1),
//              /*flags=*/ NodeFlags::None,
//              /*parent=*/ dss::InvalidNode,
//              /*children=*/ { dss::NodeId{2}, dss::NodeId{3} });
//   ...
//   Tree t = std::move(rb).finish(/*root=*/ dss::NodeId{1});

namespace dss::tests {

class RawTreeBuilder {
public:
    explicit RawTreeBuilder(std::string source,
                            std::string name = "<test>",
                            TreeId      id   = TreeId{1})
        : src_(SourceBuffer::fromString(std::move(source), std::move(name)))
        , rules_(std::make_shared<RuleInterner>())
        , id_(id) {
        // Slot 0 reserved as the InvalidNode sentinel.
        nodes_.emplace_back();
    }

    // Intern a rule name. Freezes when finish() runs.
    RuleId internRule(std::string_view name) {
        return rules_->intern(name);
    }

    [[nodiscard]] TreeId treeId() const noexcept { return id_; }

    // Append a node to the arena. Returns the new node's id, tagged with
    // this builder's TreeId so cross-tree usage trips the validators.
    //
    // Children are flushed into the flat childIndex_ table by finish() so
    // each addNode() call is independent of insertion order. Any child
    // NodeId passed in is retagged with this builder's TreeId — callers
    // can therefore use literal `NodeId{2}` references without worrying
    // about provenance.
    NodeId addNode(NodeKind                  kind,
                   RuleId                    rule,
                   SourceSpan                span,
                   NodeFlags                 flags,
                   NodeId                    parent,
                   std::vector<NodeId>       children = {},
                   SchemaTokenId             tokenKind = InvalidSchemaToken) {
        const auto value = static_cast<std::uint32_t>(nodes_.size());
        detail::Node n{};
        n.kind      = kind;
        n.flags     = flags;
        n.tokenKind = tokenKind;
        n.rule      = rule;
        n.span      = span;
        // Retag parent if the caller passed an untagged literal; keep an
        // already-tagged-from-elsewhere id intact so the validators can
        // detect the mistake on later lookup.
        n.parent    = retagIfUntagged_(parent);
        nodes_.push_back(n);
        for (auto& k : children) k = retagIfUntagged_(k);
        pendingChildren_.emplace_back(NodeId{value, id_.v}, std::move(children));
        return NodeId{value, id_.v};
    }

    // Finalize the tree. Flushes per-node children into the flat childIndex
    // table, freezes the interner, and constructs the Tree. The builder is
    // single-use.
    //
    // The optional `id` parameter overrides the TreeId passed at
    // construction — useful when a test wants to commit to a TreeId only
    // at finish time. When overridden, every previously-emitted NodeId
    // STILL carries the construction-time tag; callers mixing the two
    // should pass the desired id at construction.
    Tree finish(NodeId root, std::optional<TreeId> id = std::nullopt) && {
        if (id) id_ = *id;
        for (auto& [nodeId, kids] : pendingChildren_) {
            auto& n = nodes_[nodeId.v];
            n.firstChild = static_cast<std::uint32_t>(td_.childIndex.size());
            n.childCount = static_cast<std::uint32_t>(kids.size());
            for (NodeId k : kids) {
                td_.childIndex.push_back(k);
            }
        }
        rules_->freeze();
        td_.source = src_;
        td_.rules  = rules_;
        td_.arena  = substrate::ArenaContainer<detail::Node, NodeId, TreeId>{std::move(nodes_), id_};
        td_.root   = retagIfUntagged_(root);
        return Tree{std::move(td_)};
    }

    [[nodiscard]] std::shared_ptr<SourceBuffer> const& source() const { return src_; }
    [[nodiscard]] std::shared_ptr<RuleInterner> const& rules()  const { return rules_; }

private:
    // Stamp untagged literal NodeIds with this builder's TreeId so
    // callers can mix `NodeId{2}` literals freely. Already-tagged ids
    // (e.g. a foreign-tree id deliberately passed in for a cross-tree
    // death test) are returned unchanged.
    [[nodiscard]] NodeId retagIfUntagged_(NodeId id) const noexcept {
        return id.treeTag == 0 ? NodeId{id.v, id_.v} : id;
    }

    std::shared_ptr<SourceBuffer> src_;
    std::shared_ptr<RuleInterner> rules_;
    TreeId                        id_;
    std::vector<detail::Node>     nodes_;   // arena under construction (slot 0 = sentinel)
    detail::TreeData              td_;
    std::vector<std::pair<NodeId, std::vector<NodeId>>> pendingChildren_;
};

} // namespace dss::tests
