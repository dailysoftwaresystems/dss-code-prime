#pragma once

#include "core/types/parse_diagnostic.hpp"       // DiagnosticCode (the scan's codes)
#include "core/types/semantic_config.hpp"        // InlineAsmConfig
#include "core/types/string_literal_decode.hpp"  // decodeAdjacentStringBodies
#include "core/types/tree.hpp"                   // Tree / NodeId / NodeKind

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ── inline-asm P5: the ONE structural read of an `__asm__` statement ─────────
//
// Every fact the front end needs about an inline-asm statement, gathered in a
// single pass over the statement's own subtree, config-keyed throughout: not one
// keyword spelling, rule NAME or language identity is consulted.
//
// ★★★ WHY THIS IS A SHARED HEADER AND NOT TWO WALKS. Two tiers ask about the
// same statement — the semantic analyzer (which refuses what cannot be bound)
// and `cst_to_hir` (which builds the descriptor from what survived). Before P5
// they each had their own walk, and `cst_to_hir`'s was deliberately WEAKER (a
// presence probe, not a decode). Two walks over one grammar is how the two
// answers drift apart: the analyzer would validate operand 3 and the lowering
// would build a descriptor with two. One gatherer, two callers.
//
// ⛔ NOT A TYPED VIEW. `docs/tree-model.md` records that the typed-view layer
// was DELETED in the 08.55 cleanup because role-POSITION helpers drift silently
// on grammar edits. Everything below is keyed on a config-resolved `RuleId` or
// `SchemaTokenId`, and the two places where document ORDER is unavoidable are
// guarded by a rule check that fails LOUD rather than shifting silently.

namespace dss {

namespace inline_asm_detail {

// Visible (non-EmptySpace) children of `parent`, in document order. File-local
// by the same convention `decl_prefix_strip.hpp` states for its own copy: that
// header owns only the decl-prefix discipline and tells non-declaration walks
// to keep their own enumerator.
[[nodiscard]] inline std::vector<NodeId>
visibleChildren(Tree const& tree, NodeId parent) {
    std::vector<NodeId> out;
    for (auto const& child : tree.children(parent)) {
        if (!isEmptySpace(tree.flags(child))) out.push_back(child);
    }
    return out;
}

// Descendants of `parent` in DOCUMENT order, pushed onto `stk` so that
// `stk.back()` pops the FIRST child (an explicit-stack pre-order DFS — the
// recursion-free idiom the analyzer uses everywhere).
inline void pushChildrenReversed(Tree const& tree, NodeId parent,
                                 std::vector<NodeId>& stk) {
    auto const kids = visibleChildren(tree, parent);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) stk.push_back(*it);
}

// Every Internal descendant of `root` whose rule is `want`, in document order,
// WITHOUT descending into a match (so nested occurrences inside a match — a
// string inside an operand's expression — are not collected).
//
// ★ A DFS BY RULE RATHER THAN A CHILD-INDEX WALK, because a `{repeat}` or an
// inline `{sequence}` in the grammar may or may not mint an intermediate node
// and the answer must not depend on which. `asmOperandList` is
// `asmOperand (, asmOperand)*` and `asmClobberList` is `str (, str)*`; both
// read correctly flat or nested.
[[nodiscard]] inline std::vector<NodeId>
descendantsOfRule(Tree const& tree, NodeId root, RuleId want) {
    std::vector<NodeId> out;
    if (!want.valid()) return out;
    std::vector<NodeId> stk;
    pushChildrenReversed(tree, root, stk);
    int guard = 0;
    while (!stk.empty() && ++guard <= 65536) {
        NodeId const n = stk.back();
        stk.pop_back();
        if (tree.kind(n) != NodeKind::Internal) continue;
        if (tree.rule(n).v == want.v) { out.push_back(n); continue; }
        pushChildrenReversed(tree, n, stk);
    }
    return out;
}

} // namespace inline_asm_detail

// ── ONE captured operand ────────────────────────────────────────────────────

// `[name] "constraint" ( expr )`, decomposed. Every NodeId here is a
// DIAGNOSTIC SPAN as well as a lowering input, which is why the constraint node
// is kept separately from the operand node: a refused constraint must underline
// the constraint, not the whole operand.
struct InlineAsmOperandFact {
    NodeId      operandNode{};      // the `asmOperand` node
    NodeId      constraintNode{};   // the constraint string-literal expression
    NodeId      valueExpr{};        // the host-language expression being bound
    NodeId      symbolicNameTok{};  // the `[name]` Identifier, invalid when absent
    std::string symbolicName;       // its text, empty when absent
    std::string constraint;         // DECODED constraint bytes
    bool        isOutput = false;   // written in the OUTPUTS section

    // The decomposition could not be completed — the operand node did not
    // present a constraint child of the configured shape, or presented no
    // distinct value expression. A CONFIG/GRAMMAR disagreement, never a source
    // error, and it must refuse rather than bind a guess.
    bool        malformed = false;
    // Why, for the message. Empty iff `!malformed`.
    std::string malformedDetail;

    // EVERY spelling this operand answers to in a template, minted from the
    // language's OWN declared lexemes by `mintTemplateSpellings`: the
    // POSITIONAL form always, and the SYMBOLIC form when the source named it.
    // Order is [positional, symbolic].
    //
    // ⚠ Empty ONLY when the language declares no `templatePlaceholder` at all
    // — i.e. it has no operand-reference surface, the same condition on which
    // `scanInlineAsmTemplate` returns without scanning. Never empty for a
    // language that can write `%0`.
    std::vector<std::string> spellings;
};

// One clobber-list entry, with the node its diagnostic underlines.
struct InlineAsmClobberFact {
    NodeId      node{};
    std::string text;
};

// One `asm goto` target label, as written.
struct InlineAsmLabelFact {
    NodeId      node{};
    std::string name;

    // EVERY spelling this label answers to in a template, minted by
    // `mintTemplateSpellings`: the BRACKETED form (it always has a name — a
    // label list names labels) and the POSITIONAL form on the reference
    // numbering. Order is [bracketed, positional].
    //
    // ⚠ Empty ⇔ the language declares `templateLabelPlaceholder` as `null`.
    // That is the HONOURED-ABSENCE case of the loader's four-way table, not a
    // hole: a language with no label sigil cannot spell a label reference at
    // all, and synthesizing one would invent a form nobody declared.
    std::vector<std::string> spellings;
};

// ── the statement ───────────────────────────────────────────────────────────

struct InlineAsmFacts {
    // ── section PRESENCE ──
    bool hasOutputs        = false;   // an operandList under the OUTPUTS tail
    bool hasInputs         = false;   // an operandList under an INPUTS tail
    bool hasClobbers       = false;   // a clobberList node
    bool hasLabelList      = false;   // a gotoLabelList PAYLOAD node
    // ★ The label SECTION is the TAIL node, which is a strictly weaker (and
    // therefore safer) fact than the label LIST: `asm("" ::::)` has the fourth
    // section with nothing in it, and that is precisely the shape all three
    // reference compilers reject without a `goto` qualifier. Keeping the two
    // apart is why each fused arm is its own named rule.
    bool hasLabelsSection  = false;
    bool hasGotoQualifier  = false;

    // ★★★ ANY tail rule at all — i.e. the statement carried at least one `:`,
    // which is what makes it the EXTENDED form. ✔MEASURED 2026-08-14 on gcc
    // 13.3.0 and clang 18.1.3: `:` alone, `:::` and the `volatile` spelling all
    // switch the template's lexing, so this is a LEXICAL discriminator and not
    // a summary of the four flags above (`asm("" :)` sets this and none of
    // them).
    bool isExtended        = false;

    // ★ FAIL-SAFE: set when the structural scan hit its iteration guard, i.e.
    // the subtree was not fully inspected. Treated as "payload present" by the
    // gate — an unfinished scan must never be read as a clean bare barrier.
    bool scanTruncated     = false;

    // ── the payload, CAPTURED ──
    // Outputs then inputs, each in source order. This concatenation IS the `%N`
    // index space (GNU 6.47.2.3) — `outputCount` splits it.
    std::vector<InlineAsmOperandFact> operands;
    std::uint32_t                     outputCount = 0;

    std::vector<InlineAsmClobberFact> clobbers;
    std::vector<InlineAsmLabelFact>   labels;
    std::string                       gotoQualifierText;   // as WRITTEN (`goto`)

    // ── nodes the diagnostics are positioned on ──
    NodeId templateNode{};        // located by RuleId — invalid ⇒ fail loud
    NodeId labelsSectionNode{};   // the offending tail, for the goto-check span
    NodeId dupQualifierTok{};     // the SECOND token of a repeated kind
    std::string dupQualifierText; // its SOURCE TEXT (`__volatile__`, as written)

