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

// The declaration's HEAD node — the role child that holds its type specifiers
// (`headChild` of a declarator-mode row, `typeChild` of a positional one), or
// InvalidNode when the row names neither or the child is structurally absent.
[[nodiscard]] inline NodeId
declarationHeadNode(Tree const& tree, NodeId node, DeclarationRule const& decl) {
    auto const headIdx = decl.headChild.has_value() ? decl.headChild : decl.typeChild;
    if (!headIdx.has_value()) return {};
    auto const kids = declRoleChildren(tree, node, decl);
    return *headIdx < kids.size() ? kids[*headIdx] : NodeId{};
}

// ★★ P69 (lane `cs`) — WHICH SPELLING AN ATTRIBUTE SPECIFIER IS WRITTEN IN.
//
// `attributeSpellingOf` answers for a specifier node (a node of the language's
// `attributeSemantics.attrSpecRule`): the `attributeSemantics.spellings` row whose
// introducer is the node's FIRST token, or nullptr when the language declares no
// such row for it (every specifier of a language that declares no spellings, and
// the standard `[[…]]` form, which is another rule and has no spelling row).
//
// `enclosingAttributeSpelling` answers for anything written INSIDE a specifier — a
// clause node, a clause-name token: the spelling of the nearest specifier that
// holds it. The walk goes up the parent chain and stops at the first attribute
// specifier of either form, so a clause of the standard form nested anywhere is
// never given the spelling of some specifier further out.
//
// Both tiers that look an attribute NAME up (the semantic attribute fold and the
// lowering's linkage fold) ask here and nowhere else, so the two can never come to
// read one clause in two spellings.
[[nodiscard]] inline AttributeSpelling const*
attributeSpellingOf(SemanticConfig const& cfg, Tree const& tree, NodeId attrNode) {
    if (cfg.attributeSpellings.empty() || !attrNode.valid()) return nullptr;
    if (tree.kind(attrNode) != NodeKind::Internal) return nullptr;
    if (!cfg.attrSpecRule.valid() || tree.rule(attrNode).v != cfg.attrSpecRule.v) {
        return nullptr;
    }
    for (NodeId c : decl_prefix_detail::visibleChildren(tree, attrNode)) {
        if (tree.kind(c) != NodeKind::Token) return nullptr;   // no opening token
        SchemaTokenId const k = tree.tokenKind(c);
        for (AttributeSpelling const& sp : cfg.attributeSpellings) {
            if (sp.introducer.v == k.v) return &sp;
        }
        return nullptr;   // the FIRST token decides, and no row names it
    }
    return nullptr;
}

[[nodiscard]] inline AttributeSpelling const*
enclosingAttributeSpelling(SemanticConfig const& cfg, Tree const& tree, NodeId n) {
    if (cfg.attributeSpellings.empty() || !cfg.attrSpecRule.valid()) return nullptr;
    for (NodeId cur = n; cur.valid(); cur = tree.parent(cur)) {
        if (tree.kind(cur) != NodeKind::Internal) continue;
        RuleId const r = tree.rule(cur);
        if (r.v == cfg.attrSpecRule.v) return attributeSpellingOf(cfg, tree, cur);
        if (cfg.stdAttrRule.valid() && r.v == cfg.stdAttrRule.v) return nullptr;
    }
    return nullptr;
}

// ★★ P69 (lane `cs`) — THE SPECIFIERS, WRITTEN AFTER A COMPOSITE'S BODY, THAT ARE THE
// DECLARATION'S AND NOT THE TYPE'S. `list` is one composite-attribute list of a
// specifier that DEFINES here, standing after the body. A specifier in it whose
// spelling says so (`AttributeSpelling::afterCompositeBodyIsTheDeclarations`) is
// appended to `out`, in source order; every other one stays the definition's own.
//
// ONE function, called from both sides of the question — `headAttributeRuns` (which
// hands these to the declaration's readers) and the composite scan (which must then
// NOT read them as the type's) — so a specifier can be neither read twice nor
// dropped between the two. It never enters a specifier (an argument may hold a type
// name with specifiers of its own).
inline void
appendDeclarationSpecifiersAfterBody(SemanticConfig const& cfg, Tree const& tree,
                                     NodeId list, std::vector<NodeId>& out) {
    if (cfg.attributeSpellings.empty() || !list.valid()) return;
    std::vector<NodeId> stack{list};
    while (!stack.empty()) {
        NodeId const cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) != NodeKind::Internal) continue;
        if (isAttributeSpecifierRule(cfg, tree.rule(cur))) {
            AttributeSpelling const* const sp = attributeSpellingOf(cfg, tree, cur);
            if (sp != nullptr && sp->afterCompositeBodyIsTheDeclarations) {
                out.push_back(cur);
            }
            continue;
        }
        auto const kids = decl_prefix_detail::visibleChildren(tree, cur);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }
}

