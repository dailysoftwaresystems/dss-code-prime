#pragma once

#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <cstdint>
#include <optional>
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

// ── VLA C4c (D-CSUBSET-VLA, C99 §6.7.6.2/6.7.6.3): array-suffix bound locating ──
//
// A C99 array-PARAMETER declarator suffix (`arrayDeclSuffix`) may carry a leading
// `static` and/or cv-qualifier run and a bound that is either an expression or
// absent — `int a[static n]`, `int a[const 3]`, `int a[]`. (The bare
// unspecified-size `int a[*]` form is its OWN `arrayStarSuffix` rule
// [D-CSUBSET-VLA-PARAM-STAR], NOT this suffix, so it never reaches these
// helpers.) The grammar admits the decorations on the ONE shared
// `arrayDeclSuffix` (they are legal only in a parameter; a non-parameter use is
// caught semantically). A leading decoration SHIFTS the bound off its former
// fixed child index, so EVERY bound-locating site scans past the decorations via
// these two helpers rather than assuming "the bound is child N". Direct children
// ONLY — never recurse into the bound expression's own subtree (a `const`/`*`
// inside a cast/sizeof bound is NOT a suffix decoration). Engine-agnostic: the
// decoration token-kind set is per-language config
// (`DeclaratorConfig.arraySuffixModifierTokens`); an EMPTY set degrades both
// helpers to the plain first-non-bracket-child view (the pre-C4c behavior).

namespace array_suffix_detail {

// Is `child` one of the configured array-parameter decoration tokens
// (static / const / volatile / restrict / `*`)? A decoration is always a BARE
// token — a `*p`-form deref bound (`int a[*p]`) is an Internal expression node,
// NOT a bare `*` token, so a legal deref-sized VLA is never mistaken for a
// bare-`*` decoration token. (`StarOp` stays in the set defensively; a real
// `arrayDeclSuffix` never carries a bare `*` — the bare `[*]` is arrayStarSuffix.)
[[nodiscard]] inline bool
isArraySuffixModifierToken(Tree const& tree, NodeId child,
                           std::span<SchemaTokenId const> modifierTokens) {
    if (tree.kind(child) != NodeKind::Token) return false;
    SchemaTokenId const tk = tree.tokenKind(child);
    for (SchemaTokenId const m : modifierTokens)
        if (tk == m) return true;
    return false;
}

} // namespace array_suffix_detail

// The length-BOUND child of an array-declarator suffix, or nullopt when the
// suffix carries no bound (`[]`, `[const]`). Skips the bracket delimiters
// (first + last visible child) and any array-parameter decoration tokens; the
// first remaining child is the bound (an expression node, or — for languages
// where a scalar bound is a bare token — that token).
[[nodiscard]] inline std::optional<NodeId>
arraySuffixBoundNode(Tree const& tree, NodeId suffix,
                     std::span<SchemaTokenId const> modifierTokens) {
    auto const kids = decl_prefix_detail::visibleChildren(tree, suffix);
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (i == 0 || i + 1 == kids.size()) continue;   // skip `[` and `]`
        if (array_suffix_detail::isArraySuffixModifierToken(tree, kids[i],
                                                            modifierTokens))
            continue;
        return kids[i];
    }
    return std::nullopt;
}

// Does the array-declarator suffix carry ANY array-parameter decoration
// (static / cv-qualifier / `*`)? Such a decoration is legal ONLY in a
// function-parameter declarator (C 6.7.6.3p7); a non-parameter use is a
// constraint violation (S_ArrayParamQualifierNonParameter). Direct children
// only — a `[*p]` deref bound is an Internal node, not a bare `*` token, so a
// legal deref-sized VLA never trips this.
[[nodiscard]] inline bool
arraySuffixHasModifier(Tree const& tree, NodeId suffix,
                       std::span<SchemaTokenId const> modifierTokens) {
    for (NodeId const c : decl_prefix_detail::visibleChildren(tree, suffix))
        if (array_suffix_detail::isArraySuffixModifierToken(tree, c,
                                                            modifierTokens))
            return true;
    return false;
}

} // namespace dss
