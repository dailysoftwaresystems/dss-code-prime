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

// ★★ D-CSUBSET-PARENTHESIZED-FUNCTION-DEFINITION-DECLARATOR-REFUSED — THE UPWARD
//    HALF OF THE DECLARATOR WALK.
//
// Everything above answers "what does this declarator DECLARE" by descending from a
// carrier to the name. These two answer the complementary question — "what does this
// declarator declare the name TO BE" — by walking back OUT from the name.
//
// C 6.7.6p1 builds a declared type from the identifier outward:
//     declarator       := pointerLayer* directDeclarator
//     directDeclarator := (name | '(' declarator ')') suffix*
// so the derivations reach the name in a fixed order: the suffixes on the name's OWN
// direct declarator first (left to right, so the LEFTMOST is innermost), then the
// pointer layers of the declarator enclosing it, and only then the SAME two questions
// one parenthesis level further out. Whatever the walk reaches FIRST is what the name
// is — a function suffix ⇒ a function, a pointer layer ⇒ a pointer, an array suffix
// ⇒ an array.
//
// ★ THE PARENTHESES ARE THE WHOLE POINT AND THEY ARE NOT DECORATION. C 6.7.6 permits
// redundant parentheses around any declarator, and `int (foo)(int x) { … }` — the
// standard glibc/musl idiom for DEFINING a name that is also a function-like macro —
// is exactly that. Both consumers of this question used to look only at the name's
// OWN direct declarator, which sees a bare `foo` with no suffix and answers "not a
// function". ✔MEASURED at the pre-fix HEAD against gcc 13.3.0 and clang 18.1.3, which
// both compile AND run every one of these, DSS produced FOUR symptoms from that ONE
// assumption: the definition was refused (S0018 "must be a function declarator"); the
// parenthesized PROTOTYPE never became one, so it bound as an object and its
// definition collided (S0002); the definition's own PARAMETERS were scoped as a
// function pointer's, so the body could not see them (S0001); and the CST→HIR param
// harvest found none, so the lowered Function's arity disagreed with its own
// signature (H0009 "Function param count 0 mismatches FnSig param count 1").
//
// ★ WHICH IS WHY IT LIVES HERE AND NOT IN ONE TIER. Those four sites span the
// semantic analyzer and the HIR lowering. Written twice, they drift — and the drift
// is not loud: three of the four symptoms above are a tier answering a shape question
// slightly differently from its neighbour. This header already exists to stop exactly
// that (see its opening note on the name-extraction walk).
//
// ⚠ Tree-ONLY, deliberately, where everything above is a template over the node-view
// adapter. This walk ascends, and the parser's mid-parse `TreeBuilder` adapter has no
// parent link to ascend through — a node is not yet attached to its frame. Both real
// consumers hold a frozen `Tree`. Making it a template would mean inventing a
// `parent()` the parser cannot honestly implement.

// The INNERMOST TYPE DERIVATION applied to `nameNode` — the single grammar node that
// decides what kind of thing the name is (a function suffix / a pointer layer / an
// array suffix), or an invalid NodeId when the name carries NO derivation at all
// (`int x;`, whose type IS the declaration head's) or is not a declarator name.
[[nodiscard]] inline NodeId
innermostNameDerivation(Tree const& tree, NodeId nameNode,
                        DeclaratorConfig const& dc) {
    TreeDeclaratorView const v{tree};
    NodeId direct = tree.parent(nameNode);
    if (!direct.valid() || v.kind(direct) != NodeKind::Internal
        || v.rule(direct) != dc.directRule) {
        return {};
    }
    // The child of `direct` the walk ASCENDED FROM — the name token at the first
    // level, the parenthesis group at every level above it. It is the BASE of the
    // direct declarator, never one of its suffixes.
    NodeId base = nameNode;
    for (std::size_t step = 0;
         step < declarator_walk_detail::kMaxDeclaratorDepth; ++step) {
        // (1) A SUFFIX on this direct declarator; leftmost = innermost. Declarator
        //     DECORATIONS (attribute runs, asm labels) annotate a declarator, they
        //     never derive a type from it, so they are skipped through the shared
        //     predicate rather than a second local list.
        for (NodeId c : v.children(direct)) {
            if (!v.isVisible(c) || c.v == base.v) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            if (isDeclaratorDecorationRule(dc, v.rule(c))) continue;
            return c;
        }
        // (2) No suffix ⇒ the enclosing declarator's POINTER LAYERS come next. This
        //     is what keeps `int (*fp)(int)` a POINTER after the walk learned to see
        //     through parentheses — the layer reaches the name before the suffix does.
        NodeId const encl = tree.parent(direct);
        if (!encl.valid() || v.kind(encl) != NodeKind::Internal
            || v.rule(encl) != dc.declaratorRule) {
            return {};
        }
        for (NodeId c : v.children(encl)) {
            if (!v.isVisible(c) || c.v == direct.v) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            if (isDeclaratorDecorationRule(dc, v.rule(c))) continue;
            return c;
        }
        // (3) Neither ⇒ step OUT one redundant-parenthesis level and ask the very
        //     same two questions of the direct declarator enclosing the group.
        //     Anything else above us (an init-declarator, the declaration row) means
        //     the name reached the top undecorated — its type IS the head's.
        NodeId const group = tree.parent(encl);
        if (!group.valid() || v.kind(group) != NodeKind::Internal
            || v.rule(group) != dc.groupRule) {
            return {};
        }
        NodeId const outer = tree.parent(group);
        if (!outer.valid() || v.kind(outer) != NodeKind::Internal
            || v.rule(outer) != dc.directRule) {
            return {};
        }
        base   = group;
        direct = outer;
    }
    return {};   // depth cap — corrupted/cyclic node graph, a bounded miss
}

// The FUNCTION SUFFIX that declares `nameNode` a function — i.e. its innermost
// derivation, when that derivation is a function suffix — or an invalid NodeId when
// the name is not a function declarator's.
//
// ★ IT RETURNS THE NODE, NOT A BOOL, because the two consumers need two different
// things from the same answer and neither can be derived from the other cheaply:
// the semantic tier asks whether there IS one (the prototype / definition-declarator
// constraint, and — by NODE IDENTITY against a given suffix — whether a parameter
// list is the definition's OWN or a prototype scope's), while the CST→HIR lowering
// needs the node itself to harvest the parameters from. A bool would force the
// lowering to re-derive the shape, which is how the two tiers came to disagree.
[[nodiscard]] inline NodeId
declaratorFnSuffixNode(Tree const& tree, NodeId nameNode,
                       DeclaratorConfig const& dc) {
    NodeId const der = innermostNameDerivation(tree, nameNode, dc);
    if (!der.valid() || tree.kind(der) != NodeKind::Internal) return {};
    return isFnSuffixRule(tree.rule(der), dc) ? der : NodeId{};
}

} // namespace dss
