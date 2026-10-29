#pragma once

#include "core/substrate/walk_steps.hpp"
#include "core/types/hir_lowering_config.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
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

// ── WHAT A WRAPPER IS — the ONE statement an initializer descent takes its steps by ─────
// (cycle P69 round 1, lane `lm`.)
//
// A node is a WRAPPER when it is nothing but grouping: it has exactly one non-token child
// and it is none of the constructs below, so its value, its type and its meaning are that
// child's (a parenthesized expression, `__extension__ e`, an alternative's or an operand's
// carrier). Every OTHER node says something itself, whatever the number of its children —
// and a descent that walks through one hands its caller the OPERAND for the expression:
//   * an operator application — unary (`-e`, `!e`, `~e`, `&e`, `*e`, `++e`), binary,
//     postfix (`e++`, a call, an index, a member access), conditional;
//   * `sizeof` / `_Alignof` in every form (the alternative's node, and the type and value
//     forms under it): the operand is not evaluated, its SIZE is the value;
//   * a cast, a compound literal, a brace list;
//   * a compile-time selection (`_Generic`, `__builtin_choose_expr`), a label address, a
//     statement expression;
//   * a builtin the language document folds to a constant the semantic tier computes
//     (`hirLowering.foldedConstantRules`: `__builtin_offsetof`, `__builtin_types_compatible_p`)
//     — its operands are TYPE names and a member designator, none of them the value.
// (A literal's node — a string, a character — has no child to walk into, so no descent
// needs it named; a caller that wants one asks for it by its own means.)
//
// ★ WHY IT IS ONE STATEMENT, AND WHY IT IS HERE. It was written once, inside the semantic
// tier's static-initializer check (lane `cs`), and FOUR other descents counted children
// instead — three in the semantic analyzer (the initializer descent, the constant-subobject
// peel, the brace walks' peel) and the HIR lowering's `peelToBraceInitOrCore`: "exactly one
// child that is not a token" was their whole test. A unary operator's node has exactly one —
// its operator is a token. ✔MEASURED 2026-10-10 (pe64, baseline and release alike; 51 kinds
// of initializer, each read in a condition, a static assertion, an enumerator with an array
// bound, and on a `constexpr` object: 134 of 247 cells wrong):
//   * `static const struct S g = {.b = -5};` — the CONSTANT read of `g.b` was 5. The static
//     assertion `g.b == -5` failed and `g.b == 5` passed; an enumerator and an array bound
//     built on it were wrong; and `if (g.b != -5) return 7; return 42;` died of an illegal
//     instruction, because the lowering believes a constant condition and marks what
//     follows the statement as never reached. The same for `!e`, `~e`, `(-e)`,
//     `__extension__ -e`, `sizeof e` (read as the VALUE of `e`), `__alignof__(e)`, a
//     statement expression, for a nested, a positional and a union member, a compound
//     literal, an automatic `const` object, and for the `while`, `for` and `do` forms;
//   * `{.a = sizeof(int)}` — the walk went on INTO the type name: the member was no
//     constant at all, and `constexpr struct S g = {.a = sizeof n};` was refused;
//   * `constexpr int *p = !(int *)0;` was ACCEPTED: the null-pointer-cast question was
//     asked of the operand;
//   * `char s[] = { *"abc" };` was taken for a string-initialized array by BOTH tiers and
//     refused by the MIR verifier (a `store` of a character into an array).
// The constant read of the image was never wrong at RUN time: only what the compiler
// concluded from it. Both tiers and the evaluator already include this header for their
// shared declaration descents; it is `core`, so neither tier reaches upward for it.
//
// A count is not a decision — a construct with ONE operand is the commonest kind there
// is. A construct the grammar gains is named HERE, or it is a wrapper to every descent
// that reads this: that is the reason there is one of these.
//
// ★ EVERY DESCENT READS IT (cycle P69 round 1, lane `lm`; a review of the fold found six
// that took their steps by a list or a count of their own, and this comment saying they
// did not): the semantic tier's initializer descents, constant-subobject peel, brace walks
// (`peelSingleChildrenTo`), builtin-operand peel (`peelBuiltinOperand`), the search of
// `completeIncompleteArrayFromInit` and the `typeof` bit-field probe (`resolveTypeNodeImpl`,
// which a later review found descending every node of one internal child); the evaluator's
// `wrapperChild`, `valueStepChild` and `evalNode`'s wrapper tail (`hir/cst_const_eval.cpp`),
// whose operators ask `bitFieldValueWidth` through the tier; the lowering's
// `peelToBraceInitOrCore`, `peelToCore` and `peelToDesignatorLeaf`
// (`hir/lowering/cst_to_hir.cpp`); and `bitFieldValueWidth`
// below. A descent asking a VALUE question takes the steps the statement leaves to it
// (the statement expression's sole expression, a selection's result) in its own body,
// by name — never by counting children.
//
// THE RULE IDS: each construct is read from ONE key of the language document — the
// operators, the `sizeof` / `_Alignof` alternatives, the selection, label-address and
// statement-expression rules and the brace list from `hirLowering`; the `sizeof` /
// `_Alignof` forms, `_Generic`, the casts and the compound literals from `semantics` — and
// both tiers hand in the same two blocks of the same schema.