// ★★ P69 round 4 (lane `cs`) — THE ATTRIBUTE RUNS WRITTEN INSIDE A TYPE HEAD, in
// source order: a run among a type's specifiers (`unsigned __attribute__((…)) int`,
// the language's `attributeSemantics.specifierRunRule`) and the list after a tag that
// is only REFERRED to (`struct S __attribute__((…)) w;`, the composite attribute
// list of a specifier that defines nothing here).
//
// They are not part of the type the head names — every positional reader of a head
// skips them — and they are not a composite definition's own lists either. What they
// decorate is decided by WHERE THE HEAD STANDS, which is why this is one walk with
// several consumers instead of a rule inside any of them: in a DECLARATION they are
// the declaration's (`unsigned __attribute__((aligned(16))) int v;` aligns `v`; gcc
// 13.3.0, clang 18.1.3, Apple clang and mingw-w64 gcc agree, lane `cs`'s probe ta3),
// so the attribute fold, the noreturn fold and the lowering's linkage fold all read
// this one list and cannot come to see different positions; in a TYPE NAME they are
// the named type's.
//
// The walk never enters an attribute specifier, a nested type name (its runs are its
// own) or a declaration row: a row that DEFINES here owns its lists (the composite
// scan reads them), and a row that only refers contributes its direct lists and is
// not entered further — an enumeration's underlying type has its own specifiers,
// which are not this head's. A run can therefore be reached only through the head's
// own wrappers, and a grammar that puts an expression inside a head (a bit-precise
// width, a `typeof` operand) cannot lend this walk a run of some type named in it.
//
// ★ A SPECIFIER THAT IS A DIRECT CHILD OF THE HEAD, WITHOUT THE RUN RULE, IS A HEAD
// RUN TOO. The shipped C grammar wraps every such specifier in `specifierRunRule`,
// so this arm adds nothing there; it exists for the grammar that does not — a
// language document that places an attribute slot in its type-resolved head's own
// sequence. Every head reader skips the specifier (it is opaque to them), so without
// this arm NOBODY would read it and its effect would be dropped in silence: the
// typedef-head pin `TypeHeadHijackSweep.AnAttributeInsideTheHeadIsTheDeclarationsOwn`
// holds that shape to "honoured", where before the specifiers were opaque it was a
// refusal. DIRECT children only: a bare specifier deeper in the head can be a
// statement's (a statement expression inside a `typeof` operand), which is not this
// declaration's to fold — a run among specifiers cannot be, it only ever stands in a
// specifier sequence, under a type name or a declaration row the walk does not enter.
//
// Explicit stack (no input-proportional recursion); every node of the head is seen
// at most once, and there is no bound to fall off: a run past a bound would be a
// run nobody reads.
[[nodiscard]] inline std::vector<NodeId>
headAttributeRuns(SemanticConfig const& cfg, Tree const& tree, NodeId head) {
    std::vector<NodeId> out;
    if (!head.valid()) return out;
    bool const haveRun  = cfg.attrSpecifierRunRule.valid();
    bool const haveList = cfg.compositeAttrListRule.valid();
    // A language that declares no attribute shape at all has nothing to find.
    if (!haveRun && !haveList && !cfg.attrSpecRule.valid() && !cfg.stdAttrRule.valid()) {
        return out;
    }
    std::vector<NodeId> stack{head};
    while (!stack.empty()) {
        NodeId const cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur) != NodeKind::Internal) continue;
        RuleId const r = tree.rule(cur);
        if (haveRun && r.v == cfg.attrSpecifierRunRule.v) {
            out.push_back(cur);
            continue;
        }
        if (isAttributeSpecifierRule(cfg, r)) {
            if (tree.parent(cur).v == head.v) out.push_back(cur);
            continue;
        }
        if (cur.v != head.v && cfg.attrTypeNameRule.valid()
            && r.v == cfg.attrTypeNameRule.v) {
            continue;
        }
        DeclarationRule const* row = nullptr;
        for (DeclarationRule const& d : cfg.declarations) {
            if (d.rule.v == r.v) { row = &d; break; }
        }
        auto const kids = decl_prefix_detail::visibleChildren(tree, cur);
        if (row != nullptr) {
            bool defines = !row->definesWhenChildRule.has_value();
            if (!defines) {
                for (NodeId c : kids) {
                    if (tree.kind(c) == NodeKind::Internal
                        && tree.rule(c).v == row->definesWhenChildRule->v) {
                        defines = true;
                        break;
                    }
                }
            }
            if (!haveList) continue;
            if (defines) {
                // ★★ P69 (lane `cs`): A DEFINITION OWNS ITS LISTS — EXCEPT WHAT A
                // SPELLING SAYS IS THE DECLARATION'S. After the body
                // (`struct S { … } __declspec(align(32)) s;`) a specifier of such a
                // spelling is an ordinary specifier of the declaration: it is
                // handed out here, one specifier at a time, and the composite scan
                // skips exactly the same ones
                // (`appendDeclarationSpecifiersAfterBody`). A row that defines
                // unconditionally has no body to stand after.
                if (!row->definesWhenChildRule.has_value()) continue;
                bool pastBody = false;
                for (NodeId c : kids) {
                    if (tree.kind(c) != NodeKind::Internal) continue;
                    if (tree.rule(c).v == row->definesWhenChildRule->v) {
                        pastBody = true;
                        continue;
                    }
                    if (pastBody && tree.rule(c).v == cfg.compositeAttrListRule.v) {
                        appendDeclarationSpecifiersAfterBody(cfg, tree, c, out);
                    }
                }
                continue;
            }
            for (NodeId c : kids) {
                if (tree.kind(c) == NodeKind::Internal
                    && tree.rule(c).v == cfg.compositeAttrListRule.v) {
                    out.push_back(c);
                }
            }
            continue;
        }
        // Reverse-push so the pop order is source order.
        for (auto it = kids.rbegin(); it != kids.rend(); ++it) stack.push_back(*it);
    }
    return out;
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