    // The template's DECODED bytes. `nullopt` ⇒ the literal could not be
    // decoded at all (a malformed escape). ⚠ NOT the same as an empty string:
    // `decodeAdjacentStringBodies` returns "" — not nullopt — for a node with
    // no body token, so folding the two would make an unreadable template look
    // like an empty one and PASS the barrier check.
    std::optional<std::string> templateText;

    // Does this statement carry anything at all beyond a bare template?
    [[nodiscard]] bool hasPayload() const noexcept {
        return hasOutputs || hasInputs || hasClobbers || hasLabelsSection
               || hasLabelList || hasGotoQualifier || scanTruncated;
    }
    // Did the capture fail anywhere? The RESIDUAL predicate the extended-asm
    // refusal now keys on.
    [[nodiscard]] bool captureFailed() const noexcept {
        if (scanTruncated) return true;
        for (auto const& o : operands) {
            if (o.malformed) return true;
        }
        return false;
    }

    // ── WHAT A WRITTEN TEMPLATE FORM NAMES — A LOOKUP, NEVER A COMPUTATION ──
    //
    // ★★★ THE NUMBERING HAS ONE OWNER (`mintTemplateSpellings`) AND THESE TWO
    // ARE THE ONLY QUESTIONS ANY OTHER TIER MAY ASK ABOUT IT. ✔MEASURED
    // 2026-08-19 by the P20 independent audit: before they existed
    // `scanInlineAsmTemplate` re-derived `base = operandCount` in FIVE places,
    // and forcing the minting's base to 0 left that scan validating against the
    // OLD base — the divergence surfaced two tiers later, at the LIR label
    // binding. One fact, two owners, and a mutant that walks between them. A
    // lookup cannot drift from the list it looks in.
    //
    // ⚠ THE COMPARISON IS ON THE WHOLE FORM, EXACTLY AS THE LIR BINDING'S IS
    // (`mir_to_lir`: "ONE BINDING ROW PER SPELLING … the engine holds no `%N`
    // convention and only COMPARES"). That is what makes "the front end accepts
    // exactly what the lowering can bind" true BY CONSTRUCTION rather than by
    // two implementations agreeing about an index.
    [[nodiscard]] InlineAsmOperandFact const*
    operandAnsweringTo(std::string_view form) const noexcept {
        if (form.empty()) return nullptr;
        for (auto const& op : operands) {
            for (auto const& s : op.spellings) {
                if (s == form) return &op;
            }
        }
        return nullptr;
    }
    [[nodiscard]] InlineAsmLabelFact const*
    labelAnsweringTo(std::string_view form) const noexcept {
        if (form.empty()) return nullptr;
        for (auto const& l : labels) {
            for (auto const& s : l.spellings) {
                if (s == form) return &l;
            }
        }
        return nullptr;
    }

    // Every form the operands / the labels answer to, in minting order — the
    // inventory a refusal quotes back at the author.
    //
    // ★ AN INVENTORY RATHER THAN A RENDERED `first..last` RANGE, and the
    // difference is not cosmetic: a range has to KNOW where the label numbering
    // starts, so rendering one is a second statement of the numbering, while
    // this is the numbering itself read back. It also says MORE — the symbolic
    // forms are in it, and those are the ones an edited operand or label list
    // cannot silently renumber.
    //
    // ⚠ EMPTY IS NOT THE SAME QUESTION AS "the list is empty". A language whose
    // `templateLabelPlaceholder` is declared ABSENT has labels that answer to no
    // form at all, so a caller reporting on emptiness must say WHICH of the two
    // it found.
    [[nodiscard]] std::vector<std::string> mintedOperandForms() const {
        std::vector<std::string> out;
        for (auto const& op : operands) {
            out.insert(out.end(), op.spellings.begin(), op.spellings.end());
        }
        return out;
    }
    [[nodiscard]] std::vector<std::string> mintedLabelForms() const {
        std::vector<std::string> out;
        for (auto const& l : labels) {
            out.insert(out.end(), l.spellings.begin(), l.spellings.end());
        }
        return out;
    }
};

// ── the gatherer ────────────────────────────────────────────────────────────