// `sizeof` / `_Alignof` in any of their forms: the operand below is not evaluated
// (C 6.5.3.4p2).
[[nodiscard]] inline bool
nodeIsAnUnevaluatedOperandForm(SemanticConfig const& cfg, HirLoweringConfig const& hirCfg,
                               Tree const& tree, NodeId n) {
    if (!n.valid() || tree.kind(n) != NodeKind::Internal) return false;
    std::uint32_t const r = tree.rule(n).v;
    auto const is = [r](RuleId rule) { return rule.valid() && rule.v == r; };
    return is(hirCfg.sizeofRule) || is(hirCfg.alignofRule)
        || is(cfg.sizeofTypeRule) || is(cfg.sizeofValueRule)
        || is(cfg.alignofTypeRule) || is(cfg.alignofValueRule);
}

[[nodiscard]] inline bool
nodeSaysSomethingItself(SemanticConfig const& cfg, HirLoweringConfig const& hirCfg,
                        Tree const& tree, NodeId n) {
    if (!n.valid() || tree.kind(n) != NodeKind::Internal) return false;
    std::uint32_t const r = tree.rule(n).v;
    auto const is = [r](RuleId rule) { return rule.valid() && rule.v == r; };
    if (is(hirCfg.unaryExprRule) || is(hirCfg.binaryExprRule)
        || is(hirCfg.postfixExprRule) || is(hirCfg.ternaryExprRule)
        || is(hirCfg.labelAddressRule) || is(hirCfg.chooseExprRule)
        || is(hirCfg.stmtExprRule) || is(cfg.genericRule)
        || is(hirCfg.braceInitListRule)
        || nodeIsAnUnevaluatedOperandForm(cfg, hirCfg, tree, n)) {
        return true;
    }
    for (RuleId const folded : hirCfg.foldedConstantRules)
        if (is(folded)) return true;
    for (auto const& row : cfg.castRules)
        if (row.rule.valid() && row.rule.v == r) return true;
    for (auto const& row : cfg.compoundLiteralRules)
        if (row.rule.valid() && row.rule.v == r) return true;
    return false;
}

