#pragma once

#include "core/types/semantic_config.hpp"        // InlineAsmConfig
#include "core/types/string_literal_decode.hpp"  // decodeAdjacentStringBodies
#include "core/types/tree.hpp"                   // Tree / NodeId / NodeKind

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

} // namespace inline_asm_detail

// Gather every inline-asm fact for `asmNode`.
[[nodiscard]] inline InlineAsmFacts
gatherInlineAsmFacts(Tree const& tree, NodeId asmNode, InlineAsmConfig const& ia,
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
