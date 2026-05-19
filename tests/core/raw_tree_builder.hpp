#pragma once

#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <memory>
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
    explicit RawTreeBuilder(std::string source, std::string name = "<test>")
        : src_(SourceBuffer::fromString(std::move(source), std::move(name)))
        , rules_(std::make_shared<RuleInterner>()) {
        // Slot 0 reserved as the InvalidNode sentinel.
        td_.nodes.emplace_back();
    }

    // Intern a rule name. Freezes when finish() runs.
    RuleId internRule(std::string_view name) {
        return rules_->intern(name);
    }

    // Append a node to the arena. Returns the new node's id.
    //
    // Children are flushed into the flat childIndex_ table by finish() so
    // each addNode() call is independent of insertion order.
    NodeId addNode(NodeKind                  kind,
                   RuleId                    rule,
                   SourceSpan                span,
                   NodeFlags                 flags,
                   NodeId                    parent,
                   std::vector<NodeId>       children = {},
                   SchemaTokenId             tokenKind = InvalidSchemaToken) {
        const auto value = static_cast<std::uint32_t>(td_.nodes.size());
        detail::Node n{};
        n.kind      = kind;
        n.flags     = flags;
        n.tokenKind = tokenKind;
        n.rule      = rule;
        n.span      = span;
        n.parent    = parent;
        td_.nodes.push_back(n);
        pendingChildren_.emplace_back(NodeId{value}, std::move(children));
        return NodeId{value};
    }

    // Finalize the tree. Flushes per-node children into the flat childIndex
    // table, freezes the interner, and constructs the Tree. The builder is
    // single-use.
    Tree finish(NodeId root, TreeId id = TreeId{1}) && {
        for (auto& [nodeId, kids] : pendingChildren_) {
            auto& n = td_.nodes[nodeId.v];
            n.firstChild = static_cast<std::uint32_t>(td_.childIndex.size());
            n.childCount = static_cast<std::uint32_t>(kids.size());
            for (NodeId k : kids) {
                td_.childIndex.push_back(k);
            }
        }
        rules_->freeze();
        td_.source = src_;
        td_.rules  = rules_;
        td_.id     = id;
        td_.root   = root;
        return Tree{std::move(td_)};
    }

    [[nodiscard]] std::shared_ptr<SourceBuffer> const& source() const { return src_; }
    [[nodiscard]] std::shared_ptr<RuleInterner> const& rules()  const { return rules_; }

private:
    std::shared_ptr<SourceBuffer> src_;
    std::shared_ptr<RuleInterner> rules_;
    detail::TreeData              td_;
    std::vector<std::pair<NodeId, std::vector<NodeId>>> pendingChildren_;
};

} // namespace dss::tests
