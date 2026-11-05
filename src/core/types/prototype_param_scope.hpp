#pragma once

#include "core/types/declarator_walk.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

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
// ★ P69 L2b1 (lane `lm`, the L2a2 review's A3): WHICH SUFFIX DECLARES THE NAME A FUNCTION is the
// declarator walk's one question (`declaratorFnSuffixOf`, core/types/declarator_walk.hpp, the walk
// the analyzer and the HIR lowering ask through `declaratorFnSuffixNode`). This header kept a copy
// of it — a top-down descent written beside the bottom-up climb because a mid-parse builder has no
// parent link — equal to it only by reading; the walk is top-down now, and this header asks it.

namespace dss {

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
    NodeId const own = declaratorFnSuffixOf(v, declarator, dc);
    return !(own.valid() && own.v == fnSuffix.v);
}

} // namespace dss