namespace inline_asm_detail {

// Decompose ONE `asmOperand` node. Grammar (asm.lang.json `asmOperand`):
//
//     { optional [ '[' symbolName ']' ] }  templateText  '(' operandExpr ')'
//
// ★★ HOW THE VALUE EXPRESSION IS FOUND, AND WHY IT IS NOT A CHILD INDEX. The
// optional `[name]` prefix is three TOKEN roles, so it shifts every child index
// by three when present — which is exactly the ambiguity a positional locator
// walks into. What it does NOT shift is the sequence of INTERNAL children:
// there are two of those, the constraint and the expression, in that order.
// ⇒ the constraint is the FIRST internal child and its rule is CHECKED against
// `templateRule`; the expression is the LAST. The rule check is what makes the
// positional half safe rather than lucky: if a grammar edit ever mints a node
// for the prefix, that node lands FIRST and the check FAILS LOUD instead of
// silently promoting the prefix to a constraint.
// ★ Taking the LAST internal child rather than "the one that is not a
// templateRule" is deliberate: `"r"("abc")` is a legal operand whose VALUE is
// itself a string-literal expression, so a rule-difference test would find no
// candidate and refuse code gcc accepts.
[[nodiscard]] inline InlineAsmOperandFact
captureOperand(Tree const& tree, NodeId operandNode, InlineAsmConfig const& ia,
               SchemaTokenId identifierToken, SchemaTokenId bodyToken) {
    InlineAsmOperandFact f;
    f.operandNode = operandNode;

    std::vector<NodeId> internals;
    for (NodeId c : visibleChildren(tree, operandNode)) {
        if (tree.kind(c) == NodeKind::Internal) { internals.push_back(c); continue; }
        // The symbolic name is the ONLY Identifier token an operand can carry:
        // the other three token roles are brackets and parens. Keyed on the
        // token KIND the host bound to `symbolName`, never on position.
        if (identifierToken.valid() && !f.symbolicNameTok.valid()
            && tree.tokenKind(c).v == identifierToken.v) {
            f.symbolicNameTok = c;
            f.symbolicName    = tree.text(c);
        }
    }

    if (internals.size() < 2) {
        f.malformed       = true;
        f.malformedDetail =
            "the operand presents " + std::to_string(internals.size())
            + " composite child/children where the configured shape "
              "(semantics.inlineAsm.operandRule) requires two — a constraint "
              "and a value expression";
        return f;
    }
    if (!ia.templateRule.valid()
        || tree.rule(internals.front()).v != ia.templateRule.v) {
        f.malformed       = true;
        f.malformedDetail =
            "the operand's first composite child is not of the configured "
            "constraint shape '" + ia.templateRuleName
            + "' (semantics.inlineAsm.templateRule), so the constraint cannot "
              "be told apart from the value expression";
        return f;
    }

    f.constraintNode = internals.front();
    f.valueExpr      = internals.back();

    auto decoded = decodeAdjacentStringBodies(tree, f.constraintNode, bodyToken);
    if (!decoded) {
        f.malformed       = true;
        f.malformedDetail =
            "the operand's constraint string literal could not be decoded (a "
            "malformed escape), so the binding it names is unknown";
        return f;
    }
    f.constraint = std::move(*decoded);
    return f;
}

// ── the ONE minting site for every template spelling ────────────────────────
//
// ★★★ THE NUMBERING IS STATED HERE AND NOWHERE ELSE, AND THAT IS THE WHOLE
// POINT. Every tier below the front end used to be free to recompute "which
// `%N` names this operand" from whatever list it happened to hold; this
// function makes the question unaskable anywhere else by ANSWERING it once, on
// the SOURCE's own operand list, and carrying the answer.
//
// The rule:
//   * operand `k` — outputs first, then source inputs — is positional index `k`;
//   * label `j` is positional index `operandCount + j`, where `operandCount` is
//     `f.operands.size()`: the SOURCE list, in which a `"+"` (read-write)
//     operand appears exactly ONCE.
//
// ★★ THE SECOND CLAUSE IS WHY MINTING HAPPENS HERE AND NOT ONE TIER DOWN. A
// `"+"` operand is realized further along as an output PLUS a synthesized tied
// input, so a base computed from `outputs + inputs` at that tier OVERCOUNTS by
// the number of `"+"` operands and every label reference shifts. Minting on the
// source list deletes the second count outright: there is one list, and the two
// tiers cannot disagree because only one of them counts.
//
// ⛔ NOT ONE SIGIL BYTE IS A C++ LITERAL HERE. The operand sigil, the label
// sigil and the two name brackets are LANGUAGE vocabulary with a single
// declared owner (`semantics.inlineAsmTemplateLexemes`), and the four render
// helpers used below — `operandRef`, `namedOperandRef`, `labelRef`,
// `namedLabelRef`, all members of `InlineAsmTemplateLexemes` — are the same
// ones every diagnostic quotes. Spelling a sigil in this file would re-create
// the two-owner state
// `D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER` was closed
// for, one tier removed from where it was closed.
//
// ⚠ A ROLE DECLARED `null` COSTS A FORM AND NEVER INVENTS ONE. No
// `templatePlaceholder` ⇒ no operand spellings; no `templateLabelPlaceholder`
// ⇒ no label spellings; no bracket PAIR ⇒ no symbolic spelling (an opener alone
// can never close a name, which is the same both-halves rule the template scan
// applies).
inline void mintTemplateSpellings(InlineAsmFacts&                 f,
                                  InlineAsmTemplateLexemes const& lex) {
    bool const hasNamed = !lex.symbolicNameOpen.empty()
                          && !lex.symbolicNameClose.empty();

    if (!lex.placeholder.empty()) {
        for (std::size_t k = 0; k < f.operands.size(); ++k) {
            auto& op = f.operands[k];
            op.spellings.push_back(lex.operandRef(std::to_string(k)));
            if (hasNamed && !op.symbolicName.empty())
                op.spellings.push_back(lex.namedOperandRef(op.symbolicName));
        }
    }

    if (lex.labelPlaceholder.empty()) return;   // the honoured-absence case
    std::size_t const base = f.operands.size();
    for (std::size_t j = 0; j < f.labels.size(); ++j) {
        auto& l = f.labels[j];
        if (hasNamed && !l.name.empty())
            l.spellings.push_back(lex.namedLabelRef(l.name));
        l.spellings.push_back(lex.labelRef(std::to_string(base + j)));
    }
}

// ── ONE NAME SPACE, AND A REPEAT IN IT IS A REFUSAL ─────────────────────────
//
// ★★★ THIS SITS BESIDE THE MINTING BECAUSE THE MINTING IS WHAT MAKES A REPEAT
// DANGEROUS, AND THE CYCLE THAT ADDED THE MINTING IS THE CYCLE THAT OPENED THE
// DEFECT (D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND). Before
// `%[name]` bound at all, a repeated name was a failed build by accident — the
// form matched no binding and was refused by name. `mintTemplateSpellings` gives
// the SECOND occurrence the SAME spelling as the first, and every lookup below
// this tier — `operandAnsweringTo` here, `TemplateHost::bindingFor` and
// `resolveBranchTarget` at the LIR tier — is a FIRST-MATCH scan. So the second
// binding is not rejected, it is silently discarded.
//
// ✔MEASURED 2026-08-19 through the shipped CLI, at debug AND release:
// `__asm__("movl %[v], %[out]" : [out] "=r"(r), [v] "=r"(d) : [v] "r"(a))` with
// `a == 20` compiled rc=0 and the program returned 0 — `%[v]` bound the OUTPUT,
// because outputs are scanned first.
//
// ★★ THE NAME SPACE IS ONE, AND THAT IS MEASURED RATHER THAN ASSUMED. GNU keeps
// operands' `[name]` labels and the `asm goto` label list in a SINGLE per-
// statement space, so there are THREE collisions and not one. ✔MEASURED
// 2026-08-19, `-std=gnu17`, gcc 13.3.0 and clang 19.1.1, each shape probed
// separately:
//   * two operands sharing a name  — gcc "duplicate 'asm' operand name 'v'";
//                                    clang "duplicate use of asm operand name
//                                    \"v\"" + a note at the first use.
//   * two `asm goto` labels        — the SAME two messages, verbatim, with the
//                                    label's name in them.
//   * an operand AND a label       — the SAME two messages again.
// ⇒ NOT ONE reference accepts any of the three, which is what makes acceptance a
// defect under the bidirectional half of the reference rule as §A.3b bounds it
// ("the defect is accepting what NOT ONE reference accepts"). ✔MEASURED the
// same day, DSS accepted all three: the operand pair miscompiled as above, and
// the other two compiled and ran to the right answer — which is why the third
// shape needed the reference probe to be classified at all, and why probing the
// operand pair alone would have produced a narrower rule than the references'.
//
// ⛔ NO SIGIL IS A C++ LITERAL HERE EITHER. The forms the message quotes are
// built through `InlineAsmTemplateLexemes`' render helpers, the same ones
// `mintTemplateSpellings` and every refusal in `scanInlineAsmTemplate` use.
//
// ⚠ AN EMPTY NAME IS NOT A NAME. An operand with no `[name]` prefix carries "",
// and so does the synthetic malformed entry the gatherer pushes for an operand
// list it could not read; folding those together would report a "duplicate"
// on two anonymous operands, i.e. refuse the ordinary case.
//
// The sink takes the SECOND occurrence and the FIRST separately, because the
// first is attached as a `related` location — the structured form of the note
// clang prints, and the one thing a message about "used twice" cannot express
// on its own.
template <class Report>
inline void scanDuplicateSymbolicNames(InlineAsmFacts const&           f,
                                       InlineAsmTemplateLexemes const& L,
                                       Report const&                   report) {
    // One entry per NAMED thing, in the order `mintTemplateSpellings` numbers
    // them: operands first, then labels.
    // ★ `section` + `position` DESCRIBE THE SOURCE, THEY DO NOT RESTATE THE
    // NUMBERING. A message that said "operand 2" would be quoting the `%N` index
    // space, and this file's whole discipline is that the index space has ONE
    // owner (`mintTemplateSpellings`) and is only ever LOOKED UP. What the author
    // can count without knowing the numbering is the position inside the section
    // they wrote — the 2nd output, the 1st input — so that is what is said.
    struct Named {
        std::string_view name;
        NodeId           at;
        // "output operand" / "input operand" / "`asm goto` label".
        std::string      section;
        std::size_t      position;   // 0-based WITHIN that section
        bool             isLabel;
    };
    std::vector<Named> named;
    named.reserve(f.operands.size() + f.labels.size());
    for (std::size_t k = 0; k < f.operands.size(); ++k) {
        auto const& op = f.operands[k];
        if (op.symbolicName.empty()) continue;
        bool const out = k < f.outputCount;
        named.push_back({op.symbolicName,
                         op.symbolicNameTok.valid() ? op.symbolicNameTok
                                                    : op.operandNode,
                         out ? "output operand" : "input operand",
                         out ? k : k - f.outputCount,
                         false});
    }
    for (std::size_t j = 0; j < f.labels.size(); ++j) {
        auto const& l = f.labels[j];
        if (l.name.empty()) continue;
        named.push_back({l.name, l.node, "`asm goto` label", j, true});
    }

    auto const quote = [](std::string const& s) { return "`" + s + "`"; };
    // How the author refers to one occurrence: what it IS and where in the
    // section they wrote it, never a line number and never a `%N` index.
    auto const describe = [](Named const& n) {
        return n.section + " " + std::to_string(n.position);
    };
    // The template form the occurrence answers to, spelled by THIS language.
    auto const formOf = [&](Named const& n) {
        return n.isLabel ? L.namedLabelRef(n.name) : L.namedOperandRef(n.name);
    };

    for (std::size_t b = 0; b < named.size(); ++b) {
        for (std::size_t a = 0; a < b; ++a) {
            if (named[a].name != named[b].name) continue;
            std::string const first  = formOf(named[a]);
            std::string const second = formOf(named[b]);
            // ★ THE TWO COLLISION SHAPES SAY DIFFERENT THINGS, because the
            // CONSEQUENCE differs and only one of them is a wrong register. Two
            // occurrences of the SAME kind mint the SAME form and the lookup
            // silently keeps one; an operand and a label mint DIFFERENT forms
            // (the sigils differ), so nothing here is ambiguous — the refusal is
            // owed to the references, which keep one name space and reject it.
            std::string consequence =
                first == second
                    ? "Both would answer to " + quote(first)
                      + " in the template, and every spelling lookup below this "
                        "tier takes the FIRST match — so one of the two bindings "
                        "would be discarded in silence"
                    : "They answer to different template forms here ("
                      + quote(first) + " and " + quote(second)
                      + "), but GNU 6.47.2.3/6.47.2.7 keeps operand names and "
                        "`asm goto` label names in ONE name space per statement, "
                        "so the reference compilers reject the pair outright";
            report(DiagnosticCode::S_InlineAsmDuplicateSymbolicName,
                   named[b].at, named[a].at,
                   "duplicate inline-asm symbolic name "
                   + quote(std::string{named[b].name}) + " — this statement "
                     "already uses that name for " + describe(named[a])
                   + ", and uses it again for " + describe(named[b]) + ". "
                   + consequence
                   + ". ✔MEASURED 2026-08-19: gcc 13.3.0 (\"duplicate 'asm' "
                     "operand name\") and clang 19.1.1 (\"duplicate use of asm "
                     "operand name\", with a note at the first use) both reject "
                     "this. Give one of the two its own name "
                     "(D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND)");
            // One report per REPEAT, not per pair: a name used three times is
            // two repeats, and the author fixes them one at a time. Stop
            // comparing this occurrence once it has been reported against its
            // first partner, so the second and third do not both point at it.
            break;
        }
    }
}

// ── the template's `%` forms, JUDGED AGAINST WHAT WAS MINTED ────────────────
//
// ★★★ WHY THIS FUNCTION LIVES BESIDE THE MINTING AND NOT IN THE TIER THAT
// REPORTS ITS FINDINGS (moved 2026-08-19). It sat in `semantic_analyzer.cpp`'s
// anonymous namespace and independently re-derived `base = operandCount` in FIVE
// places — the label range it rendered, the two `%l<N>` bounds, the `%<N>`
// label-range test, and an INDEX MAPPING (`f.labels[idx - operandCount]`) used to
// name a label in a message. ✔MEASURED by the P20 independent audit: forcing
// `mintTemplateSpellings`' base to 0 did NOT move this scan — it kept validating
// against `operandCount`, and the two answers only diverged two tiers later at
// the LIR label binding. That is the definition of a second owner.
//
// The rewrite asks `InlineAsmFacts::operandAnsweringTo` / `labelAnsweringTo`
// instead, which are lookups in the minted lists; co-locating the two makes the
// one-owner property a property of this FILE rather than of a convention, and it
// is what lets a test mutate the minting and require this scan to follow (the
// derivation is LIVE, not two copies that happen to agree). It is a free
// template over its report sink, so it depends on the semantic analyzer for
// nothing — the same argument that moved `gatherInlineAsmFacts` here in P5.
//
// ★★★ BASIC AND EXTENDED TEMPLATES LEX DIFFERENTLY, AND THAT IS MEASURED, NOT
// assumed. ✔MEASURED 2026-08-14 and RE-MEASURED 2026-08-15 on gcc 13.3.0 and
// clang 18.1.3 (sources fed as base64 so no shell quoting could alter a byte):
//
//   basic    `__asm__("xorl %eax, %eax")`            → assembles, rc=0. `%` is LITERAL.
//   extended `__asm__("xorl %eax, %eax" ::: "eax")`  → gcc "operand number missing
//                                                      after %-letter";
//                                                      clang "invalid % escape".
//   basic    `__asm__("movl %0, %eax")`              → gcc EMITS `movl %0, %eax`
//                                                      verbatim; the ASSEMBLER then
//                                                      refuses: `Error: bad register
//                                                      name '%0'`. clang: `error:
//                                                      invalid register name`.
//   basic    `__asm__("xorl %%eax, %%eax")`          → emitted verbatim; assembler
//                                                      `Error: bad register name
//                                                      '%%eax'`. clang the same.
//
// ⇒ TWO consequences, and both are load-bearing here. (1) **Any colon makes it
// extended**, so `isExtended` — not "does it have operands" — selects the
// reading. (2) `S_InlineAsmPlaceholderInBasicTemplate`'s honesty clause, which
// its own docblock labelled INFERRED and demanded be measured before a consumer
// landed, is now **MEASURED**: the reference toolchain fails on `%0`/`%%` in a
// basic template too, one stage later. Refusing here is therefore NOT a
// divergence — it moves the refusal to the one place that knows what the
// statement declared. The third row above is the matched POSITIVE CONTROL that
// keeps this from being a claim about `%` in general.
//
// ⚠ THE SPAN IS THE TEMPLATE NODE, NOT THE OFFENDING BYTE, and the message
// carries the byte offset instead. The template's DECODED bytes have no
// column-accurate preimage in the source: escapes change length and adjacent
// literal concatenation crosses tokens. A span computed by adding the decoded
// offset to the literal's start would be confidently wrong, which is worse than
// a whole-literal span plus an exact offset.
//
// ★★★ EVERY SIGIL BELOW COMES FROM `semantics.inlineAsmTemplateLexemes` AND NOT
// ONE OF THEM IS A C++ LITERAL (2026-08-17,
// D-SEMANTIC-ASM-TEMPLATE-SIGILS-HARDCODED-BESIDE-A-CONFIG-OWNER, CLOSED). This
// function used to compare against `'%'`, `'l'`, `'['` and `']'` while every
// assembly DIALECT declared the same spellings in config — one fact with two
// owners, and nothing in the shipped build could detect them disagreeing,
// because this tier has NO dialect in scope when it scans (the dialect is
// resolved at MIR→LIR; `TargetSchema` carries only its stem name; and `target`
// is legitimately `nullptr` for the LSP, the FFI header parser and every
// direct-API caller — all of which still scan templates). The sigils are
// HOST-LANGUAGE vocabulary — gcc substitutes them before the assembler ever sees
// the text — so they live beside `memoryClobber`/`conditionCodeClobber`, and the
// language config is the one thing this tier is guaranteed to have.
//
// ★★ THE MESSAGES ARE DERIVED TOO, NOT JUST THE COMPARISONS. A refusal that
// quotes a hard-coded `%0` at a language spelling it `$0` sends the reader to
// fix the wrong byte, which is the same defect one tier over. `L.operandRef` and
// its three siblings build every quoted form from the declared lexemes, and the
// INVENTORIES a refusal lists are the minted spellings themselves.
//
// ★ FORMS ARE SEPARATED BY MAXIMAL MUNCH — longest declared lexeme first —
// which is exactly how `longestMatchInMode` separates the same three in the
// tokenizer, so the two tiers cannot disagree about where a form starts. The
// loader REFUSES an escape or label lexeme that is not longer than the
// placeholder, so the ordering below can never be ambiguous.
//
// ★★ AND NO NUMBER IS EVER COMPUTED FROM THE TEMPLATE. An index is carried as
// the DIGIT BYTES the author wrote and pasted straight back through
// `L.operandRef`/`L.labelRef`, so the form looked up is the form written. Two
// things fall out, both of them correctness rather than tidiness: a monstrous
// index can no longer WRAP a `std::size_t` accumulator into an in-range one and
// be accepted, and a leading-zero spelling is refused HERE — by the tier that
// can say why — instead of clearing this gate and failing at the LIR binding,
// which matches on the exact spelling and would never find `%01`.
//
// ⓘ THE WIDTH-VIEW LETTER IS NOT A ROLE OF THIS SET AND IS NOT CHECKED HERE,
// and that is a boundary rather than an exception. A letter is per-DIALECT
// vocabulary (`assembly.templateModifiers`) — ✔MEASURED 2026-08-24 on gcc 13.3.0
// both ports that `%w` is 32 bits on aarch64 and 16 on x86-64 — while this tier
// has no dialect and often no target in scope at all. So this scan validates
// what a `%<letter><selector>` form NAMES, on the minted inventory, and leaves
// the letter to the tier where the dialect is resolved; an undeclared letter
// mints no lexeme and is refused by name at MIR→LIR. ⚠ The scan used to REFUSE
// every such form (`S_InlineAsmOperandModifierUnsupported`), which was correct
// while nothing declared the vocabulary and is a divergence now that something
// does — see the width-view arm below.
template <class Report>
inline void scanInlineAsmTemplate(InlineAsmFacts const&           f,
                                  InlineAsmTemplateLexemes const& L,
                                  std::string_view                text,
                                  Report const&                   report) {
    NodeId const at = f.templateNode;
    // No placeholder role ⇒ this language declares no operand-reference surface
    // at all, and there is nothing to scan FOR. Refusing to guess a sigil is the
    // point: the alternative is scanning for a byte nobody declared.
    if (L.placeholder.empty()) return;

    auto const isDigit = [](char c) { return c >= '0' && c <= '9'; };
    auto const isAlpha = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    auto const at_ = [&](std::size_t i, std::string_view lex) {
        return !lex.empty() && i + lex.size() <= text.size()
            && text.compare(i, lex.size(), lex) == 0;
    };
    // The DIGIT BYTES following a sigil, exactly as written, advancing `i` past
    // them. Never a parsed number — see the docblock: the bytes are what the
    // minted spellings are made of, so the bytes are what gets looked up.
    auto const readIndexDigits = [&](std::size_t& i) {
        std::size_t const start = i;
        while (i < text.size() && isDigit(text[i])) ++i;
        return std::string{text.substr(start, i - start)};
    };
    // `<open>name<close>` — returns the name and advances `i` past the closing
    // lexeme, or nullopt when it never closes (a template defect, not a name).
    auto const readBracketName =
        [&](std::size_t& i) -> std::optional<std::string> {
        if (L.symbolicNameClose.empty()) return std::nullopt;
        std::size_t const close = text.find(L.symbolicNameClose, i);
        if (close == std::string_view::npos) return std::nullopt;
        std::string name{text.substr(i, close - i)};
        i = close + L.symbolicNameClose.size();
        return name;
    };
    // The spelling of ONE form, quoted back at the author exactly as this
    // language spells it.
    auto const quote = [](std::string const& s) { return "`" + s + "`"; };
    // ★ A CLAUSE THAT DISAPPEARS WHEN ITS ROLE DOES. A role may be declared
    // ABSENT (an explicit `null`), and a message that then advised the author to
    // "write `` for a literal percent" would be quoting a lexeme no language
    // spells — the empty-backtick shape a hard-coded sigil produces one step
    // further along. Every optional clause below is built through this, so an
    // absent role costs the reader a sentence rather than handing them one that
    // is wrong.
    auto const clause = [&](std::string const& lexeme, std::string_view before,
                            std::string_view after) {
        return lexeme.empty() ? std::string{}
                              : std::string{before} + quote(lexeme)
                                    + std::string{after};
    };
    // Is the bracketed `<open>name<close>` form spellable at all in this
    // language? Both halves are needed; one alone can never close a name.
    bool const hasNamed =
        !L.symbolicNameOpen.empty() && !L.symbolicNameClose.empty();

    // ── the INVENTORIES every refusal quotes ────────────────────────────────
    //
    // ★★★ THE MESSAGES NAME THE FORMS THAT EXIST, NOT A RANGE RECOMPUTED FROM
    // THE COUNTS. GNU 6.47.2.7 numbers the `asm goto` labels AFTER every
    // operand, so the two sigils share ONE index space and every refusal needs
    // to tell the author which half the index they wrote falls in — but the
    // place that KNOWS is `mintTemplateSpellings`, and rendering `%l2..%l4` here
    // would be this file stating the numbering a second time. Listing the minted
    // spellings says strictly more (the symbolic forms are in it) and cannot
    // disagree with what the lowering binds.
    //
    // ⚠ THE COUNTS BELOW ARE DESCRIPTIVE AND NOTHING ELSE. "this statement
    // declares 2 operand(s)" is a fact about the statement; it is never
    // arithmetic on the way to deciding what an index names.
    std::string const operandCountText = std::to_string(f.operands.size());
    std::string const labelCountText   = std::to_string(f.labels.size());
    auto const inventory = [&](std::vector<std::string> const& forms) {
        std::string out;
        for (std::size_t k = 0; k < forms.size(); ++k) {
            if (k != 0) out += ", ";
            out += quote(forms[k]);
        }
        return out;
    };
    std::vector<std::string> const operandForms = f.mintedOperandForms();
    std::vector<std::string> const labelForms   = f.mintedLabelForms();
    // ⚠ LABELS THAT ANSWER TO NOTHING ARE NOT THE SAME AS NO LABELS, and saying
    // "none" for both would tell an author who wrote a label list that they did
    // not. The honoured-absence case (`templateLabelPlaceholder` declared null)
    // is named as what it is: the LANGUAGE has no label-reference form.
    bool const labelsUnspellable = !f.labels.empty() && labelForms.empty();
    std::string const operandInventory =
        operandForms.empty()
            ? std::string{"(none — this statement declares no operands)"}
            : inventory(operandForms);
    std::string const labelInventory =
        !labelForms.empty()
            ? inventory(labelForms)
            : (labelsUnspellable
                   ? std::string{"(none — this language declares no "
                                 "label-reference form, "
                                 "semantics.inlineAsmTemplateLexemes."
                                 "templateLabelPlaceholder is absent)"}
                   : std::string{"(none — this statement declares no `asm goto` "
                                 "labels)"});

    for (std::size_t i = 0; i < text.size();) {
        // Longest-first: the escape and the label sigil both EXTEND the
        // placeholder, and the loader guarantees they are strictly longer.
        bool const atEscape = at_(i, L.escape);
        bool const atLabel  = at_(i, L.labelPlaceholder);
        bool const atPlain  = at_(i, L.placeholder);
        if (!atEscape && !atLabel && !atPlain) { ++i; continue; }
        std::size_t const formStart = i;
        // The byte that FOLLOWS a plain placeholder — what selects the form.
        std::size_t const afterSigil = formStart + L.placeholder.size();
        if (!atEscape && !atLabel && afterSigil >= text.size()) {
            // A trailing bare placeholder. In BASIC it is one literal character
            // and the reference toolchain assembles it; in EXTENDED both
            // reference compilers error.
            if (!f.isExtended) break;
            report(DiagnosticCode::S_InlineAsmOperandModifierUnsupported, at,
                   "the inline-asm template ends with a bare "
                   + quote(L.placeholder) + " at byte offset "
                   + std::to_string(formStart)
                   + " — in an EXTENDED template (any `:` makes it extended) "
                   + quote(L.placeholder)
                   + " introduces an operand reference, so a trailing one names "
                     "nothing. gcc and clang reject this form too"
                   + clause(L.escape, "; write ", " for a literal percent"));
            break;
        }
        char const c = afterSigil < text.size() ? text[afterSigil] : '\0';

        // ── the BASIC reading: the placeholder is LITERAL, with exactly two
        //    exceptions ──
        if (!f.isExtended) {
            bool const opensName = at_(afterSigil, L.symbolicNameOpen);
            if (!atEscape && !isDigit(c) && !opensName) { ++i; continue; }
            std::string const form =
                atEscape ? L.escape
                         : (opensName ? L.placeholder + L.symbolicNameOpen
                                      : L.placeholder + std::string{c});
            report(DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate, at,
                   "the inline-asm template contains " + quote(form)
                   + " at byte offset " + std::to_string(formStart)
                   + ", but this statement has NO operand sections at all, so it "
                     "is a BASIC template in which " + quote(L.placeholder)
                   + " is LITERAL — the text is "
                     "emitted verbatim and nothing is substituted. ✔MEASURED on "
                     "gcc 13.3.0 and clang 18.1.3: the reference toolchain emits "
                     "it unchanged and the ASSEMBLER then rejects it (\"bad "
                     "register name\"), so this program does not build there "
                     "either. Add the operand sections this template expects "
                     "(even `::` alone makes it extended)"
                   + clause(L.escape, ", or write a literal percent as ",
                            " in an extended template"));
            i = formStart + form.size();
            continue;
        }

        // ── the EXTENDED reading ──
        // The escape. Consumed WHOLE — nothing inside it is re-read.
        // ⚠ THIS ADVANCE HAD AN OFF-BY-ONE AND IT WAS NOT COSMETIC: advancing
        // past the pair skipped the byte AFTER it, so `%%%0` lost its
        // placeholder — a literal percent followed by operand 0 read as a
        // literal percent followed by nothing. That is a MISSED refusal, i.e.
        // the silent direction. Advancing by the lexeme's own length is what
        // makes the bug unwritable rather than fixed.
        if (atEscape) { i = formStart + L.escape.size(); continue; }

        // The label reference. GNU 6.47.2.7 numbers the labels AFTER every
        // operand, so the label index space is a CONTINUATION of the operand
        // one, not a second one starting at zero — which is exactly why NOTHING
        // here recomputes where it starts.
        std::size_t const afterLabel = formStart + L.labelPlaceholder.size();
        if (atLabel && afterLabel < text.size()
            && (isDigit(text[afterLabel])
                || (hasNamed && at_(afterLabel, L.symbolicNameOpen)))) {
            i = afterLabel;
            // ⚠ BOTH HALVES OF THE PAIR, OR THE FORM IS NOT SPELLABLE. A
            // language that declares an opener and no closer cannot write a
            // symbolic name at all, so recognizing the opener would make every
            // such reference report "no closing ``" — a refusal quoting a lexeme
            // nobody declared. It falls through to the generic refusal instead.
            if (hasNamed && at_(i, L.symbolicNameOpen)) {
                i += L.symbolicNameOpen.size();
                auto const name = readBracketName(i);
                if (!name) {
                    report(DiagnosticCode::S_InlineAsmTemplateUnparsable, at,
                           "the inline-asm template has an unterminated "
                           + quote(L.labelPlaceholder + L.symbolicNameOpen)
                           + " label reference at byte offset "
                           + std::to_string(formStart) + " — no closing "
                           + quote(L.symbolicNameClose));
                    continue;
                }
                std::string const written = L.namedLabelRef(*name);
                if (f.labelAnsweringTo(written) != nullptr) continue;
                // ★ THE NAMED MIRROR OF THE POSITIONAL ARM BELOW. A name that
                // answers as an OPERAND is the SIGIL being wrong, not the name —
                // the same mistake `%l<N>` makes when its index names an operand
                // — and the remedy is different from "that label does not
                // exist", so the two must not share a sentence.
                std::string const asOperand = L.namedOperandRef(*name);
                bool const namesAnOperand =
                    f.operandAnsweringTo(asOperand) != nullptr;
                report(DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, at,
                       "the inline-asm template references label "
                       + quote(written)
                       + " at byte offset " + std::to_string(formStart)
                       + (namesAnOperand
                              ? ", but that name belongs to an OPERAND of this "
                                "statement rather than to one of its "
                                    + labelCountText
                                    + " `asm goto` label(s) — write "
                                    + quote(asOperand)
                                    + " to name the operand"
                              : ", but this statement declares " + labelCountText
                                    + " `asm goto` label(s) and the label forms "
                                      "it declares are "
                                    + labelInventory));
                continue;
            }
            // ── the POSITIONAL form `%l<N>` — VALIDATED ON THE MINTED FORMS ──
            //
            // ★★★ THIS BRANCH USED TO REFUSE THE FORM OUTRIGHT, AND THE REFUSAL
            // WAS THE DIVERGENCE. Both reference compilers accept `%l<N>`, and
            // under §A.3b of the bar one working reference makes the behaviour
            // REQUIRED — so the question this branch may ask is only whether the
            // form NAMES a label, never whether the spelling exists. The grammar
            // half (`asmTemplateLabelRef` admitting the index form) and the
            // lowering half (a label binding that carries a BLOCK rather than a
            // register) landed with it, so there is no longer a tier that cannot
            // represent what this one accepts.
            //
            // ★★ AND IT ASKS BY LOOKUP, WHICH IS THE WHOLE CORRECTNESS QUESTION.
            // The list the numbering runs over is the SOURCE's, in which a `"+"`
            // read-write operand appears ONCE; tiers further down realize that
            // one operand as an output PLUS a synthesized tied input, so a base
            // computed anywhere else is larger by the number of `"+"` operands
            // and every label reference shifts — silently, onto the wrong edge.
            // `mintTemplateSpellings` answered that on the source list; this
            // asks it rather than answering it again.
            //
            // ⚠ TWO WAYS TO BE WRONG, AND THEY ARE DIFFERENT MISTAKES, so they
            // get different messages. Both are the same DSS code — the form does
            // not name what the sigil requires — because the remedy is the same
            // shape: write the form the intended target actually answers to.
            // ✔MEASURED, each arm on BOTH references, `-std=gnu17`: an index
            // landing among the OPERANDS is gcc's "'%l' operand isn't a label"
            // (P20 reference probe), and an index past the last label is gcc
            // 13.3.0's "invalid 'asm': operand number out of range" and clang
            // 19.1.1's "invalid operand number in inline asm string"
            // (2026-08-19). ⚠ The second row was INFERRED until that date while
            // the message already asserted it to users as fact — the failure
            // mode is stated here because the message reads as evidence.
            //
            // ⓘ A label that is LISTED AND NEVER REFERENCED is legal and silent
            // on both references, so nothing here (or anywhere in this scan)
            // walks the label list looking for unused entries.
            std::string const digits  = readIndexDigits(i);
            std::string const written = L.labelRef(digits);
            if (f.labelAnsweringTo(written) != nullptr) continue;
            std::string const asOperand = L.operandRef(digits);
            if (f.operandAnsweringTo(asOperand) != nullptr) {
                report(DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, at,
                       "the inline-asm template references " + quote(written)
                       + " at byte offset " + std::to_string(formStart)
                       + " — the " + quote(L.labelPlaceholder)
                       + " form names an `asm goto` LABEL — but index " + digits
                       + " names an OPERAND. GNU 6.47.2.7 numbers the labels "
                         "AFTER every operand, so with " + operandCountText
                       + " operand(s) and " + labelCountText
                       + " label(s) the operand forms this statement declares "
                         "are " + operandInventory + " and the label forms are "
                       + labelInventory + ". Write " + quote(asOperand)
                       + " to name the operand. gcc rejects this form too");
            } else {
                report(DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, at,
                       "the inline-asm template references `asm goto` label "
                       + quote(written) + " at byte offset "
                       + std::to_string(formStart)
                       + ", but no label of this statement answers to that form. "
                         "This statement declares " + labelCountText
                       + " label(s), and GNU 6.47.2.7 numbers them AFTER every "
                         "operand, so with " + operandCountText
                       + " operand(s) the label forms it declares are "
                       + labelInventory
                       + (hasNamed
                              ? ". The bracketed form is the one an edited label "
                                "list cannot silently renumber"
                              : std::string{})
                       + ". ✔MEASURED 2026-08-19 with 1 input and 1 label (so "
                         "the only valid label index is 1) and a template "
                         "writing an index of 3: gcc 13.3.0 rejects it, "
                         "\"invalid 'asm': operand number out of range\", and "
                         "clang 19.1.1 rejects it, \"invalid operand number in "
                         "inline asm string\"");
            }
            continue;
        }

        // `<placeholder><N>` — an operand by index.
        if (isDigit(c)) {
            i = afterSigil;
            std::string const digits  = readIndexDigits(i);
            std::string const written = L.operandRef(digits);
            if (f.operandAnsweringTo(written) != nullptr) continue;
            // ★★ AN INDEX PAST THE OPERANDS IS NOT ALWAYS A TYPO — ON AN
            // `asm goto` IT IS USUALLY THE WRONG SIGIL. The labels continue this
            // very numbering (GNU 6.47.2.7), so `%2` on a two-operand-one-label
            // statement is the author naming their label with the OPERAND form.
            // Telling them "you declared 2 operands" is true and useless; naming
            // the label form is the half they can act on. The code stays the
            // same — the form still does not name an operand — because the
            // reader routes on the code and reads the message.
            InlineAsmLabelFact const* const asLabel =
                f.labelAnsweringTo(L.labelRef(digits));
            std::string tail;
            if (asLabel != nullptr) {
                tail = ". Index " + digits
                     + " names an `asm goto` LABEL, not an operand: GNU 6.47.2.7 "
                       "numbers the labels AFTER every operand, so write one of "
                       "the forms that label answers to — "
                     + inventory(asLabel->spellings) + " — to branch to it";
            } else if (labelsUnspellable) {
                // This statement HAS labels and this language declares no
                // label-reference form at all, so no index names one. Worth
                // stating — an author staring at a label list needs to know why
                // nothing reaches it — but there is no spelling to recommend and
                // inventing one would quote a lexeme nobody declares.
                tail = ". This statement declares " + labelCountText
                     + " `asm goto` label(s), but this language declares no "
                       "label-reference form "
                       "(semantics.inlineAsmTemplateLexemes."
                       "templateLabelPlaceholder is absent), so no form names "
                       "one";
            } else {
                tail = ". The index space is the OUTPUTS-THEN-INPUTS "
                       "concatenation (GNU 6.47.2.3), so "
                     + quote(L.operandRef("0"))
                     + " is the first OUTPUT when one exists and the first INPUT "
                       "when it does not — an off-by-one here is usually a "
                       "section edited without renumbering";
            }
            report(DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, at,
                   "the inline-asm template references operand " + quote(written)
                   + " at byte offset " + std::to_string(formStart)
                   + ", but this statement declares " + operandCountText
                   + " operand(s)"
                   + (operandForms.empty()
                          ? std::string{}
                          : " (the operand forms it declares are "
                                + operandInventory + ")")
                   + tail);
            continue;
        }

        // `<placeholder><open>name<close>` — an operand by its symbolic name.
        // Both halves of the pair, or the form is not spellable (see the label
        // branch above for why recognizing a lone opener is worse than not).
        if (hasNamed && at_(afterSigil, L.symbolicNameOpen)) {
            i = afterSigil + L.symbolicNameOpen.size();
            auto const name = readBracketName(i);
            if (!name) {
                report(DiagnosticCode::S_InlineAsmTemplateUnparsable, at,
                       "the inline-asm template has an unterminated "
                       + quote(L.placeholder + L.symbolicNameOpen)
                       + " operand reference at byte offset "
                       + std::to_string(formStart)
                       + " — no closing " + quote(L.symbolicNameClose));
                continue;
            }
            std::string const written = L.namedOperandRef(*name);
            if (f.operandAnsweringTo(written) != nullptr) continue;
            report(DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, at,
                   "the inline-asm template references operand " + quote(written)
                   + " at byte offset " + std::to_string(formStart)
                   + ", but no operand of this statement answers to that form — "
                     "the operand forms it declares are " + operandInventory
                   + ". An operand is given a symbolic name by writing it "
                     "(GNU 6.47.2.3 "
                   + quote(L.symbolicNameOpen + "name" + L.symbolicNameClose
                           + " \"constraint\" (expr)")
                   + ")");
            continue;
        }

        // ── `%<letter><selector>` — AN OPERAND THROUGH A **WIDTH VIEW** ──
        //
        // ★★★ THIS ARM VALIDATES THE **SELECTOR** AND SAYS NOTHING ABOUT THE
        // LETTER, AND THE SPLIT IS FORCED BY WHAT THIS TIER CAN SEE. `%w0` and
        // `%0` name the SAME operand — a letter selects which VIEW of the bound
        // register is written (`w0` versus `x0`), never a different operand — so
        // "does this reference name something" is answerable here, on the minted
        // inventory, exactly as it is for the plain form. "Is `w` a letter this
        // dialect spells" is NOT: the letters are per-DIALECT vocabulary
        // (`assembly.templateModifiers`), ✔MEASURED 2026-08-24 on gcc 13.3.0
        // both ports to differ between them — `%w` is 32 bits on aarch64 and 16
        // on x86-64 — and this tier legitimately has NO dialect and often no
        // target at all (the LSP, the FFI header parser and every direct-API
        // caller scan templates with `target == nullptr`). A letter check
        // written here would be a check that silently does not run.
        //
        // ★★ THE LETTER IS ANSWERED WHERE THE DIALECT IS RESOLVED, AND LOUDLY.
        // An undeclared letter mints no template lexeme, so `%z0` reaches no
        // shape and the dialect refuses the whole template by name at MIR→LIR
        // (`S_InlineAsmTemplateUnparsable`, which carries the inner
        // diagnostic) — the same tier and the same code that already answer "is
        // `frobnicate` a mnemonic" and "is `%r99` a register". ⇒ one owner per
        // question, and nothing accepts a letter that no tier ever checks.
        //
        // ⚠ THE LETTERS ARE READ AS A RUN RATHER THAN AS ONE CHARACTER, so a
        // dialect declaring a multi-byte view spelling is validated by the same
        // code. A run that is followed by no selector at all (`%eax` — the
        // classic un-escaped register in an extended template) falls through to
        // the generic refusal below, exactly as before.
        if (isAlpha(c)) {
            std::size_t afterLetters = afterSigil;
            while (afterLetters < text.size() && isAlpha(text[afterLetters])) {
                ++afterLetters;
            }
            std::string const letters{
                text.substr(afterSigil, afterLetters - afterSigil)};
            bool const viewOpensName =
                hasNamed && at_(afterLetters, L.symbolicNameOpen);
            bool const viewHasIndex =
                afterLetters < text.size() && isDigit(text[afterLetters]);
            if (viewHasIndex || viewOpensName) {
                // The form the author WROTE, and the PLAIN form it is a view of.
                // ⛔ Both are built through the language's render helpers, so no
                // sigil is a C++ literal here either.
                std::string written;
                std::string plain;
                if (viewOpensName) {
                    i = afterLetters + L.symbolicNameOpen.size();
                    auto const name = readBracketName(i);
                    if (!name) {
                        report(DiagnosticCode::S_InlineAsmTemplateUnparsable, at,
                               "the inline-asm template has an unterminated "
                               + quote(L.placeholder + letters
                                       + L.symbolicNameOpen)
                               + " operand reference at byte offset "
                               + std::to_string(formStart)
                               + " — no closing " + quote(L.symbolicNameClose));
                        continue;
                    }
                    plain   = L.namedOperandRef(*name);
                    written = L.placeholder + letters + L.symbolicNameOpen
                            + *name + L.symbolicNameClose;
                } else {
                    i = afterLetters;
                    std::string const digits = readIndexDigits(i);
                    plain   = L.operandRef(digits);
                    written = L.placeholder + letters + digits;
                }
                if (f.operandAnsweringTo(plain) != nullptr) continue;
                // ★ THE MESSAGE NAMES BOTH FORMS. The author wrote the view; the
                // thing that has to be bound is the operand the view is OF, and
                // an author told only about `%0` when they wrote `%w0` cannot
                // see which half of their spelling was wrong.
                InlineAsmLabelFact const* const asLabel =
                    viewHasIndex ? f.labelAnsweringTo(
                                       L.labelRef(plain.substr(
                                           L.placeholder.size())))
                                 : nullptr;
                report(DiagnosticCode::S_InlineAsmPlaceholderOutOfRange, at,
                       "the inline-asm template references " + quote(written)
                       + " at byte offset " + std::to_string(formStart)
                       + " — a width view names the SAME operand as "
                       + quote(plain)
                       + " and only states how wide a view of it to write — but "
                         "no operand of this statement answers to " + quote(plain)
                       + ". This statement declares " + operandCountText
                       + " operand(s)"
                       + (operandForms.empty()
                              ? std::string{}
                              : " (the operand forms it declares are "
                                    + operandInventory + ")")
                       + (asLabel != nullptr
                              ? ". That index names an `asm goto` LABEL, not an "
                                "operand: GNU 6.47.2.7 numbers the labels AFTER "
                                "every operand, and a label has no register and "
                                "therefore no width to view — write "
                                    + inventory(asLabel->spellings)
                                    + " to branch to it"
                              : std::string{}));
                continue;
            }
        }

        // Every other placeholder form in an EXTENDED template. gcc says
        // "operand number missing after %-letter" and clang "invalid % escape" —
        // so refusing matches the reference rather than being stricter than it.
        // GNU's per-instance unique number (`%=`) lands here too: it is a real
        // GNU feature this build does not expand, and emitting it verbatim would
        // hand the assembler a form it reads as a register spelling.
        report(DiagnosticCode::S_InlineAsmOperandModifierUnsupported, at,
               "the inline-asm template contains "
               + quote(L.placeholder + std::string{c})
               + " at byte offset " + std::to_string(formStart)
               + ", which is not an operand reference this build can expand. In "
                 "an EXTENDED template (any `:` makes it extended) "
               + quote(L.placeholder) + " introduces an operand — "
               + quote(L.operandRef("0"))
               + (hasNamed ? ", " + quote(L.namedOperandRef("name"))
                           : std::string{})
               + (L.labelPlaceholder.empty()
                      ? std::string{}
                      : ", " + quote(L.labelRef("0"))
                            + " for an `asm goto` label")
               + clause(L.escape, " — and ", " is the literal percent")
               + ". gcc and clang reject an unrecognised placeholder form here "
                 "too");
        i = afterSigil + 1;
    }
}

} // namespace inline_asm_detail

