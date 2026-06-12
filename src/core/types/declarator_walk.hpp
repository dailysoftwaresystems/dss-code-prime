#pragma once

#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <cstddef>
#include <vector>

// FC4 c1 (M2): the ONE declarator name-extraction walk, shared by BOTH the
// parser's binder sketch (analysis/syntactic — mid-parse, over `TreeBuilder`
// finalized-below-the-frame nodes) and the semantic analyzer
// (analysis/semantic — over frozen `Tree`s). The walk is written ONCE as a
// template over a tiny node-view adapter (kind / rule / tokenKind / visible
// children) so the two substrates cannot drift — the decl-prefix-strip
// lesson (three diverging file-local copies) applied preemptively.
//
// THE WALK (C 6.7.6 shapes, by config-resolved ROLE — never by rule name):
//   initDeclaratorRule  →  descend to its declaratorRule child
//   declaratorRule      →  descend to its directRule child
//                          (pointerLayerRule children are SKIPPED — stars
//                          carry type structure, never the name)
//   directRule          →  a `nameToken` Token child IS the name;
//                          else descend the groupRule child's declarator
//                          (`(*x)` — the name nests inside the parens);
//                          neither present ⇒ ABSTRACT declarator — a legal
//                          outcome (a parameter type with no name), the
//                          walk returns InvalidNode, never an error.
//
// Layering: header-only over `core/types` vocabulary (Tree + DeclaratorConfig)
// — visible to every consumer, no cross-library edge (the decl_prefix_strip
// precedent). The parser supplies its own TreeBuilder adapter at its sole
// call site; the Tree adapter ships here (two consumers already).

namespace dss {

namespace declarator_walk_detail {

// Hard cap on descent steps. A well-formed declarator chain is depth ~N for
// N grouping parens; 4096 is unreachable by real input and turns a cyclic /
// corrupted node graph into a bounded miss instead of a hang.
inline constexpr std::size_t kMaxDeclaratorDepth = 4096;

// First visible child of `node` that is an Internal node of `rule`, or
// InvalidNode. The role-targeted descent step every arm of the walk uses.
template <class View>
[[nodiscard]] NodeId firstChildOfRule(View const& v, NodeId node, RuleId rule) {
    for (NodeId c : v.children(node)) {
        if (!v.isVisible(c)) continue;
        if (v.kind(c) == NodeKind::Internal && v.rule(c) == rule) return c;
    }
    return {};
}

} // namespace declarator_walk_detail

// The name-bearing `nameToken` leaf declared by the declarator (or
// initDeclarator / direct) rooted at `node`, or InvalidNode for an ABSTRACT
// declarator (no name — legal) and for any node outside the declarator role
// shapes (the caller's signal that the subtree is not a declarator at all,
// e.g. an error-recovery shape — the safe degrade direction, mirroring how
// positional nameChild extraction tolerates structurally-absent children).
template <class View>
[[nodiscard]] NodeId declaratorNameNode(View const& v, NodeId node,
                                        DeclaratorConfig const& dc) {
    namespace det = declarator_walk_detail;
    NodeId cur = node;
    for (std::size_t step = 0; step < det::kMaxDeclaratorDepth; ++step) {
        if (!cur.valid() || v.kind(cur) != NodeKind::Internal) return {};
        RuleId const r = v.rule(cur);
        if (r == dc.initDeclaratorRule) {
            cur = det::firstChildOfRule(v, cur, dc.declaratorRule);
            continue;
        }
        if (r == dc.declaratorRule) {
            cur = det::firstChildOfRule(v, cur, dc.directRule);
            continue;
        }
        if (r == dc.directRule) {
            for (NodeId c : v.children(cur)) {
                if (!v.isVisible(c)) continue;
                if (v.kind(c) == NodeKind::Token
                    && v.tokenKind(c) == dc.nameToken) {
                    return c;
                }
            }
            NodeId const group = det::firstChildOfRule(v, cur, dc.groupRule);
            if (!group.valid()) return {};   // abstract declarator — no name
            cur = det::firstChildOfRule(v, group, dc.declaratorRule);
            continue;
        }
        return {};   // not a declarator-role shape
    }
    return {};   // depth cap — corrupted/cyclic input, bounded miss
}

// Collect the declarator-carrying nodes below a declaration's
// list-or-single child, in source order:
//   listRule node          → its initDeclaratorRule / declaratorRule
//                            Internal children (commas skipped);
//   initDeclaratorRule or
//   declaratorRule node    → the node itself (the single-declarator form);
//   anything else          → nothing (structurally absent / errored decl —
//                            the caller mints no symbols, the safe degrade).
template <class View>
void collectDeclarators(View const& v, NodeId node, DeclaratorConfig const& dc,
                        std::vector<NodeId>& out) {
    if (!node.valid() || v.kind(node) != NodeKind::Internal) return;
    RuleId const r = v.rule(node);
    if (r == dc.listRule) {
        for (NodeId c : v.children(node)) {
            if (!v.isVisible(c)) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            RuleId const cr = v.rule(c);
            if (cr == dc.initDeclaratorRule || cr == dc.declaratorRule) {
                out.push_back(c);
            }
        }
        return;
    }
    if (r == dc.initDeclaratorRule || r == dc.declaratorRule) {
        out.push_back(node);
    }
}

// ── the Tree adapter (the frozen-tree consumer; the parser's TreeBuilder
//    adapter lives at its call site in parser.cpp) ──
struct TreeDeclaratorView {
    Tree const& tree;
    [[nodiscard]] NodeKind kind(NodeId n) const { return tree.kind(n); }
    [[nodiscard]] RuleId rule(NodeId n) const { return tree.rule(n); }
    [[nodiscard]] SchemaTokenId tokenKind(NodeId n) const {
        return tree.tokenKind(n);
    }
    [[nodiscard]] bool isVisible(NodeId n) const {
        return !isEmptySpace(tree.flags(n));
    }
    [[nodiscard]] auto children(NodeId n) const { return tree.children(n); }
};

[[nodiscard]] inline NodeId
declaratorNameNode(Tree const& tree, NodeId node, DeclaratorConfig const& dc) {
    return declaratorNameNode(TreeDeclaratorView{tree}, node, dc);
}

inline void collectDeclarators(Tree const& tree, NodeId node,
                               DeclaratorConfig const& dc,
                               std::vector<NodeId>& out) {
    collectDeclarators(TreeDeclaratorView{tree}, node, dc, out);
}

} // namespace dss