// ── WHAT A STATEMENT EXPRESSION IS THE VALUE OF ─────────────────────────────────────────
// (cycle P69 round 1, lane `lm`.)
//
// A statement expression says something itself — the statement above names it — and for
// a question of FORM that is the whole answer: a string literal inside one is not "the
// string that initializes the array". ✔MEASURED 2026-10-12, `char k[] = { ({ "abc"; }) };`
// is an array of ONE element where a reference builds it (gcc 13 and mingw-w64 gcc 13.2)
// and a refused program elsewhere (clang 18, AppleClang 21); a string-initialized array
// of four on none. A descent that asks what an initializer IS stops at the node.
//
// A question of VALUE — "is this the null pointer constant", "which constant object is
// this the value of" — is not answered by the node. `({ e; })` of ONE expression has the
// value of `e`: nothing is declared, changed or discarded on the way to it. The
// references read through it: ✔MEASURED the same day, `constexpr int *p = ({ (int *)0;
// });` is accepted by gcc 13, mingw-w64 gcc 13.2 and AppleClang 21, and
// `_Static_assert(({ h; }).b == 9, "")` over a constant structure `h` by clang 18 and
// AppleClang 21. Stopping such a question at the node refused both. A descent that asks
// one takes the step below where the statement stops it — and ONLY such a descent.
//
// `statementExpressionBody` is THE reading of the construct, the lowering's too (its
// `stmtExprItems`, `hir/lowering/cst_to_hir.cpp`, asks it for the value and keeps only
// the items it runs as statements): its items are its children that are not tokens, and
// its value is the expression of the LAST item when that item is an expression
// statement — the item peeled through its single-child carriers to the first rule with
// a HIR mapping, which must be the kind `ExprStmt`; the expression is that node's first
// child that is not a token. A body ending in anything else has no value (`void`).
// ★ A LABEL ON THE LAST ITEM IS STEPPED THROUGH (cycle P69 round 1, lane `lm`): the
// labeled statement's own statement is the item read. ✔MEASURED 2026-10-15,
// `({ int r = 0; if (g) goto in; r = 1; in: r; })` is 1 for g == 0 and 0 otherwise,
// `({ x: 5; })` is 5 and two labels in a row are stepped through alike, on AppleClang 21,
// gcc 13.3 (x86_64 and arm64) and clang 18 — DSS refused the construct. The labels stepped
// through are handed back (`labels`, outermost first): the lowering places each one, on
// an empty statement, where it stood — before the value is computed — so a `goto` in the
// body reaches it. A `case` or `default` label is stepped through ALIKE (P69 L2, lane `lm`):
// its `switch` is always outside the statement expression — inside it, the `switch` would
// be the last item — so the program is refused for the jump into it
// (S_JumpIntoStatementExpression, the lowering's switch prescan), and reading no value
// there said a second, false thing about it ("its last statement is not an expression
// statement"; ✔MEASURED 2026-10-16 on `return ({ case 2: 5; });` and `({ default: 5; })`).
//
// ★ ONE ITEM, AND IT IS SAID IN ONE PLACE. `soleExpressionOfStatementExpression` is the
// step, and it steps only through a body of exactly one item. A body of several
// (`({ 1; -5; })`, which clang 18 and AppleClang 21 fold and gcc does not) is read as a
// constant by widening THIS function — to the last item, when every earlier one is
// inert — and nowhere else.
// THE WORDS OF THE REFUSAL of a statement expression outside every function body
// (`S_StatementExprAtFileScope`), said once: the semantic tier refuses the construct at its
// own node (`pass2Post`), and the lowering's guard says the same if it is ever reached on
// a model that holds no error.
inline constexpr std::string_view kStatementExprOutsideAFunctionBody =
    "a statement expression '({ ... })' is only valid inside a function (GNU C 6.1) — "
    "at file scope there is no frame to run its statements in";

struct StatementExpressionBody {
    NodeId              value{};     // the last item's expression; invalid when the body has no value
    std::size_t         items = 0;   // how many items the body holds
    std::vector<NodeId> labels;      // the labeled statements (`case` and `default` ones too) the value stands under, outermost first
};

