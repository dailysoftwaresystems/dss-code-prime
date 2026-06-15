#pragma once

#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <cstdint>
#include <span>
#include <vector>

// D-DECL-PREFIX-STRIP-SHARED-HELPER (closed 2026-06-11): the ONE source of
// truth for the declaration-specifier-prefix strip guard that previously
// existed as THREE file-local copies — the semantic analyzer's
// `declRoleChildren`/`descendVisibleDecl`, cst_const_eval's inline strip in
// `findInitExprInDecl`, and CST→HIR's `specifierPrefix`/`declVisible`/
// `descendDecl` member helpers. A fix applied to one copy but not the others
// is exactly the missed-site drift class this extraction kills.
//
// THE STRIP RULE (D-DECL-SPECIFIER-PREFIX-SUBSTRATE): when a DeclarationRule
// declares a `specifierPrefixRule` AND the declaration node's FIRST VISIBLE
// child is an Internal node of that rule (e.g. C `static int f()` /
// `__attribute__((weak)) int g`), that leading child is dropped before
// positional-child counting — so the schema-authored `name`/`type`/`init`/
// `params`/`body`/`kindByChild` indices stay stable whether or not specifiers
// are present. The prefix subtree itself remains reachable via
// `specifierPrefixChild` for per-language specifier scans (CST→HIR
// `linkageFrom`). With no prefix declared/present the helpers degrade to the
// plain visible-children view (the no-op that keeps every prefix-free
// declaration unchanged).
//
// Engine-agnostic: WHICH rule is the prefix, and what its specifiers mean,
// are entirely per-language config — nothing here names a language.
//
// Layering: header-only free functions over `Tree` + `DeclarationRule`, both
// `core/types` — visible to ALL consumers (analysis_semantic, hir,
// hir_lowering all link `core`) with no cross-library edge. NOTE the parser's
// binder-sketch keeps its own strip (`parser.cpp` `extractBinderName_`): it
// runs MID-PARSE over `TreeBuilder` pending frames (no finished `Tree`
// exists yet) against the sketch's plain-`RuleId` row, so it cannot take
// these `Tree const&` helpers — a deliberate non-consumer, not a missed site.

namespace dss {

namespace decl_prefix_detail {

// Visible (non-EmptySpace) children of `parent` — the indexing convention the
// v4 `semantics` block uses for declaration child positions. Internal to the
// strip helpers; consumers keep their own file-local enumerators for
// non-declaration walks (this header owns only the decl-prefix discipline).
[[nodiscard]] inline std::vector<NodeId>
visibleChildren(Tree const& tree, NodeId parent) {
    std::vector<NodeId> out;
    for (auto const& child : tree.children(parent)) {
        if (!isEmptySpace(tree.flags(child))) out.push_back(child);
    }
    return out;
}

} // namespace decl_prefix_detail

// The declaration's specifier-prefix subtree, or InvalidNode when the rule
// declares no `specifierPrefixRule` / the first visible child is not an
// Internal node of that rule (incl. a Token first child, or no children).
// The accessor consumers use to SCAN the prefix they otherwise strip.
[[nodiscard]] inline NodeId
specifierPrefixChild(Tree const& tree, NodeId node,
                     DeclarationRule const& decl) {
    if (!decl.specifierPrefixRule.has_value()) return {};
    auto kids = decl_prefix_detail::visibleChildren(tree, node);
    if (!kids.empty() && tree.kind(kids.front()) == NodeKind::Internal
        && tree.rule(kids.front()) == *decl.specifierPrefixRule)
        return kids.front();
    return {};
}

// A declaration's "role children" — its visible children with a leading
// declaration-specifier prefix stripped per the strip rule above. Positional
// name/type/init/params/body/kindByChild indices resolve against THESE.
[[nodiscard]] inline std::vector<NodeId>
declRoleChildren(Tree const& tree, NodeId node, DeclarationRule const& decl) {
    auto kids = decl_prefix_detail::visibleChildren(tree, node);
    if (decl.specifierPrefixRule.has_value() && !kids.empty()
        && tree.kind(kids.front()) == NodeKind::Internal
        && tree.rule(kids.front()) == *decl.specifierPrefixRule) {
        kids.erase(kids.begin());
    }
    return kids;
}

// Visible-child descent whose FIRST path step indexes the declaration's
// role-children (specifier-prefix stripped — `start` IS the declaration);
// every LATER step descends via the ordinary RAW visible children (a nested
// node is not itself a declaration, so nothing below the first step is
// re-stripped). Lets a `kindByChild` childPath be authored against the same
// prefix-free numbering as name/type/etc. Empty path returns `start`;
// any out-of-range step returns InvalidNode.
[[nodiscard]] inline NodeId
descendVisibleDecl(Tree const& tree, NodeId start,
                   std::span<std::uint32_t const> path,
                   DeclarationRule const& decl) {
    if (path.empty()) return start;
    auto roleKids = declRoleChildren(tree, start, decl);
    if (path.front() >= roleKids.size()) return {};
    NodeId cur = roleKids[path.front()];
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (!cur.valid()) return {};
        auto kids = decl_prefix_detail::visibleChildren(tree, cur);
        if (path[i] >= kids.size()) return {};
        cur = kids[path[i]];
    }
    return cur;
}

} // namespace dss
