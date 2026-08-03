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
//   memberDeclaratorRule→  descend to its declaratorRule child (c23
//                          D-CSUBSET-STRUCT-MULTI-DECLARATOR — the struct/
//                          union member-list slot; OPTIONAL role, absent inner
//                          declarator ⇒ anonymous bit-field ⇒ no name)
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
        // c23 (D-CSUBSET-STRUCT-MULTI-DECLARATOR): a struct/union member-list
        // slot wraps ONE declarator (+ its own bitfield suffix). Descend to the
        // inner declaratorRule, identical to the initDeclaratorRule arm — an
        // ABSENT inner declarator (the anonymous bit-field `int : 3;`) yields {}
        // → abstract → no name, the same legal degrade. Guarded on the OPTIONAL
        // role so a language without member lists never matches here.
        if (dc.memberDeclaratorRule.has_value()
            && r == *dc.memberDeclaratorRule) {
            cur = det::firstChildOfRule(v, cur, dc.declaratorRule);
            continue;
        }
        if (r == dc.declaratorRule) {
            cur = det::firstChildOfRule(v, cur, dc.directRule);
            continue;
        }
        // c26 (D-CSUBSET-ABSTRACT-DECLARATOR-TYPE-NAME): the abstract twin of
        // `directRule` (type-name position) shares the SAME group/suffix children
        // but has NO `nameToken` base (Identifier excluded by grammar). Treat it
        // exactly like `directRule`: the name-token scan finds nothing at this
        // level (correct — abstract), but a name NESTED in its parenDeclarator
        // (`(int (x))` — a named declarator illegal in type-name position) is still
        // recovered via the group descent, so the type-name resolver's reject can
        // fire. Guarded on the OPTIONAL role (a language without it never matches).
        if (r == dc.directRule
            || (dc.directAbstractRule.has_value()
                && r == *dc.directAbstractRule)) {
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
//   memberListRule node    → its memberDeclaratorRule Internal children
//                            (commas skipped) — c23 struct/union member list;
//   initDeclaratorRule or
//   declaratorRule or
//   memberDeclaratorRule   → the node itself (the single-slot form);
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
    // TF-C88 (D-CSUBSET-TYPEDEF-MULTI-DECLARATOR): a BARE-declarator LIST —
    // `declarator (',' declarator)*`, no per-slot wrapper. Collect each
    // `declaratorRule` child (commas skipped), in source order. Structurally
    // this is the `listRule` arm minus the `initDeclaratorRule` alternative,
    // kept as its OWN arm rather than folded in because a language may declare
    // both shapes and they must not alias each other's slot grammar. Guarded on
    // the OPTIONAL role (a language without it never matches).
    if (dc.plainListRule.has_value() && r == *dc.plainListRule) {
        for (NodeId c : v.children(node)) {
            if (!v.isVisible(c)) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            if (v.rule(c) == dc.declaratorRule) out.push_back(c);
        }
        return;
    }
    // c23 (D-CSUBSET-STRUCT-MULTI-DECLARATOR): a struct/union member
    // declarator LIST — collect each per-slot `memberDeclaratorRule` child
    // (commas skipped), in source order. The downstream name/type walks
    // descend each slot to its inner declarator. Guarded on the OPTIONAL role.
    if (dc.memberListRule.has_value() && r == *dc.memberListRule) {
        for (NodeId c : v.children(node)) {
            if (!v.isVisible(c)) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            if (dc.memberDeclaratorRule.has_value()
                && v.rule(c) == *dc.memberDeclaratorRule) {
                out.push_back(c);
            }
        }
        return;
    }
    if (r == dc.initDeclaratorRule || r == dc.declaratorRule) {
        out.push_back(node);
        return;
    }
    // c23: a BARE single-slot member declarator (a one-element list the
    // grammar may collapse to the slot directly) — push self; the caller's
    // name/type walk descends it to the inner declarator.
    if (dc.memberDeclaratorRule.has_value() && r == *dc.memberDeclaratorRule) {
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

// TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME) — THE shared "this child is a
// decoration, not the initializer" predicate.
//
// Every init-detection scan in the compiler reads an init-declarator's initializer
// as "the first visible INTERNAL child that is not the declarator". That idiom is
// correct only if every OTHER thing the grammar may put between the declarator and
// its `= init` is skipped. Before TF-C88 that set was one member —
// `afterDeclaratorAttrRules` — and each of the six scan sites open-coded the loop
// (TF-C62 had to fix all six at once). The asm label is the second member, and
// open-coding it a seventh time is how the sites drift: a scan that misses it reads
// `int f(void) __asm("_x");` as an initializer and type-checks a string against a
// function type. One predicate, one place to extend.
//
// ★ WIDER THAN `isAfterDeclaratorAttrNode` ON PURPOSE. That predicate stays
// attribute-ONLY because it also gates the LINKAGE/attribute FOLDS, which must not
// be handed an asm label. This one gates the INIT scans, which must skip both.
[[nodiscard]] inline bool isDeclaratorDecorationRule(DeclaratorConfig const& dc,
                                                    RuleId r) noexcept {
    for (RuleId ar : dc.afterDeclaratorAttrRules)
        if (r.v == ar.v) return true;
    return dc.asmLabelRule.has_value() && r.v == dc.asmLabelRule->v;
}

[[nodiscard]] inline bool isDeclaratorDecorationNode(Tree const& tree,
                                                     DeclaratorConfig const& dc,
                                                     NodeId c) {
    if (!c.valid() || tree.kind(c) != NodeKind::Internal) return false;
    return isDeclaratorDecorationRule(dc, tree.rule(c));
}

// TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME) — the asm-label node attached to
// declarator carrier `dNode`, or an invalid
// NodeId when there is none. `dNode` is a carrier from `collectDeclarators` (an
// `initDeclaratorRule` node or a bare `declaratorRule` node); only the former can
// carry a label, because the run lives in the init-declarator slot. Returns the
// FIRST one — a declarator with two labels (`int x __asm("a") __asm("b");`) is
// rejected by the caller rather than silently resolved to one of them.
[[nodiscard]] inline NodeId asmLabelNodeOf(Tree const& tree, NodeId dNode,
                                           DeclaratorConfig const& dc) {
    if (!dc.asmLabelRule.has_value()) return {};
    if (!dNode.valid() || tree.kind(dNode) != NodeKind::Internal) return {};
    for (NodeId c : tree.children(dNode)) {
        if (isEmptySpace(tree.flags(c))) continue;
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c).v == dc.asmLabelRule->v) return c;
    }
    return {};
}

// TF-C88 (D-CSUBSET-ASM-LABEL-SYMBOL-RENAME) — how many asm labels `dNode`
// carries (0, 1, or the over-decorated case
// the caller must reject loud). Cheap: only ever walks one declarator's direct
// children, and returns 0 immediately for a language with no asm-label role.
[[nodiscard]] inline std::size_t asmLabelCountOf(Tree const& tree, NodeId dNode,
                                                 DeclaratorConfig const& dc) {
    if (!dc.asmLabelRule.has_value()) return 0;
    if (!dNode.valid() || tree.kind(dNode) != NodeKind::Internal) return 0;
    std::size_t n = 0;
    for (NodeId c : tree.children(dNode)) {
        if (isEmptySpace(tree.flags(c))) continue;
        if (tree.kind(c) != NodeKind::Internal) continue;
        if (tree.rule(c).v == dc.asmLabelRule->v) ++n;
    }
    return n;
}

} // namespace dss