[[nodiscard]] inline StatementExpressionBody
statementExpressionBody(HirLoweringConfig const& hirCfg, Tree const& tree, NodeId n) {
    StatementExpressionBody out;
    if (!n.valid() || tree.kind(n) != NodeKind::Internal || !hirCfg.stmtExprRule.valid()
        || tree.rule(n).v != hirCfg.stmtExprRule.v) {
        return out;
    }
    NodeId last{};
    for (NodeId c : decl_prefix_detail::visibleChildren(tree, n)) {
        if (tree.kind(c) == NodeKind::Token) continue;   // ( { } )
        ++out.items;
        last = c;
    }
    auto const mappingOf = [&](NodeId x) -> HirRuleMapping const* {
        std::uint32_t const r = tree.rule(x).v;
        for (HirRuleMapping const& m : hirCfg.ruleMappings)
            if (m.rule.v == r) return &m;
        return nullptr;
    };
    // The item's core: down its single-child carriers, stopping where the lowering stops —
    // at a token, at an expression node, at the first rule with a HIR mapping; a labeled
    // statement's mapping is stepped through to its statement (its first child that is
    // not a token) and the peel goes on. Every step is to a strict descendant, so the
    // tree's node count bounds the walk; a walk past it is a cyclic node graph, never the
    // program's fault, and stops the process.
    NodeId core = last;
    for (substrate::WalkSteps steps{tree.nodeCount(), "the statement expression's item peel"};
         core.valid() && tree.kind(core) == NodeKind::Internal; steps.step()) {
        std::uint32_t const r = tree.rule(core).v;
        if (HirRuleMapping const* const lm = mappingOf(core);
            lm != nullptr && (lm->hirKind == "LabelStmt" || lm->hirKind == "CaseStmt")) {
            // A named label's statement is its first child that is not a token; a `case` or
            // `default` label's is its LAST (`caseLabel statement`).
            NodeId stmt{};
            for (NodeId c : decl_prefix_detail::visibleChildren(tree, core)) {
                if (tree.kind(c) == NodeKind::Token) continue;
                stmt = c;
                if (lm->hirKind == "LabelStmt") break;
            }
            if (lm->hirKind == "CaseStmt" && stmt.valid()
                && tree.kind(stmt) == NodeKind::Internal && hirCfg.caseLabelRule.valid()
                && tree.rule(stmt).v == hirCfg.caseLabelRule.v) {
                stmt = NodeId{};   // the label alone: no statement
            }
            if (!stmt.valid()) return out;   // a label on nothing: no value
            out.labels.push_back(core);
            core = stmt;
            continue;
        }
        if (r == hirCfg.binaryExprRule.v || r == hirCfg.unaryExprRule.v
            || r == hirCfg.postfixExprRule.v || r == hirCfg.operandRule.v
            || (hirCfg.ternaryExprRule.valid() && r == hirCfg.ternaryExprRule.v)
            || mappingOf(core) != nullptr) {
            break;
        }
        NodeId sole{};
        bool   many = false;
        for (NodeId c : decl_prefix_detail::visibleChildren(tree, core)) {
            if (tree.kind(c) == NodeKind::Token) continue;
            if (sole.valid()) { many = true; break; }
            sole = c;
        }
        if (many || !sole.valid()) break;
        core = sole;
    }
    if (!core.valid() || tree.kind(core) != NodeKind::Internal) {
        out.labels.clear();
        return out;
    }
    HirRuleMapping const* const m = mappingOf(core);
    if (m == nullptr || m->hirKind != "ExprStmt") {
        out.labels.clear();
        return out;
    }
    for (NodeId c : decl_prefix_detail::visibleChildren(tree, core)) {
        if (tree.kind(c) == NodeKind::Token) continue;
        out.value = c;
        break;
    }
    return out;
}

// THE STEP of a VALUE question: the expression `n` is the value of, when `n` is a
// statement expression of exactly ONE item and that item is an expression statement;
// else an invalid node.
[[nodiscard]] inline NodeId
soleExpressionOfStatementExpression(HirLoweringConfig const& hirCfg, Tree const& tree, NodeId n) {
    StatementExpressionBody const body = statementExpressionBody(hirCfg, tree, n);
    return body.items == 1 ? body.value : NodeId{};
}

