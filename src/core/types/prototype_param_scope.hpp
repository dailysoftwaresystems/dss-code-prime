#pragma once

#include "core/substrate/walk_steps.hpp"
#include "core/types/declarator_walk.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <vector>

// ★ P69 L2a2 (lane `lm`, the L2a review's N9): WHICH PARAMETER LIST OPENS A FUNCTION PROTOTYPE
// SCOPE, asked ONE way by the parser's binder sketch (mid-parse, over the `TreeBuilder`'s finished
// nodes) and the semantic analyzer (over a frozen `Tree`), each through its node-view adapter.
//
// C 6.2.1p4: a parameter declared in a function declarator that is not part of a function
// definition has FUNCTION PROTOTYPE scope — it ends with that declarator. Only the parameter list
// that makes a DEFINITION's own name a function gives its names to the body. The analyzer isolated
// every other list (`isPrototypeParamScopeNode`); the sketch isolated none, so a parameter of a
// nested function declarator stayed visible to the type-name triage for the rest of its block and
// hid a typedef of its spelling: `typedef int T; … int (*fp)(int T) = 0; … return (T)(x);` parsed
// `(T)(x)` as a call. ✔MEASURED 2026-10-28 (WSL gcc 13.3.0 / clang 18.1.3, gnu2x and c2x
// -pedantic-errors, built and run): that program, a block-scope prototype `int f(int T);`, a
// typedef `typedef int (*F)(int T);` and a cast `(void)(int (*)(int T))0;`, each followed by
// `return (T)(x);`, exit 42 on all four.
//
// ★ TOP-DOWN, so the parser can ask it. `innermostNameDerivation` (declarator_walk.hpp) answers
// the same question bottom-up, from the name through parent links, which a mid-parse builder has
// not got. This walk descends from the declarator to its name, keeping every level it passed, and
// then reads those levels innermost first in EXACTLY the order the bottom-up walk climbs: the
// suffixes of the level's direct declarator (leftmost first), then the pointer layers of its
// declarator, then one parenthesis level out. The first derivation met is what the name is.

namespace dss {

// The function suffix that makes the NAME declared by `declarator` (a declarator, or a per-slot
// wrapper of one) a function — its innermost derivation, when that is a function suffix — or an
// invalid NodeId (an abstract declarator, a name that is a pointer or an array first, or a node
// outside the declarator shapes).
template <class View>
[[nodiscard]] NodeId nameDeclaringFnSuffix(View const& v, NodeId declarator,
                                           DeclaratorConfig const& dc) {
    namespace det = declarator_walk_detail;
    struct Level {
        NodeId declarator;
        NodeId direct;
        NodeId base;   // the name token, or the parenthesis group the walk descended into
    };
    std::vector<Level> levels;
    NodeId cur = declarator;
    for (substrate::WalkSteps steps{v.nodeCount(), "nameDeclaringFnSuffix's descent"};;
         steps.step()) {
        if (!cur.valid() || v.kind(cur) != NodeKind::Internal) return {};
        RuleId const r = v.rule(cur);
        if (isDeclaratorSlotWrapper(r, dc)) {
            cur = det::firstChildOfRule(v, cur, dc.declaratorRule);
            continue;
        }
        if (r != dc.declaratorRule) return {};
        NodeId const direct = det::firstChildOfRule(v, cur, dc.directRule);
        if (!direct.valid()) return {};
        NodeId name{};
        for (NodeId c : v.children(direct)) {
            if (v.isVisible(c) && v.kind(c) == NodeKind::Token && v.tokenKind(c) == dc.nameToken) {
                name = c;
                break;
            }
        }
        if (name.valid()) {
            levels.push_back(Level{cur, direct, name});
            break;
        }
        NodeId const group = det::firstChildOfRule(v, direct, dc.groupRule);
        if (!group.valid()) return {};   // abstract — no name to declare
        levels.push_back(Level{cur, direct, group});
        cur = det::firstChildOfRule(v, group, dc.declaratorRule);
    }
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        // (1) a suffix of this level's direct declarator, leftmost = innermost;
        for (NodeId c : v.children(it->direct)) {
            if (!v.isVisible(c) || c.v == it->base.v) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            if (isDeclaratorDecorationRule(dc, v.rule(c))) continue;
            return isFnSuffixRule(v.rule(c), dc) ? c : NodeId{};
        }
        // (2) else a pointer layer of its declarator: the name is a pointer;
        for (NodeId c : v.children(it->declarator)) {
            if (!v.isVisible(c) || c.v == it->direct.v) continue;
            if (v.kind(c) != NodeKind::Internal) continue;
            if (isDeclaratorDecorationRule(dc, v.rule(c))) continue;
            return {};
        }
        // (3) else one parenthesis level out.
    }
    return {};   // the name reached the top undecorated
}

// The parameter list (`prototypeParamScopeRule`) of the function suffix `fnSuffix`, or invalid.
template <class View>
[[nodiscard]] NodeId prototypeParamListOf(View const& v, NodeId fnSuffix,
                                          DeclaratorConfig const& dc) {
    if (!fnSuffix.valid() || !dc.prototypeParamScopeRule.has_value()) return {};
    return declarator_walk_detail::firstChildOfRule(v, fnSuffix, *dc.prototypeParamScopeRule);
}

// THE QUESTION: does the parameter list of `fnSuffix`, a function suffix inside `declarator`,
// open a function prototype scope? Every list does, except the one whose suffix declares the name
// of a function DEFINITION (`isDefinition`: the declaration has a body) — its names reach the body.
template <class View>
[[nodiscard]] bool paramListOpensPrototypeScope(View const& v, NodeId declarator, NodeId fnSuffix,
                                                bool isDefinition, DeclaratorConfig const& dc) {
    if (!isDefinition) return true;
    NodeId const own = nameDeclaringFnSuffix(v, declarator, dc);
    return !(own.valid() && own.v == fnSuffix.v);
}

} // namespace dss