// Gather every inline-asm fact for `asmNode`.
//
// ★★ `lex` IS REQUIRED, NOT DEFAULTED, AND THE ABSENCE OF A DEFAULT IS THE
// GUARD. The spellings minted from it are what every tier below compares
// against, so a caller that could omit them would silently produce a statement
// whose operands answer to no `%N` at all — an asm that binds nothing, with a
// clean build log. A required parameter makes "facts without spellings"
// unwritable instead of merely discouraged.
[[nodiscard]] inline InlineAsmFacts
gatherInlineAsmFacts(Tree const& tree, NodeId asmNode, InlineAsmConfig const& ia,
                     InlineAsmTemplateLexemes const& lex,
                     SchemaTokenId identifierToken, SchemaTokenId bodyToken) {
    using namespace inline_asm_detail;
    InlineAsmFacts f;

    // ── (0) the TEMPLATE, located by RuleId — NEVER positionally ──
    // ★ THIS WAS A LATENT SILENT MISCOMPILE. The pre-P1 locator took the FIRST
    // Internal child, which was sound only while `asmStmt` could carry nothing
    // but a template. With a tail present it is "first of ≥2", and
    // `decodeAdjacentStringBodies` returns "" (NOT nullopt) for a node with no
    // body token — so a mis-picked node decodes to the empty string and the
    // empty-template check PASSES on an asm that is anything but empty.
    if (ia.templateRule.valid()) {
        for (NodeId c : visibleChildren(tree, asmNode)) {
            if (tree.kind(c) != NodeKind::Internal) continue;
            if (tree.rule(c).v != ia.templateRule.v) continue;
            f.templateNode = c;
            break;
        }
    }
    if (f.templateNode.valid()) {
        f.templateText = decodeAdjacentStringBodies(tree, f.templateNode, bodyToken);
    }

    // ── (1) the STRUCTURAL scan: sections + payload, UNPRUNED at tails ──
    // Tails NEST (`outputsTail` contains `inputsTail` contains …), so pruning at
    // the first tail would hide every later section — the label section most of
    // all. Each frame carries its NEAREST ENCLOSING TAIL, which is what gives an
    // `operandList` its ROLE: the same list shape is OUTPUTS under the outputs
    // tail and INPUTS under either inputs tail. Payload lists ARE pruned: they
    // provably contain no tail and no other list (they are the leaf sections of
    // the asm grammar), so descending into them would only walk user expressions.
    std::vector<InlineAsmOperandFact> outputOps;
    std::vector<InlineAsmOperandFact> inputOps;
    struct Frame { NodeId node; std::uint32_t tail; };
    std::vector<Frame> stack;
    {
        auto const kids = visibleChildren(tree, asmNode);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back({*it, 0u});
    }
    int guard = 0;
    while (!stack.empty()) {
        if (++guard > 65536) { f.scanTruncated = true; break; }
        Frame const cur = stack.back();
        stack.pop_back();
        if (tree.kind(cur.node) != NodeKind::Internal) continue;
        RuleId const r = tree.rule(cur.node);
        std::uint32_t tail = cur.tail;
        if (ia.isTailRule(r)) {
            tail          = r.v;
            f.isExtended  = true;   // ANY colon — the lexical discriminator
            if ((ia.labelsTailRule.valid()      && r.v == ia.labelsTailRule.v)
                || (ia.labelsTailFusedRule.valid()
                    && r.v == ia.labelsTailFusedRule.v)) {
                f.hasLabelsSection = true;
                if (!f.labelsSectionNode.valid()) f.labelsSectionNode = cur.node;
            }
        } else if (ia.operandListRule.valid() && r.v == ia.operandListRule.v) {
            bool const isOutputs = ia.outputsTailRule.valid()
                                   && tail == ia.outputsTailRule.v;
            // ★ NOT AN `else if (isInputs)`. An operand list whose enclosing
            // tail is neither (a grammar shape this build does not know) is
            // still OPERANDS — real payload that must be captured and refused.
            // Attributing it to inputs over-reports a role in a message;
            // DROPPING it would under-report PAYLOAD, and only one of those is
            // a miscompile.
            auto& sink = isOutputs ? outputOps : inputOps;
            if (isOutputs) f.hasOutputs = true; else f.hasInputs = true;
            for (NodeId opNode :
                 descendantsOfRule(tree, cur.node, ia.operandRule)) {
                auto op     = captureOperand(tree, opNode, ia, identifierToken,
                                             bodyToken);
                op.isOutput = isOutputs;
                sink.push_back(std::move(op));
            }
            // A list that yielded NO operand nodes is a config/grammar
            // disagreement of the same class `captureOperand` reports, and it
            // must not read as "an empty section": the section's presence is
            // already recorded above, so a silent zero here would let a real
            // operand vanish between the two facts.
            if (sink.empty()) {
                InlineAsmOperandFact bad;
                bad.operandNode     = cur.node;
                bad.isOutput        = isOutputs;
                bad.malformed       = true;
                bad.malformedDetail =
                    "the operand list contains no node of the configured "
                    "operand shape '" + ia.operandRuleName
                    + "' (semantics.inlineAsm.operandRule)";
                sink.push_back(std::move(bad));
            }
            continue;                              // prune: a leaf section
        } else if (ia.clobberListRule.valid() && r.v == ia.clobberListRule.v) {
            f.hasClobbers = true;
            // A clobber list holds string literals and no expressions at all,
            // so every `templateRule` node beneath it IS a clobber — the walk
            // is exact here in a way it is not inside an operand.
            for (NodeId s : descendantsOfRule(tree, cur.node, ia.templateRule)) {
                auto decoded = decodeAdjacentStringBodies(tree, s, bodyToken);
                f.clobbers.push_back(
                    {s, decoded ? *decoded
                                : std::string{"<malformed string literal>"}});
            }
            continue;                              // prune: a leaf section
        } else if (ia.gotoLabelListRule.valid() && r.v == ia.gotoLabelListRule.v) {
            f.hasLabelList = true;
            if (identifierToken.valid()) {
                std::vector<NodeId> lstk;
                pushChildrenReversed(tree, cur.node, lstk);
                int lguard = 0;
                while (!lstk.empty() && ++lguard <= 4096) {
                    NodeId const ln = lstk.back();
                    lstk.pop_back();
                    if (tree.kind(ln) == NodeKind::Token) {
                        if (tree.tokenKind(ln).v == identifierToken.v)
                            f.labels.push_back({ln, std::string{tree.text(ln)}});
                        continue;
                    }
                    pushChildrenReversed(tree, ln, lstk);
                }
            }
            continue;                              // prune: a leaf section
        }
        auto const kids = visibleChildren(tree, cur.node);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back({*it, tail});
    }

    // ★ OUTPUTS FIRST, THEN INPUTS — the `%N` index space, materialized ONCE
    // here so no consumer re-derives it. The DFS already yields document order
    // within each section; the concatenation is what makes `%0` the first
    // OUTPUT when one exists and the first INPUT when it does not.
    f.outputCount = static_cast<std::uint32_t>(outputOps.size());
    f.operands    = std::move(outputOps);
    f.operands.insert(f.operands.end(),
                      std::make_move_iterator(inputOps.begin()),
                      std::make_move_iterator(inputOps.end()));

    // ★ AND THE SPELLINGS THAT NAME THAT INDEX SPACE, MINTED ONCE, HERE. The
    // concatenation above is the numbering; `mintTemplateSpellings` is the only
    // place it is ever rendered into the language's own sigils, so no tier
    // below re-derives it. Labels are already captured by the structural scan
    // (the label list is a leaf section, pruned before this point), so both
    // lists are final.
    mintTemplateSpellings(f, lex);

    // ── (2) the QUALIFIER scan: PRE-TEMPLATE tokens, PRUNED at every tail ──
    // ★ THE BOUND IS LOAD-BEARING, NOT AN OPTIMISATION. A qualifier is a plain
    // keyword token, and an operand EXPRESSION can legitimately contain the same
    // keyword — `"r"(*(volatile int*)p)` is real code. An unbounded search for
    // "a volatile token under this asm statement" would report that cast as a
    // duplicate qualifier, i.e. REFUSE VALID C (the TF-C77 lesson: a gate that
    // refuses code every real toolchain compiles is worse than the silence it
    // replaced). Qualifiers sit between the keyword and the `(`; sections sit
    // after the template. Stopping at the template and never entering a tail
    // leaves exactly {asm keyword, qualifiers, `(`} — and of those only a
    // qualifier can repeat, since the grammar requires exactly one of the others.
    // That is also why the duplicate test is over ALL kinds in this window rather
    // than an enumerated qualifier set: a language that later adds `asm inline`
    // is covered WITHOUT a code change, instead of silently unchecked.
    {
        std::vector<NodeId> stk;
        pushChildrenReversed(tree, asmNode, stk);
        std::vector<std::pair<std::uint32_t, NodeId>> seen;
        int qguard = 0;
        while (!stk.empty() && ++qguard <= 4096) {
            NodeId const cur = stk.back();
            stk.pop_back();
            if (f.templateNode.valid() && cur.v == f.templateNode.v) break;
            if (tree.kind(cur) == NodeKind::Token) {
                std::uint32_t const kind = tree.tokenKind(cur).v;
                if (ia.gotoQualifierToken.valid()
                    && kind == ia.gotoQualifierToken.v) {
                    f.hasGotoQualifier = true;
                    if (f.gotoQualifierText.empty())
                        f.gotoQualifierText = tree.text(cur);
                }
                for (auto const& [k2, n2] : seen) {
                    if (k2 != kind) continue;
                    if (!f.dupQualifierTok.valid()) {
                        f.dupQualifierTok  = cur;      // the SECOND occurrence
                        f.dupQualifierText = tree.text(cur);
                    }
                    break;
                }
                seen.emplace_back(kind, cur);
                continue;
            }
            if (ia.isTailRule(tree.rule(cur))) continue;   // never enter a section
            pushChildrenReversed(tree, cur, stk);
        }
    }
    return f;
}

} // namespace dss