// ── WHICH EXPRESSION IS A BIT-FIELD'S VALUE ────────────────────────────────────────────
// (cycle P69 round 1, lane `lm`.)
//
// C 6.3.1.1p2 promotes a bit-field by its WIDTH (`bitFieldPromotedType`, type_rules.hpp),
// so an operator must know whether an operand IS a bit-field's value — and that is a
// question of the expression's FORM, asked by both tiers: the semantic typer
// (`subtreeType`) and the lowering (`asBitFieldValue`) ask THIS, and neither re-derives it.
// The answer is the width of the field, or nothing. The forms, ✔MEASURED 2026-10-15 on
// AppleClang 21, clang 18, gcc 13 and mingw-w64 gcc 13.2 (`_Generic` of the result + 0,
// and a value that tells `int` from `unsigned`), each one promoted by all four:
//   * a member access, `.` or `->`, naming a bit-field (through an element of an array of
//     structures and a nested structure alike);
//   * through a WRAPPER — parentheses, `__extension__` (`nodeSaysSomethingItself`);
//   * a SELECTION's result — `_Generic(0, int: v.b)`, `__builtin_choose_expr(1, v.b, 0)`:
//     the selected expression (`selectedArm`, the tier's record of the winner);
//   * the comma's RIGHT operand; an assignment or a compound assignment (its value is the
//     field's after the store); a PREFIX `++` / `--`.
// NOT a bit-field's value, each a fork decided by the references that answer it (clang 18
// and AppleClang 21 — the same source as the WIDER-than-`int` answer): the postfix `E++` /
// `E--` (clang and cl 19.51 give the declared type; gcc and mingw promote); a statement
// expression `({ v.b; })` (both clangs give the declared type; gcc and mingw promote — with
// the own-width bit-field type the wider answer declines). A conditional is not one either
// — its arms are each promoted and then balanced (all four agree).
// `memberWidth(n)` is the tier's: the width of the field a member-access node `n` names,
// when that field is a bit-field. `selectedArm(n)`: the expression a `_Generic` or
// `__builtin_choose_expr` node selected, or an invalid node.
// Every step is to a strict descendant (a selected arm is one), so the tree's node count
// bounds the walk; a walk past it is a cyclic node graph and stops the process.
template <class MemberWidth, class SelectedArm>
[[nodiscard]] std::optional<std::uint32_t>
bitFieldValueWidth(SemanticConfig const& cfg, HirLoweringConfig const& hirCfg, Tree const& tree,
                   NodeId n, MemberWidth const& memberWidth, SelectedArm const& selectedArm) {
    auto const entryOf = [&](NodeId node, std::vector<HirOperatorEntry> const& ops)
        -> HirOperatorEntry const* {
        for (NodeId c : decl_prefix_detail::visibleChildren(tree, node)) {
            if (tree.kind(c) != NodeKind::Token) continue;
            for (HirOperatorEntry const& e : ops)
                if (e.token.v == tree.tokenKind(c).v) return &e;
        }
        return nullptr;
    };
    auto const operand = [&](NodeId node, std::size_t which) -> NodeId {
        std::size_t k = 0;
        for (NodeId c : decl_prefix_detail::visibleChildren(tree, node)) {
            if (tree.kind(c) == NodeKind::Token) continue;
            if (k++ == which) return c;
        }
        return NodeId{};
    };
    for (substrate::WalkSteps steps{tree.nodeCount(), "the bit-field value walk"};
         n.valid() && tree.kind(n) == NodeKind::Internal; steps.step()) {
        std::uint32_t const r = tree.rule(n).v;
        auto const is = [r](RuleId rule) { return rule.valid() && rule.v == r; };
        if (is(hirCfg.postfixExprRule)) {
            HirOperatorEntry const* const e = entryOf(n, hirCfg.postfixOps);
            if (e == nullptr
                || (e->target != "MemberAccess" && e->target != "MemberAccessThruPtr")) {
                return std::nullopt;                      // a call, an index, `E++`, `E--`
            }
            return memberWidth(n);
        }
        if (is(hirCfg.binaryExprRule)) {
            HirOperatorEntry const* const e = entryOf(n, hirCfg.binaryOps);
            if (e == nullptr) return std::nullopt;
            if (e->target == "Assign") { n = operand(n, 0); continue; }   // `=` and `OP=`
            if (e->target == "Comma")  { n = operand(n, 1); continue; }
            return std::nullopt;
        }
        if (is(hirCfg.unaryExprRule)) {
            HirOperatorEntry const* const e = entryOf(n, hirCfg.unaryOps);
            if (e != nullptr && (e->target == "PreInc" || e->target == "PreDec")) {
                n = operand(n, 0);
                continue;
            }
            return std::nullopt;
        }
        if (is(cfg.genericRule) || is(hirCfg.chooseExprRule) || is(cfg.builtinChooseExprRule)) {
            n = selectedArm(n);
            continue;
        }
        if (nodeSaysSomethingItself(cfg, hirCfg, tree, n)) return std::nullopt;
        NodeId sole{};
        for (NodeId c : decl_prefix_detail::visibleChildren(tree, n)) {
            if (tree.kind(c) == NodeKind::Token) continue;
            if (sole.valid()) return std::nullopt;        // not a wrapper after all
            sole = c;
        }
        n = sole;
    }
    return std::nullopt;
}

} // namespace dss
