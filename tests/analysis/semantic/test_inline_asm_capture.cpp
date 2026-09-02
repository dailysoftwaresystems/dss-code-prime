// Inline-asm P5 (D-CSUBSET-INLINE-ASM-OPERANDS / -TEXT): the FRONT-END CAPTURE.
//
// Three things are pinned here and nothing else:
//   1. the CST shape the capture reads, as a MEASUREMENT rather than a belief;
//   2. `parseAsmConstraint` — the GNU-grammar half of a constraint string;
//   3. every P5 refusal, each with its CODE, its SPAN, and a matched POSITIVE
//      CONTROL proving the supported neighbour still compiles.
//
// ★★ EVERY REFUSAL HAS A CONTROL, AND THAT IS THE POINT OF THE FILE. A refusal
// pin alone is satisfied by a compiler that refuses everything — which is
// literally what this cycle replaced. The control is what makes each pin a
// statement about ONE construct.

#include "analysis/semantic/inline_asm_facts.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_visitor.hpp"
#include "hir/hir_inline_asm.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] std::shared_ptr<TargetSchema const> shippedTarget(std::string_view arch) {
    auto loaded = TargetSchema::loadShipped(arch);
    if (!loaded) {
        ADD_FAILURE() << "TargetSchema::loadShipped(\"" << arch << "\") failed";
        return nullptr;
    }
    return *loaded;
}

// Analyze one c source WITH a target in scope, so the target-dependent
// constraint and clobber checks actually run.
//
// ⚠ THE TARGET MUST OUTLIVE THE MODEL — the model holds a NON-OWNING pointer —
// so the schema is returned alongside it rather than dropped at the end of a
// helper. A dangling target here would not fail loudly; it would read garbage
// letters, which is exactly the shape this suite exists to catch elsewhere.
struct Analyzed {
    std::shared_ptr<TargetSchema const>    target;
    std::shared_ptr<CompilationUnit const> cu;
    std::optional<SemanticModel>           model;
};

[[nodiscard]] Analyzed analyzeFor(std::string_view arch, std::string src) {
    Analyzed a;
    a.target = shippedTarget(arch);
    a.cu     = buildShippedUnit("c", {std::move(src)});
    a.model.emplace(analyze(a.cu, DiagnosticBudget::libraryDefault(),
                            DataModel::Lp64, std::nullopt, std::nullopt,
                            std::nullopt, arch, LongDoubleFormat::None,
                            a.target.get()));
    return a;
}

[[nodiscard]] bool has(SemanticModel const& m, DiagnosticCode c) {
    for (auto const& d : m.diagnostics().all()) {
        if (d.code == c) return true;
    }
    return false;
}

[[nodiscard]] std::size_t countOf(SemanticModel const& m, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : m.diagnostics().all()) {
        if (d.code == c) ++n;
    }
    return n;
}

[[nodiscard]] std::string errorInventory(SemanticModel const& m) {
    std::string out;
    for (auto const& d : m.diagnostics().all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += diagnosticCodeName(d.code);
        out += ": ";
        out += d.actual;
        out += "\n";
    }
    return out.empty() ? std::string{"(no errors)"} : out;
}

// Wrap a statement in a minimal function with two live locals and a label, so
// every probe below differs ONLY in the asm statement under test.
[[nodiscard]] std::string wrap(std::string_view stmt) {
    return "int main(void){ unsigned lo = 0, hi = 0; (void)lo; (void)hi; "
           + std::string{stmt} + " lbl: return 0; }\n";
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. THE CST SHAPE, MEASURED
// ─────────────────────────────────────────────────────────────────────────────

// ★★ THIS PIN IS THE PREMISE `captureOperand` RESTS ON, recorded as a
// measurement because the alternative was an inference. `asmOperand` is
// `{optional [ '[' name ']' ]} templateText '(' operandExpr ')'`, and whether
// the OPTIONAL group mints a node decides whether "the first Internal child is
// the constraint" is true or is an off-by-one. ✔MEASURED 2026-08-15 on the
// shipped c + asm grammars: it does NOT — the prefix is three bare
// tokens, and the operand has EXACTLY TWO Internal children, constraint first
// and value expression last.
//
// If a grammar edit ever mints a node for the prefix, this pin reds FIRST and
// names the shape, instead of the capture silently promoting the prefix node to
// a constraint.
TEST(InlineAsmCapture, AnOperandHasExactlyTwoCompositeChildrenConstraintThenValue) {
    auto cu = buildShippedUnit(
        "c", {wrap("__asm__ (\"m %0,%1\" : [o] \"=r\"(lo) : \"r\"(hi));")});
    assertNoBuilderErrors(*cu);
    auto const& schema   = cu->schema();
    auto const& ia       = schema.semantics().inlineAsm;
    ASSERT_TRUE(ia.operandRule.valid())
        << "semantics.inlineAsm.operandRule must be declared — the whole capture "
           "is keyed on it";

    std::size_t operandsSeen = 0;
    for (auto const& t : cu->trees()) {
        walkPreOrder(t, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (t.kind(n) != NodeKind::Internal) return;
            if (t.rule(n).v != ia.operandRule.v) return;
            ++operandsSeen;
            std::vector<NodeId> internals;
            std::size_t         tokens = 0;
            for (NodeId c : inline_asm_detail::visibleChildren(t, n)) {
                if (t.kind(c) == NodeKind::Internal) internals.push_back(c);
                else ++tokens;
            }
            ASSERT_EQ(internals.size(), 2u)
                << "an asmOperand must present exactly two composite children "
                   "(constraint, value expression); got " << internals.size()
                << " with " << tokens << " tokens";
            EXPECT_EQ(t.rule(internals.front()).v, ia.templateRule.v)
                << "the FIRST composite child must be the constraint "
                   "(semantics.inlineAsm.templateRule)";
            EXPECT_NE(t.rule(internals.back()).v, ia.templateRule.v)
                << "the value expression of `(lo)` / `(hi)` is not a string "
                   "literal, so the two children are distinguishable here";
        });
    }
    EXPECT_EQ(operandsSeen, 2u) << "the fixture declares one output and one input";
}

// The capture's own product: two operands, OUTPUTS FIRST, each with its
// constraint, its symbolic name and a valid value-expression NodeId.
// ★ The NodeIds are the hole this lane closed — P1 captured constraint STRINGS
// and zero expression nodes, so nothing downstream could bind anything.
TEST(InlineAsmCapture, EveryOperandYieldsAConstraintAndAValueExpressionNodeId) {
    auto cu = buildShippedUnit(
        "c", {wrap("__asm__ (\"m %0,%1\" : [out] \"=r\"(lo) : \"r\"(hi));")});
    assertNoBuilderErrors(*cu);
    auto const& schema = cu->schema();
    auto const& ia     = schema.semantics().inlineAsm;
    ASSERT_TRUE(ia.rule.valid());

    bool found = false;
    for (auto const& t : cu->trees()) {
        walkPreOrder(t, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (t.kind(n) != NodeKind::Internal || t.rule(n).v != ia.rule.v) return;
            found = true;
            auto const f = gatherInlineAsmFacts(
                t, n, ia, schema.semantics().inlineAsmTemplateLexemes,
                schema.semantics().identifierToken,
                schema.hirLowering().stringBodyToken);
            ASSERT_EQ(f.operands.size(), 2u);
            EXPECT_EQ(f.outputCount, 1u) << "outputs come FIRST and outputCount splits";
            EXPECT_TRUE(f.isExtended)    << "any colon makes the statement extended";
            ASSERT_TRUE(f.templateText.has_value());
            EXPECT_EQ(*f.templateText, "m %0,%1");

            EXPECT_FALSE(f.operands[0].malformed) << f.operands[0].malformedDetail;
            EXPECT_TRUE(f.operands[0].isOutput);
            EXPECT_EQ(f.operands[0].constraint, "=r");
            EXPECT_EQ(f.operands[0].symbolicName, "out");
            EXPECT_TRUE(f.operands[0].valueExpr.valid())
                << "the VALUE EXPRESSION NodeId is the fact P1 never captured";
            EXPECT_TRUE(f.operands[0].constraintNode.valid());
            EXPECT_NE(f.operands[0].valueExpr.v, f.operands[0].constraintNode.v);

            EXPECT_FALSE(f.operands[1].malformed) << f.operands[1].malformedDetail;
            EXPECT_FALSE(f.operands[1].isOutput);
            EXPECT_EQ(f.operands[1].constraint, "r");
            EXPECT_TRUE(f.operands[1].symbolicName.empty());
            EXPECT_TRUE(f.operands[1].valueExpr.valid());
        });
    }
    EXPECT_TRUE(found) << "the fixture must contain one asmStmt";
}

// ★★★ THE MINTING SITE, PINNED ON CONTENT. Every tier below the front end
// COMPARES template spellings and never composes one, so the strings this
// gatherer produces ARE the contract — a count assertion would be satisfied by
// two wrong strings, which is the failure this pin exists to exclude.
//
// ★★ THE FIXTURE IS CHOSEN TO DISCRIMINATE THE ONE MISTAKE THAT IS INVISIBLE
// EVERYWHERE ELSE: a `"+"` read-write operand is ONE entry in the source list
// and becomes an output PLUS a synthesized tied input further down the
// pipeline. A label base computed from that later list would be 3 here instead
// of 2, so `%l2` would silently name the WRONG edge (or be refused as naming an
// operand). With ONE output and ONE input the two bases agree and the pin
// proves nothing — hence the `"+"`.
//
// ⓘ The lexeme guards below are not decoration: the expectations are literal
// bytes, so if the c ever re-spelled a sigil this would fail with a
// confusing string diff instead of naming the config change that caused it.
TEST(InlineAsmCapture, EverySpellingIsMintedFromTheDeclaredLexemesAndAPlusOperandCountsOnce) {
    auto cu = buildShippedUnit(
        "c",
        {wrap("__asm__ goto (\"jmp %l2\" : [o] \"+r\"(lo) : \"r\"(hi) : : lbl);")});
    assertNoBuilderErrors(*cu);
    auto const& schema = cu->schema();
    auto const& ia     = schema.semantics().inlineAsm;
    auto const& lex    = schema.semantics().inlineAsmTemplateLexemes;
    ASSERT_TRUE(ia.rule.valid());
    ASSERT_EQ(lex.placeholder, "%");
    ASSERT_EQ(lex.labelPlaceholder, "%l");
    ASSERT_EQ(lex.symbolicNameOpen, "[");
    ASSERT_EQ(lex.symbolicNameClose, "]");

    bool found = false;
    for (auto const& t : cu->trees()) {
        walkPreOrder(t, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (t.kind(n) != NodeKind::Internal || t.rule(n).v != ia.rule.v) return;
            found = true;
            auto const f = gatherInlineAsmFacts(
                t, n, ia, lex, schema.semantics().identifierToken,
                schema.hirLowering().stringBodyToken);
            ASSERT_EQ(f.operands.size(), 2u)
                << "a `+` operand is ONE entry in the source list";
            ASSERT_EQ(f.labels.size(), 1u);

            // Positional first, then symbolic — the order the header states.
            EXPECT_EQ(f.operands[0].spellings,
                      (std::vector<std::string>{"%0", "%[o]"}));
            // No `[name]` was written, so there is no symbolic spelling to mint
            // — and minting one anyway would let `%[hi]` bind a variable name
            // the statement never exposed to the template.
            EXPECT_EQ(f.operands[1].spellings,
                      (std::vector<std::string>{"%1"}));
            // Bracketed first, then positional; the index is
            // `operandCount + position` = 2, NOT 3.
            EXPECT_EQ(f.labels[0].spellings,
                      (std::vector<std::string>{"%l[lbl]", "%l2"}));
        });
    }
    EXPECT_TRUE(found) << "the fixture must contain one asmStmt";
}

// ─────────────────────────────────────────────────────────────────────────────
// 1b. THE MINTING IS THE ONLY NUMBERING — THE DERIVATION, SHOWN LIVE
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// What the template scan SAID, so an arm can assert on content rather than on a
// count. The scan is a free template over its report sink, which is why it can
// be driven here with no analyzer, no target and no diagnostic engine.
struct ScanSaid {
    std::vector<DiagnosticCode> codes;
    std::vector<std::string>    messages;
    [[nodiscard]] bool clean() const { return codes.empty(); }
    [[nodiscard]] std::string joined() const {
        std::string out;
        for (auto const& m : messages) { out += m; out += "\n"; }
        return out.empty() ? std::string{"(the scan said nothing)"} : out;
    }
};

[[nodiscard]] ScanSaid scanTemplate(InlineAsmFacts const&           f,
                                    InlineAsmTemplateLexemes const& lex,
                                    std::string_view                text) {
    ScanSaid said;
    inline_asm_detail::scanInlineAsmTemplate(
        f, lex, text,
        [&](DiagnosticCode code, NodeId, std::string message) {
            said.codes.push_back(code);
            said.messages.push_back(std::move(message));
        });
    return said;
}

// A form as a diagnostic quotes it. A backtick is this project's quoting
// convention and no language's syntax, so writing one here is not writing a
// sigil.
[[nodiscard]] std::string quoted(std::string const& form) {
    return "`" + form + "`";
}

// The facts of the ONE `asmStmt` in `cu`, or nullopt when it holds none.
[[nodiscard]] std::optional<InlineAsmFacts>
onlyAsmFacts(CompilationUnit const& cu) {
    auto const& schema = cu.schema();
    auto const& ia     = schema.semantics().inlineAsm;
    if (!ia.rule.valid()) return std::nullopt;
    std::optional<InlineAsmFacts> out;
    for (auto const& t : cu.trees()) {
        walkPreOrder(t, [&](TreeCursor const& cursor) {
            NodeId const n = cursor.current();
            if (t.kind(n) != NodeKind::Internal || t.rule(n).v != ia.rule.v) return;
            if (out.has_value()) {
                ADD_FAILURE() << "the fixture holds more than one asmStmt — the "
                                 "arm below would be measuring whichever came "
                                 "last";
                return;
            }
            out = gatherInlineAsmFacts(t, n, ia,
                                       schema.semantics().inlineAsmTemplateLexemes,
                                       schema.semantics().identifierToken,
                                       schema.hirLowering().stringBodyToken);
        });
    }
    return out;
}

} // namespace

// ★★★ THE PIN THAT PROVES THE SEMANTIC TIER'S ANSWER **IS** THE MINTING'S
// ANSWER, RATHER THAN A SECOND ANSWER THAT AGREES WITH IT.
//
// ⚠⚠ WHAT THE REST OF THIS FILE COULD NOT DO, AND IT WAS MEASURED BY AN
// INDEPENDENT P20 AUDIT RATHER THAN REASONED ABOUT: force
// `mintTemplateSpellings`' base to 0 and every other arm here stays GREEN.
// `scanInlineAsmTemplate` re-derived `base = operandCount` in FIVE places — the
// rendered label range, both `%l<N>` bounds, the `%<N>` label-range test, and an
// INDEX MAPPING (`f.labels[idx - operandCount]`) that named a label in a message
// — so the two copies agreed in the shipped tree and diverged only under a
// mutant this suite had no way to build. The divergence surfaced two tiers down,
// at the LIR label binding. A pin satisfied by two copies that happen to agree
// is not a pin on the derivation.
//
// ★★ SO THE MUTANT IS BUILT HERE, ON THE FACT ITSELF: the label's spellings are
// rewritten exactly as a base of 0 would have minted them, and the SAME scan is
// re-run over the SAME templates. Every verdict must INVERT. That can only
// happen if the scan READS the spellings; a scan holding a numbering of its own
// answers identically in both columns, which is the RED this arm produces on the
// pre-P20 code.
//
// ⓘ THE SHAPE IS `test_inline_asm_template_sigil_agreement.cpp`'s, one tier
// over: move the single declaration and require everything downstream to move
// with it. There the declaration is on disk (the language's lexemes) and the
// mutant is a config rewrite; here it is a C++ list and the mutant is a rewrite
// of that list.
//
// ★ THE FIXTURE'S `"+"` OPERAND IS LOAD-BEARING, for the reason the minting pin
// above states: it is ONE entry in the source list and becomes an output PLUS a
// synthesized tied input further down, so a base taken from that later list
// would be 3 here. And the fixture's own template carries NO placeholder at all
// — every form this arm asks about is built through the language's renderers, so
// no GNU byte is written in the assertions.
TEST(InlineAsmCapture, TheTemplateScanFollowsTheMintedSpellingsRatherThanRecomputingTheBase) {
    auto cu = buildShippedUnit(
        "c",
        {wrap("__asm__ goto (\"jmp\" : [o] \"+r\"(lo) : \"r\"(hi) : : lbl);")});
    assertNoBuilderErrors(*cu);
    auto const& lex = cu->schema().semantics().inlineAsmTemplateLexemes;
    ASSERT_FALSE(lex.placeholder.empty())
        << "c declares no operand placeholder — the scan returns without "
           "scanning and this arm would assert nothing";
    ASSERT_FALSE(lex.labelPlaceholder.empty())
        << "c declares no label placeholder — there would be no label "
           "numbering to move";

    auto const shipped = onlyAsmFacts(*cu);
    ASSERT_TRUE(shipped.has_value()) << "the fixture must contain one asmStmt";
    ASSERT_EQ(shipped->operands.size(), 2u) << "a `+` operand is ONE entry";
    ASSERT_EQ(shipped->labels.size(), 1u);
    ASSERT_TRUE(shipped->isExtended) << "any colon makes the statement extended";
    std::string const labelName = shipped->labels[0].name;
    ASSERT_FALSE(labelName.empty());

    // The form the SHIPPED minting gives this label, and the form a base of 0
    // would give it. Both through the language's own renderer.
    std::string const shippedForm = lex.labelRef("2");
    std::string const mutantForm  = lex.labelRef("0");
    ASSERT_NE(shippedForm, mutantForm);
    ASSERT_EQ(shipped->labels[0].spellings,
              (std::vector<std::string>{lex.namedLabelRef(labelName), shippedForm}))
        << "THE PREMISE: the shipped minting numbers this label AFTER the two "
           "operands. Without it the two columns below are not a differential";

    // ── COLUMN 1: the SHIPPED minting ──
    auto const okShipped = scanTemplate(*shipped, lex, "jmp " + shippedForm);
    EXPECT_TRUE(okShipped.clean())
        << "the form the minting gives this label must clear the tier that "
           "guards the lowering: " << okShipped.joined();
    auto const badShipped = scanTemplate(*shipped, lex, "jmp " + mutantForm);
    EXPECT_FALSE(badShipped.clean())
        << "under the shipped minting that index names OPERAND 0, not a label";

    // ── COLUMN 2: the MUTANT — the same list, minted at a base of 0 ──
    InlineAsmFacts mutant = *shipped;
    mutant.labels[0].spellings = {lex.namedLabelRef(labelName), mutantForm};
    ASSERT_NE(mutant.labels[0].spellings, shipped->labels[0].spellings)
        << "the mutation left the spellings IDENTICAL — both columns below would "
           "be measuring one state twice, which is the shape of a red-on-disable "
           "whose 'good' column silently ran the mutant";

    EXPECT_FALSE(scanTemplate(mutant, lex, "jmp " + shippedForm).clean())
        << "the scan still ACCEPTS the shipped label form after the minting "
           "moved it — it is deciding from a numbering of its own, which is the "
           "two-owner state this arm exists to forbid";
    auto const okMutant = scanTemplate(mutant, lex, "jmp " + mutantForm);
    EXPECT_TRUE(okMutant.clean())
        << "the scan did not FOLLOW the minting to the form it now mints — it "
           "kept validating against a base it computed itself: "
        << okMutant.joined();

    // ── AND THE `%<N>` SIDE MOVES TOO ──
    // The operand form's "you meant the label" advice is the same fact read from
    // the other side, and it used to be reached by INDEXING
    // `f.labels[idx - operandCount]` — the fifth re-derivation the audit named,
    // a real index mapping rather than prose. Under the shipped minting index 2
    // IS this statement's label; after the minting moves it names nothing, and
    // advice that still recommended the label would be reading a numbering this
    // tier no longer has.
    auto const advice = scanTemplate(*shipped, lex, "jmp " + lex.operandRef("2"));
    ASSERT_FALSE(advice.clean()) << "no operand answers to that form";
    EXPECT_NE(advice.joined().find(quoted(lex.namedLabelRef(labelName))),
              std::string::npos)
        << "the actionable half is the forms the label at that index answers to: "
        << advice.joined();
    auto const noAdvice = scanTemplate(mutant, lex, "jmp " + lex.operandRef("2"));
    ASSERT_FALSE(noAdvice.clean());
    EXPECT_EQ(noAdvice.joined().find(lex.namedLabelRef(labelName)),
              std::string::npos)
        << "after the minting moved, index 2 names neither an operand nor a "
           "label — recommending the label anyway would be the index mapping "
           "answering from a base of its own: " << noAdvice.joined();
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. THE CONSTRAINT STRING, SPLIT
// ─────────────────────────────────────────────────────────────────────────────

TEST(InlineAsmConstraintParse, ModifiersSplitOffTheLetterAndTheLetterIsLeftAlone) {
    struct Row {
        char const*            raw;
        char const*            letter;
        bool                   out, rw, early, commutative;
    };
    // ★ `"Ush"` is in the table on purpose: it is a REAL aarch64 machine
    // constraint and it proves the parser does not treat "longer than one
    // character" as a defect. Deciding whether a long spelling is one letter or
    // several is the TARGET's job (`asmConstraintLooksMultiLetter`), not this
    // function's.
    Row const rows[] = {
        {"r",    "r",   false, false, false, false},
        {"=r",   "r",   true,  false, false, false},
        {"+r",   "r",   true,  true,  false, false},
        {"=&r",  "r",   true,  false, true,  false},
        {"+&r",  "r",   true,  true,  true,  false},
        {"%r",   "r",   false, false, false, true },
        {"=%r",  "r",   true,  false, false, true },
        {"a",    "a",   false, false, false, false},
        {"Ush",  "Ush", false, false, false, false},
        {"0",    "0",   false, false, false, false},
    };
    for (auto const& r : rows) {
        auto const p = parseAsmConstraint(r.raw);
        EXPECT_TRUE(p.ok()) << r.raw << " -> "
                            << asmConstraintDefectDescription(p.defect);
        EXPECT_EQ(p.value.letter, r.letter)             << r.raw;
        EXPECT_EQ(p.value.isOutput, r.out)              << r.raw;
        EXPECT_EQ(p.value.isReadWrite, r.rw)            << r.raw;
        EXPECT_EQ(p.value.earlyClobber, r.early)        << r.raw;
        EXPECT_EQ(p.value.commutative, r.commutative)   << r.raw;
        EXPECT_EQ(p.value.raw, r.raw)                   << "raw is kept verbatim";
    }
}

TEST(InlineAsmConstraintParse, EveryRefusedShapeIsNamedRatherThanLumped) {
    struct Row { char const* raw; HirAsmConstraintDefect defect; };
    Row const rows[] = {
        {"=r,m", HirAsmConstraintDefect::MultiAlternative},
        {"r,m",  HirAsmConstraintDefect::MultiAlternative},
        {"#r,m", HirAsmConstraintDefect::MultiAlternative},   // comma wins, see below
        {"",     HirAsmConstraintDefect::Empty},
        {"=",    HirAsmConstraintDefect::Empty},
        {"=&",   HirAsmConstraintDefect::Empty},
        {"#r",   HirAsmConstraintDefect::UnknownModifier},
        {"?r",   HirAsmConstraintDefect::UnknownModifier},
        {"!r",   HirAsmConstraintDefect::UnknownModifier},
        {"r&",   HirAsmConstraintDefect::UnknownModifier},
        {"==r",  HirAsmConstraintDefect::MisplacedOutputModifier},
        {"=+r",  HirAsmConstraintDefect::MisplacedOutputModifier},
        {"&=r",  HirAsmConstraintDefect::MisplacedOutputModifier},
        {"r=",   HirAsmConstraintDefect::MisplacedOutputModifier},
    };
    for (auto const& r : rows) {
        auto const p = parseAsmConstraint(r.raw);
        EXPECT_FALSE(p.ok()) << '"' << r.raw << "\" must be refused";
        EXPECT_EQ(p.defect, r.defect)
            << '"' << r.raw << "\" got: "
            << asmConstraintDefectDescription(p.defect);
        EXPECT_TRUE(p.value.letter.empty())
            << "a refused constraint must expose NO letter — a half-parsed one "
               "would be resolved against the target and report the wrong defect";
        EXPECT_EQ(p.value.raw, r.raw) << "the text is still quotable";
    }
    // ★ THE ORDERING PIN, stated as its own claim: the comma is decided FIRST
    // and WITHOUT looking at the modifiers, so `"#r,m"` above reports
    // MULTI-ALTERNATIVE and not UNKNOWN-MODIFIER. Both are true of that string;
    // only one sends the author to a fix that works.
    EXPECT_EQ(parseAsmConstraint("#r,m").defect,
              HirAsmConstraintDefect::MultiAlternative);
}

// ★★★ THE CONFIG-NOT-CODE PROOF, and it is the one this facet exists for: the
// SAME letter resolves to different things — or to nothing — under two targets,
// with no `if (arch == …)` anywhere in the resolution path.
TEST(InlineAsmConstraintParse, OneLetterTwoTargetsIsAConfigAnswerNotACodeBranch) {
    auto const x86 = shippedTarget("x86_64");
    auto const arm = shippedTarget("arm64");
    ASSERT_TRUE(x86);
    ASSERT_TRUE(arm);

    // `a` is x86's %rax and is UNDECLARED on arm64.
    EXPECT_NE(x86->asmConstraint("a"), nullptr);
    EXPECT_EQ(arm->asmConstraint("a"), nullptr);
    // `w` is arm64's SIMD&FP class and is UNDECLARED on x86_64. ⓘ It used to
    // be described as "arm64's vector class": since R1 of design A′ arm64
    // declares that register file ONCE, under class `fpr`, and `w` binds it —
    // there is no separate vector class to name.
    EXPECT_NE(arm->asmConstraint("w"), nullptr);
    EXPECT_EQ(x86->asmConstraint("w"), nullptr);
    // `r` is declared by BOTH — and resolves into disjoint register sets.
    ASSERT_NE(x86->asmConstraint("r"), nullptr);
    ASSERT_NE(arm->asmConstraint("r"), nullptr);

    // Multi-letter is PROVEN, never guessed from length: `rm` is two declared
    // letters on both targets; `Ush` is undeclared on both and is NOT reported
    // as multi-letter because `U` alone does not resolve.
    EXPECT_TRUE(asmConstraintLooksMultiLetter(*x86, "rm"));
    EXPECT_TRUE(asmConstraintLooksMultiLetter(*arm, "rm"));
    EXPECT_FALSE(asmConstraintLooksMultiLetter(*x86, "Ush"));
    EXPECT_FALSE(asmConstraintLooksMultiLetter(*arm, "Ush"));
    EXPECT_FALSE(asmConstraintLooksMultiLetter(*x86, "r"))
        << "a single declared letter is never multi-letter";
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. THE REFUSALS — each with its code, its span, and a POSITIVE CONTROL
// ─────────────────────────────────────────────────────────────────────────────

// S0065. The control is the neighbouring letter that DOES resolve, on the same
// target and in the same statement shape.
TEST(InlineAsmRefusals, AnUndeclaredConstraintLetterIsRefusedNamingTheTargetAndItsSet) {
    auto bad = analyzeFor("x86_64", wrap("__asm__ (\"\" : \"=w\"(lo));"));
    ASSERT_TRUE(bad.model.has_value());
    EXPECT_TRUE(has(*bad.model, DiagnosticCode::S_InlineAsmConstraintLetterUndeclared))
        << errorInventory(*bad.model);
    bool sawSetAndTarget = false;
    for (auto const& d : bad.model->diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InlineAsmConstraintLetterUndeclared) continue;
        // The message must RENDER the declared set rather than a hand-typed
        // list — the letters are per-CPU, so there is no correct constant.
        sawSetAndTarget = d.actual.find("'a'") != std::string::npos
                          && d.actual.find("x86_64") != std::string::npos;
    }
    EXPECT_TRUE(sawSetAndTarget)
        << "the message must name the TARGET and render "
           "declaredAsmConstraintLetters(): " << errorInventory(*bad.model);

    // POSITIVE CONTROL — the same statement with a letter x86_64 declares.
    auto good = analyzeFor("x86_64", wrap("__asm__ (\"\" : \"=a\"(lo));"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);

    // ★ AND THE MIRROR, which is what makes this a statement about CONFIG: the
    // SAME two sources swap verdicts under arm64.
    auto armBad  = analyzeFor("arm64", wrap("__asm__ (\"\" : \"=a\"(lo));"));
    auto armGood = analyzeFor("arm64", wrap("__asm__ (\"\" : \"=w\"(lo));"));
    ASSERT_TRUE(armBad.model.has_value() && armGood.model.has_value());
    EXPECT_TRUE(has(*armBad.model, DiagnosticCode::S_InlineAsmConstraintLetterUndeclared))
        << errorInventory(*armBad.model);
    EXPECT_FALSE(armGood.model->hasErrors()) << errorInventory(*armGood.model);
}

// S0066 — three shapes, three messages, one control.
TEST(InlineAsmRefusals, AnUnsupportedConstraintFormIsRefusedNamingWhichShapeFired) {
    struct Row { char const* constraint; char const* mustSay; };
    Row const rows[] = {
        {"=r,m", "MULTI-ALTERNATIVE"},
        {"#r",   "UNRECOGNISED MODIFIER"},
        {"=+r",  "MISPLACED OUTPUT MODIFIER"},
        {"rm",   "MULTI-LETTER"},   // target-proven: r and m both resolve, rm does not
    };
    for (auto const& r : rows) {
        auto a = analyzeFor("x86_64",
                            wrap(std::string{"__asm__ (\"\" : \""} + r.constraint
                                 + "\"(lo));"));
        ASSERT_TRUE(a.model.has_value());
        EXPECT_TRUE(has(*a.model, DiagnosticCode::S_InlineAsmConstraintUnsupportedForm))
            << r.constraint << " -> " << errorInventory(*a.model);
        bool named = false;
        for (auto const& d : a.model->diagnostics().all()) {
            if (d.code != DiagnosticCode::S_InlineAsmConstraintUnsupportedForm) continue;
            named = named || d.actual.find(r.mustSay) != std::string::npos;
        }
        EXPECT_TRUE(named) << r.constraint
                           << " must NAME which shape fired, not say "
                              "\"unsupported constraint\": "
                           << errorInventory(*a.model);
    }
    // POSITIVE CONTROL — the single-alternative, single-letter neighbour.
    auto good = analyzeFor("x86_64", wrap("__asm__ (\"\" : \"=r\"(lo));"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);
}

// ★★★ THE WIDTH-VIEW MODIFIER IS A CAPABILITY, AND THIS TEST IS THE INVERSION
// OF THE PIN THAT USED TO ASSERT THE OPPOSITE.
//
// It read `AnOperandWidthModifierIsRefusedRatherThanWidened` and demanded
// `S_InlineAsmOperandModifierUnsupported` on `%w0`. That was the CORRECT
// assertion while no document declared a width-view vocabulary — falling back
// to the full register runs a 32-bit operation 64-bit with a clean build log.
// The dialects now declare `assembly.templateModifiers`, so the refusal became
// the divergence: ✔MEASURED 2026-08-24 by execution on gcc 13.3.0, both ports,
// `-O0` and `-O2`, `%w0` renders `w0` on aarch64 and `%ax` on x86-64, and under
// §A.3b of the bar one working reference makes the behaviour REQUIRED.
//
// ★★ WHAT THE TEST ASSERTS NOW IS THE PROPERTY THE REFUSAL WAS PROTECTING,
// STATED POSITIVELY: a width view is ACCEPTED, and the thing that made
// accepting it dangerous — a reference to an operand that does not exist, which
// under the old code would have been swallowed by the same blanket refusal —
// is still refused, by the code that owns index questions.
TEST(InlineAsmRefusals, AWidthViewModifierIsAcceptedAndItsSelectorIsStillChecked) {
    // The exact template the old pin required to be REFUSED.
    auto good = analyzeFor(
        "arm64", wrap("__asm__ (\"mov %w0, %w1\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);

    // ★ THE OTHER DIALECT'S OWN LETTERS, on the target that declares them —
    // which is the half that proves the letters are CONFIG rather than a shared
    // table: `k` is spelled by x86-64 and not by aarch64.
    auto att = analyzeFor(
        "x86_64", wrap("__asm__ (\"movl %k1, %k0\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(att.model.has_value());
    EXPECT_FALSE(att.model->hasErrors()) << errorInventory(*att.model);

    // The SYMBOLIC selector through a view — one shape, both selectors, because
    // the width-view rule takes the SAME `asmTemplateSelector` the plain and
    // label forms take.
    auto named = analyzeFor(
        "arm64",
        wrap("__asm__ (\"mov %w[o], %w[i]\" : [o] \"=r\"(lo) : [i] \"r\"(hi));"));
    ASSERT_TRUE(named.model.has_value());
    EXPECT_FALSE(named.model->hasErrors()) << errorInventory(*named.model);

    // ⚠ THE SELECTOR IS STILL CHECKED, and this is the assertion that keeps the
    // acceptance from being a blanket one: `%w2` on a two-operand statement
    // names nothing, and the code that says so is the one that owns index
    // questions — never the modifier code, which no longer refuses anything.
    auto outOfRange = analyzeFor(
        "arm64", wrap("__asm__ (\"mov %w0, %w2\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(outOfRange.model.has_value());
    EXPECT_TRUE(has(*outOfRange.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*outOfRange.model);
    // The message must name BOTH forms — the one written and the one that has
    // to be bound. An author told only about `%2` cannot see which half of
    // `%w2` was wrong.
    bool namesBoth = false;
    for (auto const& d : outOfRange.model->diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InlineAsmPlaceholderOutOfRange) continue;
        namesBoth = namesBoth || (d.actual.find("%w2") != std::string::npos
                                  && d.actual.find("`%2`") != std::string::npos);
    }
    EXPECT_TRUE(namesBoth) << errorInventory(*outOfRange.model);

    // ⚠ AND THE FORM WITH NO SELECTOR AT ALL IS STILL REFUSED BY S0067 — the
    // un-escaped register (`%eax` where `%%eax` was meant) is the live case, and
    // it is what stops "accept any letter run" from accepting everything.
    auto bare = analyzeFor(
        "x86_64", wrap("__asm__ (\"xorl %eax, %eax\" ::: \"eax\");"));
    ASSERT_TRUE(bare.model.has_value());
    EXPECT_TRUE(has(*bare.model,
                    DiagnosticCode::S_InlineAsmOperandModifierUnsupported))
        << errorInventory(*bare.model);
}

// S0068 — with THREE controls, because "resolves" has three sources here.
TEST(InlineAsmRefusals, AnUnresolvableClobberIsRefusedAndTheThreeResolvableKindsAreNot) {
    auto bad = analyzeFor("x86_64", wrap("__asm__ (\"\" ::: \"nosuchregister\");"));
    ASSERT_TRUE(bad.model.has_value());
    EXPECT_TRUE(has(*bad.model, DiagnosticCode::S_InlineAsmClobberUnknown))
        << errorInventory(*bad.model);

    // CONTROLS: the two CONFIGURED non-register spellings, and a real register.
    for (char const* ok : {"memory", "cc", "rax"}) {
        auto good = analyzeFor(
            "x86_64", wrap(std::string{"__asm__ (\"\" ::: \""} + ok + "\");"));
        ASSERT_TRUE(good.model.has_value());
        EXPECT_FALSE(good.model->hasErrors())
            << '"' << ok << "\" must resolve: " << errorInventory(*good.model);
    }
    // ★ AND THE CROSS-TARGET HALF: `rax` is a register on x86_64 and is NOT one
    // on arm64, so the same clobber flips verdict — the register file is config
    // exactly as the letter table is.
    auto arm = analyzeFor("arm64", wrap("__asm__ (\"\" ::: \"rax\");"));
    ASSERT_TRUE(arm.model.has_value());
    EXPECT_TRUE(has(*arm.model, DiagnosticCode::S_InlineAsmClobberUnknown))
        << errorInventory(*arm.model);
    auto armOk = analyzeFor("arm64", wrap("__asm__ (\"\" ::: \"x0\", \"memory\");"));
    ASSERT_TRUE(armOk.model.has_value());
    EXPECT_FALSE(armOk.model->hasErrors()) << errorInventory(*armOk.model);
}

// S006A — the index space is the OUTPUTS-THEN-INPUTS concatenation, so the
// controls have to straddle the join or they prove nothing.
TEST(InlineAsmRefusals, APlaceholderPastTheJoinedOperandCountIsRefused) {
    auto bad = analyzeFor("x86_64",
                          wrap("__asm__ (\"m %2\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(bad.model.has_value());
    EXPECT_TRUE(has(*bad.model, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*bad.model);

    // ★ THE CONTROL THAT MATTERS: `%1` is the INPUT — reachable only because the
    // bound is the JOINED count. A check written against either section alone
    // would reject this and accept the `%2` above.
    auto good = analyzeFor("x86_64",
                           wrap("__asm__ (\"m %0, %1\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);

    // A symbolic reference that names no operand is the same fact, spelled the
    // other way; its control is the name that IS declared.
    auto badName = analyzeFor(
        "x86_64", wrap("__asm__ (\"m %[nope]\" : [o] \"=r\"(lo));"));
    ASSERT_TRUE(badName.model.has_value());
    EXPECT_TRUE(has(*badName.model, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*badName.model);
    auto goodName = analyzeFor(
        "x86_64", wrap("__asm__ (\"m %[o]\" : [o] \"=r\"(lo));"));
    ASSERT_TRUE(goodName.model.has_value());
    EXPECT_FALSE(goodName.model->hasErrors()) << errorInventory(*goodName.model);
}

// S006B. ✔MEASURED 2026-08-15 on gcc 13.3.0 and clang 18.1.3 (`-O2 -c`, sources
// fed as base64): a BASIC template containing `%0` is emitted VERBATIM and the
// ASSEMBLER then refuses it — `Error: bad register name '%0'` (gcc) /
// `error: invalid register name` (clang). The same holds for `%%eax`. The
// matched POSITIVE CONTROL `__asm__("xorl %eax, %eax")` assembles clean on both.
// ⇒ refusing here is NOT a divergence; it moves the refusal to the one tier that
// knows how many operands were declared. That measurement discharges the
// INFERRED honesty clause 0xE06B's own docblock demanded before a consumer landed.
TEST(InlineAsmRefusals, APercentFormInABasicTemplateIsRefusedButALiteralPercentIsNot) {
    for (char const* tmpl : {"movl %0, %eax", "xorl %%eax, %%eax", "m %[o]"}) {
        auto bad = analyzeFor(
            "x86_64", wrap(std::string{"__asm__ (\""} + tmpl + "\");"));
        ASSERT_TRUE(bad.model.has_value());
        EXPECT_TRUE(has(*bad.model, DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate))
            << tmpl << " -> " << errorInventory(*bad.model);
    }
    // ★ THE CONTROL IS THE MEASURED-GOOD PROGRAM: in BASIC asm `%` is LITERAL,
    // so `%eax` must compile. Without this the pin would be satisfied by
    // refusing every `%` in a basic template — which IS a divergence.
    auto good = analyzeFor("x86_64", wrap("__asm__ (\"xorl %eax, %eax\");"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);

    // ★ AND THE DISCRIMINATOR ITSELF: the SAME template becomes legal the moment
    // a colon makes the statement extended and the operand exists. "Any colon
    // makes it extended" is the measured rule, so `:` alone is enough to change
    // the reading — that is what this pair proves is implemented.
    auto extended = analyzeFor("x86_64", wrap("__asm__ (\"m %0\" : \"=r\"(lo));"));
    ASSERT_TRUE(extended.model.has_value());
    EXPECT_FALSE(extended.model->hasErrors()) << errorInventory(*extended.model);
}

// ★★★ `:::` WITH EVERY SECTION EMPTY IS **EXTENDED**, AND THIS IS THE ONLY PIN
// THAT SAYS SO ANYWHERE (D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER).
//
// Every other basic/extended pin in this file uses a statement with a POPULATED
// section, so all of them stay green under the wrong rule "extended ⟺ some
// section has content". `:::` is the one shape where the colon is the ONLY
// evidence, and it is the shape a downstream consumer cannot reconstruct: three
// colons, no outputs, no inputs, no clobbers.
//
// ✔ORACLES RE-MEASURED 2026-08-15 (not inherited from a brief) on gcc 13.3.0 and
// clang 18.1.3, `-c`, sources written to files so no shell quoting could alter a
// byte, with the matched control on each row:
//
//   `__asm__("xorl %eax,%eax")`          rc=0  / rc=0   ← BASIC, `%` literal
//   `__asm__("xorl %eax,%eax" :::)`      rc=1  / rc=1   ← "operand number missing
//                                                         after %-letter" /
//                                                         "invalid % escape"
//   `__asm__("xorl %eax,%eax" ::: "eax")` rc=1 / rc=1   ← the same message, which
//                                                         is what proves it is
//                                                         the COLON and not the
//                                                         clobber content
//   `__asm__("movl %%eax, %%ebx" :::)`   rc=0  / rc=0   ← extended, `%%` escape
//
// ★ WHY IT MATTERS BEYOND THIS TIER. The MIR/LIR expansion used to re-derive the
// surface from the section lists, which reads `:::` as BASIC — i.e. it would
// ACCEPT the bare `%eax` both oracles reject. That over-acceptance never reached
// a user because THIS check fires first; the pin below is what keeps that true.
// (The reconstruction's other half — refusing the `%%` row, which both oracles
// compile — was live, and is why `MirAsmDescriptor` now carries `isExtended`.)
//
// RED-ON-DISABLE: in `inline_asm_facts.hpp`'s structural scan, move
// `f.isExtended = true;` out of the `isTailRule(r)` arm and key it on the
// operand/clobber lists being non-empty instead → the `:::` row reads BASIC,
// `%eax` becomes a literal, the S0067 expectation drops to zero ⇒ RED, while
// every other basic/extended pin in this file stays green.
TEST(InlineAsmRefusals, EmptySectionsStillMakeTheTemplateExtended) {
    // The DISCRIMINATING statement: extended by the colon alone.
    auto empty = analyzeFor("x86_64",
                            wrap("__asm__ (\"xorl %eax,%eax\" :::);"));
    ASSERT_TRUE(empty.model.has_value());
    EXPECT_TRUE(has(*empty.model,
                    DiagnosticCode::S_InlineAsmOperandModifierUnsupported))
        << "`%eax` in an EXTENDED template is an error on gcc AND clang; reading "
           "`:::` as basic would ACCEPT it: " << errorInventory(*empty.model);
    EXPECT_FALSE(has(*empty.model,
                     DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate))
        << "the BASIC-template code must NOT fire — that would be the right "
           "refusal reached by the wrong reading: " << errorInventory(*empty.model);

    // CONTROL A — the same bytes with a POPULATED clobber section. Both oracles
    // give the identical message, so the reading must be identical here too.
    auto clob = analyzeFor("x86_64",
                           wrap("__asm__ (\"xorl %eax,%eax\" ::: \"rax\");"));
    ASSERT_TRUE(clob.model.has_value());
    EXPECT_TRUE(has(*clob.model,
                    DiagnosticCode::S_InlineAsmOperandModifierUnsupported))
        << errorInventory(*clob.model);

    // CONTROL B — the same bytes with NO colon. Both oracles COMPILE this, so
    // refusing it would be the divergence in the other direction, and without
    // this row the pin is satisfied by refusing `%eax` everywhere.
    auto basic = analyzeFor("x86_64", wrap("__asm__ (\"xorl %eax,%eax\");"));
    ASSERT_TRUE(basic.model.has_value());
    EXPECT_FALSE(basic.model->hasErrors()) << errorInventory(*basic.model);

    // CONTROL C — an EXTENDED template with empty sections that both oracles
    // COMPILE (`%%` is the literal-percent escape). It must pass the semantic
    // gate, so this pin cannot be met by refusing everything that carries `:::`.
    auto escaped = analyzeFor("x86_64",
                              wrap("__asm__ (\"movl %%eax, %%ebx\" :::);"));
    ASSERT_TRUE(escaped.model.has_value());
    EXPECT_FALSE(escaped.model->hasErrors()) << errorInventory(*escaped.model);
}

// S0069 — the template exists and cannot be turned into text at all.
TEST(InlineAsmRefusals, AnUndecodableTemplateIsRefusedRatherThanPartiallyAssembled) {
    auto bad = analyzeFor("x86_64", wrap("__asm__ (\"nop \\q\");"));
    ASSERT_TRUE(bad.model.has_value());
    EXPECT_TRUE(has(*bad.model, DiagnosticCode::S_InlineAsmTemplateUnparsable))
        << errorInventory(*bad.model);

    // POSITIVE CONTROL — a template with a WELL-FORMED escape decodes and passes.
    auto good = analyzeFor("x86_64", wrap("__asm__ (\"nop\\n\\tnop\");"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. WHAT STOPPED BEING REFUSED — the other half of "relax the gates"
// ─────────────────────────────────────────────────────────────────────────────

// ★★ THESE ARE THE PINS THAT WOULD CATCH A REVERT TO THE BLANKET REFUSAL. Each
// source is one gcc/clang accepts, and each was refused by S0062 before P5.
TEST(InlineAsmAcceptance, TheShapesTheReferenceCompilersAcceptNowCompile) {
    struct Row { char const* stmt; char const* why; };
    Row const rows[] = {
        {"__asm__ (\"nop\");",
         "a non-empty BASIC template is the commonest inline asm there is"},
        {"__asm__ __volatile__ (\"rdtsc\" : \"=a\"(lo), \"=d\"(hi));",
         "sqlite's own hwtime.h — the arc's exit criterion"},
        {"__asm__ (\"\" ::: \"memory\");",
         "the compiler barrier every lock-free header writes"},
        {"__asm__ goto (\"\" : : : : lbl);",
         "asm goto WITH labels"},
        {"__asm__ goto (\"\" : : : :);",
         "asm goto with NO labels — gcc 13.3 rejects, clang 18/19 ACCEPT, and "
         "the operator ruled to follow clang"},
        {"__asm__ volatile (\"\" : \"=r\"(lo) : \"r\"(hi) : \"memory\", \"cc\");",
         "all four sections at once"},
        {"__asm__ (\"m %0\" : \"+r\"(lo));",
         "a tied read-write operand"},
        {"__asm__ (\"m %0, %1\" : \"=&r\"(lo) : \"r\"(hi));",
         "an earlyclobber output"},
    };
    for (auto const& r : rows) {
        auto a = analyzeFor("x86_64", wrap(r.stmt));
        ASSERT_TRUE(a.model.has_value());
        EXPECT_FALSE(a.model->hasErrors())
            << r.stmt << "\n  (" << r.why << ")\n" << errorInventory(*a.model);
        EXPECT_EQ(countOf(*a.model, DiagnosticCode::S_InlineAsmExtendedUnsupported), 0u)
            << "S0062 is the RESIDUAL refusal now — it must not fire on a shape "
               "that captured cleanly: " << r.stmt;
    }
}

// The two P1 refusals that are NOT relaxed, kept as pins so "relax the gates"
// cannot quietly become "remove the gates".
TEST(InlineAsmAcceptance, TheTwoConstraintViolationsStayRefused) {
    // A label section without `goto` is ill-formed in gcc, clang AND MSVC, and
    // stays ill-formed after P5 — reporting it as "not yet supported" would be a
    // lie that only gets louder.
    auto labels = analyzeFor("x86_64", wrap("__asm__ (\"\" ::::);"));
    ASSERT_TRUE(labels.model.has_value());
    EXPECT_TRUE(has(*labels.model, DiagnosticCode::S_InlineAsmLabelSectionRequiresGoto))
        << errorInventory(*labels.model);

    auto dup = analyzeFor("x86_64", wrap("__asm__ volatile __volatile__ (\"\");"));
    ASSERT_TRUE(dup.model.has_value());
    EXPECT_TRUE(has(*dup.model, DiagnosticCode::S_InlineAsmDuplicateQualifier))
        << errorInventory(*dup.model);
}

// ★★ THE TARGET-LESS POSTURE, PINNED AS A DELIBERATE CHOICE. The LSP, the FFI
// header parser and every direct-API caller analyze with no target. The
// target-DEPENDENT checks must then be UNASKED — not guessed, and not
// conservatively refused, because refusing would make an editor red every
// `hwtime.h`. The target-INDEPENDENT ones must still fire, which is the half
// that stops this from being "skip the checks".
TEST(InlineAsmAcceptance, WithNoTargetTheMachineQuestionsAreUnaskedAndTheGrammarOnesAreNot) {
    // No target ⇒ no letter table ⇒ `=w` cannot be judged, and is not.
    auto m1 = analyzeShipped("c", {wrap("__asm__ (\"\" : \"=w\"(lo));")});
    EXPECT_FALSE(has(m1, DiagnosticCode::S_InlineAsmConstraintLetterUndeclared));
    EXPECT_FALSE(m1.hasErrors()) << errorInventory(m1);

    // Nor can a clobber be resolved against a register file that is not there.
    auto m2 = analyzeShipped("c", {wrap("__asm__ (\"\" ::: \"nosuchregister\");")});
    EXPECT_FALSE(has(m2, DiagnosticCode::S_InlineAsmClobberUnknown));
    EXPECT_FALSE(m2.hasErrors()) << errorInventory(m2);

    // But the GRAMMAR half needs no processor and still fires.
    auto m3 = analyzeShipped("c", {wrap("__asm__ (\"\" : \"=r,m\"(lo));")});
    EXPECT_TRUE(has(m3, DiagnosticCode::S_InlineAsmConstraintUnsupportedForm))
        << errorInventory(m3);
    auto m4 = analyzeShipped("c", {wrap("__asm__ (\"m %9\" : \"=r\"(lo));")});
    EXPECT_TRUE(has(m4, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(m4);
    auto m5 = analyzeShipped("c", {wrap("__asm__ (\"m %0\");")});
    EXPECT_TRUE(has(m5, DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate))
        << errorInventory(m5);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. THE `asm goto` LABEL REFERENCE — BOTH SPELLINGS ACCEPTED, THE INDEX
//    CHECKED AGAINST THE REAL RULE
// ─────────────────────────────────────────────────────────────────────────────

// ★★★ THIS TEST WAS INVERTED IN P20, AND THE INVERSION IS THE POINT. It used to
// pin a blanket REFUSAL of the positional `%l<N>` — correct while nothing below
// this tier could represent a label reference, and a DIVERGENCE the moment the
// grammar and the lowering could. Both reference compilers accept `%l<N>`, and
// §A.3b of the bar makes one working reference enough to require the behaviour;
// so the question this tier may ask is only whether the INDEX names a label.
// The refusal was not deleted, it was replaced by the check it was standing in
// for, and every arm below states which side of the boundary it is on.
//
// ⚠ THE ASSERTIONS ARE ON CONTENT, NEVER ON A COUNT OF DIAGNOSTICS. "One error
// fired" is satisfied by the wrong error; what has to hold is that the message
// names the CONSTRUCT, states what the written form actually names, and quotes
// the spelling that WOULD have named the author's target.
//
// ⓘ THE EXPECTATIONS BELOW ARE LITERAL BYTES, GUARDED. Every quoted form is one
// the c currently declares, so the guard at the top names the config
// change if a sigil is ever re-spelled — the same reason the minting pin carries
// one. The forms themselves are not owned here: they are what
// `mintTemplateSpellings` produced, read back out of a diagnostic.
TEST(InlineAsmRefusals, APositionalAsmGotoLabelReferenceIsAcceptedAndItsIndexIsCheckedAgainstTheLabelRange) {
    {
        auto probe = buildShippedUnit("c", {wrap("__asm__ (\"\");")});
        auto const& lex = probe->schema().semantics().inlineAsmTemplateLexemes;
        ASSERT_EQ(lex.placeholder,       "%");
        ASSERT_EQ(lex.labelPlaceholder,  "%l");
        ASSERT_EQ(lex.symbolicNameOpen,  "[");
        ASSERT_EQ(lex.symbolicNameClose, "]");
    }

    // The message text of the FIRST diagnostic carrying `code`, or "" when none
    // did — so an arm can assert what a refusal SAYS rather than that one fired.
    auto const messageFor = [](SemanticModel const& m, DiagnosticCode code) {
        for (auto const& d : m.diagnostics().all()) {
            if (d.code == code) return d.actual;
        }
        return std::string{};
    };

    // ── (a) THE ARM THAT WAS RED BEFORE P20. Zero operands, one label ⇒ the
    // label's index is 0, `%l0` names it, and nothing is wrong with this
    // statement at all.
    auto ok0 = analyzeFor("x86_64",
                          wrap("__asm__ goto (\"jmp %l0\" : : : : lbl);"));
    ASSERT_TRUE(ok0.model.has_value());
    EXPECT_FALSE(ok0.model->hasErrors())
        << "a correctly-numbered positional label reference is legal on both "
           "references and must be legal here: " << errorInventory(*ok0.model);

    // ── (b) THE INDEX BASE, WITH OPERANDS PRESENT — the arm that
    // discriminates. With ZERO operands the "labels are numbered after the
    // operands" rule and a labels-only numbering AGREE, so (a) alone cannot
    // catch a wrong base. One input operand moves the label to index 1.
    auto ok1 = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"jmp %l1\" : : \"r\"(hi) : : lbl);"));
    ASSERT_TRUE(ok1.model.has_value());
    EXPECT_FALSE(ok1.model->hasErrors())
        << "GNU 6.47.2.7 numbers the labels AFTER every operand, so with one "
           "operand the first label is index 1: " << errorInventory(*ok1.model);

    // ── (c) A `"+"` OPERAND COUNTS ONCE. It is realized further down the
    // pipeline as an output PLUS a synthesized tied input, so a base taken from
    // that later list would be 3 here and `%l2` would name the wrong thing.
    // The source list has TWO operands, so the label is index 2.
    auto plus = analyzeFor(
        "x86_64",
        wrap("__asm__ goto (\"jmp %l2\" : [o] \"+r\"(lo) : \"r\"(hi) : : lbl);"));
    ASSERT_TRUE(plus.model.has_value());
    EXPECT_FALSE(plus.model->hasErrors())
        << "a `+` operand is ONE entry of the index space the source declares — "
           "counting it twice shifts every label reference: "
        << errorInventory(*plus.model);

    // ── (d) AN INDEX THAT LANDS AMONG THE OPERANDS. The reference calls this
    // "'%l' operand isn't a label"; the message here must say which range the
    // index fell in and quote the operand form that WOULD have named it.
    auto namesOperand = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"jmp %l0\" : : \"r\"(hi) : : lbl);"));
    ASSERT_TRUE(namesOperand.model.has_value());
    ASSERT_TRUE(has(*namesOperand.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*namesOperand.model);
    {
        std::string const msg = messageFor(
            *namesOperand.model, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange);
        EXPECT_NE(msg.find("`%l0`"), std::string::npos) << msg;
        EXPECT_NE(msg.find("names an OPERAND"), std::string::npos) << msg;
        EXPECT_NE(msg.find("Write `%0`"), std::string::npos)
            << "the actionable half is the spelling that names what index 0 IS: "
            << msg;
        EXPECT_NE(msg.find("`%l1`"), std::string::npos)
            << "and the index the author's label actually has: " << msg;
    }

    // ── (e) AN INDEX PAST EVERY LABEL. The reference calls this "operand
    // number out of range", and the message must state what DOES name a label.
    //
    // ★★ THE INVENTORY REPLACED A RENDERED `%l0..%l0` RANGE IN P20, AND IT IS
    // NOT A COSMETIC SWAP. A range has to know where the label numbering starts,
    // so rendering one made this tier state the numbering a SECOND time — one of
    // the five re-derivations the audit found. These are the MINTED spellings
    // read back, which is strictly more (the symbolic form is in them) and
    // cannot disagree with what the lowering binds.
    auto beyond = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"jmp %l7\" : : : : lbl);"));
    ASSERT_TRUE(beyond.model.has_value());
    ASSERT_TRUE(has(*beyond.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*beyond.model);
    {
        std::string const msg =
            messageFor(*beyond.model, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange);
        EXPECT_NE(msg.find("`%l7`"), std::string::npos) << msg;
        EXPECT_NE(msg.find("`%l0`"), std::string::npos)
            << "the form the label actually answers to is the fact that lets the "
               "author renumber: " << msg;
        EXPECT_NE(msg.find("`%l[lbl]`"), std::string::npos)
            << "and the symbolic form, which is minted alongside it and is the "
               "one an edited label list cannot silently renumber: " << msg;
    }

    // ── (f) A PLAIN `%N` THAT LANDS IN THE LABEL RANGE is the SIGIL being
    // wrong, not the number, and saying only "this statement declares 0
    // operands" would send the author to add an operand they do not want.
    auto wrongSigil = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"jmp %0\" : : : : lbl);"));
    ASSERT_TRUE(wrongSigil.model.has_value());
    ASSERT_TRUE(has(*wrongSigil.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*wrongSigil.model);
    {
        std::string const msg = messageFor(
            *wrongSigil.model, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange);
        EXPECT_NE(msg.find("names an `asm goto` LABEL"), std::string::npos) << msg;
        EXPECT_NE(msg.find("`%l0`"), std::string::npos) << msg;
        EXPECT_NE(msg.find("`%l[lbl]`"), std::string::npos)
            << "the named form is the one an edited label list cannot silently "
               "renumber, so it is offered alongside: " << msg;
    }

    // ── (g) THE BRACKETED SPELLING, unchanged: a name in the list clears this
    // tier, a name that is not in it is refused. Without BOTH halves the pin
    // above is satisfied by a compiler that accepts every `%l`.
    auto bracketed = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"jmp %l[lbl]\" : : : : lbl);"));
    ASSERT_TRUE(bracketed.model.has_value());
    EXPECT_FALSE(bracketed.model->hasErrors()) << errorInventory(*bracketed.model);

    auto unknownName = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"jmp %l[nope]\" : : : : lbl);"));
    ASSERT_TRUE(unknownName.model.has_value());
    EXPECT_TRUE(has(*unknownName.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << "a `%l[name]` naming no declared label is the reference's 'undefined "
           "named operand' and must stay refused: "
        << errorInventory(*unknownName.model);

    // ── (g2) AND THE NAMED MIRROR OF (d): a `%l[name]` whose name belongs to
    // an OPERAND is the sigil being wrong, not the name. "that label does not
    // exist" would be true and would send the author to add a label they do not
    // want; the operand form is what they meant to write.
    auto labelSigilOnAnOperandName = analyzeFor(
        "x86_64",
        wrap("__asm__ goto (\"jmp %l[o]\" : [o] \"=r\"(lo) : : : lbl);"));
    ASSERT_TRUE(labelSigilOnAnOperandName.model.has_value());
    ASSERT_TRUE(has(*labelSigilOnAnOperandName.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*labelSigilOnAnOperandName.model);
    {
        std::string const msg =
            messageFor(*labelSigilOnAnOperandName.model,
                       DiagnosticCode::S_InlineAsmPlaceholderOutOfRange);
        EXPECT_NE(msg.find("belongs to an OPERAND"), std::string::npos) << msg;
        EXPECT_NE(msg.find("`%[o]`"), std::string::npos)
            << "the operand form built from the author's own name: " << msg;
    }

    // ── (h) A LABEL LISTED AND NEVER REFERENCED IS LEGAL, and silent, on both
    // references. A tier that "helpfully" reported the unused edge would refuse
    // working programs — the divergence this whole section just came back from.
    auto unreferenced = analyzeFor(
        "x86_64", wrap("__asm__ goto (\"nop\" : : : : lbl);"));
    ASSERT_TRUE(unreferenced.model.has_value());
    EXPECT_FALSE(unreferenced.model->hasErrors())
        << errorInventory(*unreferenced.model);

    // ── (i) AN ORDINARY OPERAND PLACEHOLDER IS UNTOUCHED. `%l` and `%0` share
    // the `%` sigil, so a check keyed too loosely would take the operand form
    // with it.
    auto operand = analyzeFor(
        "x86_64", wrap("__asm__ (\"m %0\" : \"=r\"(lo));"));
    ASSERT_TRUE(operand.model.has_value());
    EXPECT_FALSE(operand.model->hasErrors()) << errorInventory(*operand.model);

    // ── (j) AND THE WHOLE RULE IS TARGET-INDEPENDENT, because label numbering
    // is GNU inline-asm vocabulary rather than a processor fact. arm64 must
    // answer identically in BOTH directions — accepting the valid index and
    // refusing the out-of-range one — or the check has picked up a target
    // dependency it has no business having.
    auto armOk = analyzeFor("arm64", wrap("__asm__ goto (\"b %l0\" : : : : lbl);"));
    ASSERT_TRUE(armOk.model.has_value());
    EXPECT_FALSE(armOk.model->hasErrors()) << errorInventory(*armOk.model);

    auto armBad = analyzeFor("arm64", wrap("__asm__ goto (\"b %l7\" : : : : lbl);"));
    ASSERT_TRUE(armBad.model.has_value());
    EXPECT_TRUE(has(*armBad.model,
                    DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(*armBad.model);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. ONE SYMBOLIC NAME USED TWICE — THE DEFECT THIS ARC OPENED, AND CLOSED
// ─────────────────────────────────────────────────────────────────────────────

// ★★★ THE MEASUREMENT THAT MADE THIS TEST NECESSARY, AND IT IS THE STRONGEST
// KIND: a WRONG ANSWER out of the shipped CLI, not a reasoned hazard.
// ✔MEASURED 2026-08-19, `__asm__("movl %[v], %[out]" : [out] "=r"(r),
// [v] "=r"(d) : [v] "r"(a))` with `a == 20`, compiled by the shipped
// `dsscp` for `x86_64:pe64-x86_64-windows-exec` at `--config=debug`
// AND `--config=release`: rc=0 both times, and the program exited **0** where
// 20 was written. `%[v]` bound the OUTPUT, because `mintTemplateSpellings` gives
// both occurrences the same form and every spelling lookup below this tier is a
// FIRST-MATCH scan over a list whose outputs come first
// (D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND).
//
// ★★ THE CYCLE THAT ADDED `%[name]` BINDING IS THE CYCLE THAT OPENED THIS. The
// same shape used to be refused by name — "'%[v]' names neither a register this
// target declares nor one of the N operand(s) bound" — so making the form bind
// is precisely what turned a fail-loud refusal into a silent wrong answer.
//
// ★★ THE NAME SPACE IS ONE, AND THAT WAS MEASURED RATHER THAN ASSUMED.
// ✔MEASURED 2026-08-19, `-std=gnu17`, EACH SHAPE PROBED SEPARATELY on gcc 13.3.0
// and clang 19.1.1 — all three rejected, with the same message in each
// ("duplicate 'asm' operand name" / "duplicate use of asm operand name" plus a
// note at the first use): two OPERANDS sharing a name, two `asm goto` LABELS
// sharing a name, and an OPERAND and a LABEL sharing a name.
// ⇒ NOT ONE reference accepts any of them, which is what makes acceptance a
// defect under the bidirectional half of the reference rule as §A.3b bounds it.
// ⚠ DSS accepted all three; only the FIRST miscompiled. A fix scoped to the
// operand list alone would have left the other two accepted and would have
// looked complete, because the shape that miscompiles is the shape a probe finds
// first.
TEST(InlineAsmRefusals, ASymbolicNameUsedTwiceIsRefusedInAllThreeCollisions) {
    // ── (a) TWO OPERANDS: the shape that returned the wrong answer. ──
    auto twoOperands = analyzeFor(
        "x86_64",
        wrap("__asm__ (\"m %[v], %[o]\" : [o] \"=r\"(lo), [v] \"=r\"(hi) "
             ": [v] \"r\"(lo));"));
    ASSERT_TRUE(twoOperands.model.has_value());
    EXPECT_TRUE(has(*twoOperands.model,
                    DiagnosticCode::S_InlineAsmDuplicateSymbolicName))
        << errorInventory(*twoOperands.model);
    // ⚠ EXACTLY ONE REPORT, NOT ONE PER PAIR. A name used twice is ONE thing to
    // fix; reporting the second occurrence against every earlier one would grow
    // quadratically on a name used three times.
    EXPECT_EQ(countOf(*twoOperands.model,
                      DiagnosticCode::S_InlineAsmDuplicateSymbolicName), 1u)
        << errorInventory(*twoOperands.model);

    // ── (b) TWO `asm goto` LABELS. Both rows mint the same bracketed form. ──
    auto twoLabels = analyzeShipped(
        "c",
        {"int main(void){ unsigned a = 0; "
         "__asm__ goto (\"jmp %l[hit]\" : : \"r\"(a) : : hit, hit); "
         "return 0; hit: return 1; }\n"});
    EXPECT_TRUE(has(twoLabels, DiagnosticCode::S_InlineAsmDuplicateSymbolicName))
        << errorInventory(twoLabels);

    // ── (c) AN OPERAND AND A LABEL. The DISCRIMINATING arm: `%[v]` and `%l[v]`
    // are DIFFERENT template forms, so nothing here is ambiguous inside DSS and
    // a check written over the operand list alone stays green. It is refused
    // because GNU 6.47.2.3/6.47.2.7 keeps ONE name space and both references
    // reject the pair — measured, not inferred. ──
    auto crossKind = analyzeShipped(
        "c",
        {"int main(void){ unsigned a = 0; "
         "__asm__ goto (\"jmp %l[v]\" : : [v] \"r\"(a) : : v); "
         "return 0; v: return 1; }\n"});
    EXPECT_TRUE(has(crossKind, DiagnosticCode::S_InlineAsmDuplicateSymbolicName))
        << errorInventory(crossKind);

    // ── (d) THE MESSAGE NAMES BOTH OCCURRENCES AND CARRIES THE FIRST AS A
    // `related` LOCATION. Without it the author is told a name is duplicated and
    // left to find the other use — the half clang prints as a note. ──
    bool sawRelated = false;
    for (auto const& d : twoOperands.model->diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InlineAsmDuplicateSymbolicName) continue;
        EXPECT_NE(d.actual.find("duplicate inline-asm symbolic name `v`"),
                  std::string::npos) << d.actual;
        // ★ `[v]` is the SECOND output here, so "output operand 1" is the
        // section-relative position — and that is exactly the discriminating
        // assertion: its `%N` index is 1 as well, so a message that quoted the
        // index would be indistinguishable on this statement. The INPUT half is
        // what separates them: `[v]`'s `%N` is 2 and its section position is 0.
        EXPECT_NE(d.actual.find("output operand 1"), std::string::npos)
            << "the message must say WHERE the name was first used, in the "
               "section the author wrote rather than as a `%N` index: "
            << d.actual;
        EXPECT_NE(d.actual.find("input operand 0"), std::string::npos)
            << d.actual;
        EXPECT_NE(d.actual.find("`%[v]`"), std::string::npos)
            << "the ambiguous form is quoted through the language's own declared "
               "lexemes, never as a hard-coded sigil: " << d.actual;
        EXPECT_NE(d.actual.find(
                      "D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND"),
                  std::string::npos) << d.actual;
        sawRelated = !d.related.empty();
    }
    EXPECT_TRUE(sawRelated)
        << "the first use must ride as a `related` location — the structured "
           "form of clang's note";

    // ── (e) THE CROSS-KIND MESSAGE SAYS SOMETHING DIFFERENT, because the
    // consequence is different: nothing is ambiguous, the refusal is owed to the
    // references. A single shared sentence would tell an author their two
    // distinct forms collide, which is false. ──
    for (auto const& d : crossKind.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_InlineAsmDuplicateSymbolicName) continue;
        EXPECT_NE(d.actual.find("ONE name space"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("`%[v]`"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("`%l[v]`"), std::string::npos) << d.actual;
    }

    // ── (f) THE CONTROLS. Every one of these is a program both references
    // COMPILE, and without them the pin is satisfied by refusing every named
    // operand — the divergence in the other direction. ──
    // Two DISTINCT operand names.
    auto distinct = analyzeFor(
        "x86_64",
        wrap("__asm__ (\"m %[v], %[o]\" : [o] \"=r\"(lo) : [v] \"r\"(hi));"));
    ASSERT_TRUE(distinct.model.has_value());
    EXPECT_FALSE(distinct.model->hasErrors()) << errorInventory(*distinct.model);

    // ONE name used ONCE on an operand ALSO referenced positionally — an operand
    // answering to TWO forms is the normal case, not a duplicate.
    auto twoFormsOneOperand = analyzeFor(
        "x86_64", wrap("__asm__ (\"m %0, %[o]\" : [o] \"=r\"(lo));"));
    ASSERT_TRUE(twoFormsOneOperand.model.has_value());
    EXPECT_FALSE(twoFormsOneOperand.model->hasErrors())
        << errorInventory(*twoFormsOneOperand.model);

    // TWO ANONYMOUS operands. Their names are both empty, and folding empty
    // names together would refuse the commonest shape in the language.
    auto anonymous = analyzeFor(
        "x86_64", wrap("__asm__ (\"m %0, %1\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(anonymous.model.has_value());
    EXPECT_FALSE(anonymous.model->hasErrors()) << errorInventory(*anonymous.model);

    // A NAMED operand beside a DIFFERENTLY-named label on the same statement.
    auto namedBoth = analyzeShipped(
        "c",
        {"int main(void){ unsigned a = 0; "
         "__asm__ goto (\"jmp %l[hit]\" : : [v] \"r\"(a) : : hit); "
         "return 0; hit: return 1; }\n"});
    EXPECT_FALSE(namedBoth.hasErrors()) << errorInventory(namedBoth);

    // ── (g) TARGET-INDEPENDENT. A repeated name is host-language vocabulary,
    // not a processor fact, so arm64 must answer identically — and the check
    // must fire with NO target at all, which is the LSP / FFI-header-parser
    // path. ──
    auto arm = analyzeFor(
        "arm64",
        wrap("__asm__ (\"m %[v], %[o]\" : [o] \"=r\"(lo), [v] \"=r\"(hi) "
             ": [v] \"r\"(lo));"));
    ASSERT_TRUE(arm.model.has_value());
    EXPECT_TRUE(has(*arm.model, DiagnosticCode::S_InlineAsmDuplicateSymbolicName))
        << errorInventory(*arm.model);
    auto noTarget = analyzeShipped(
        "c",
        {wrap("__asm__ (\"m %[v], %[o]\" : [o] \"=r\"(lo), [v] \"=r\"(hi) "
              ": [v] \"r\"(lo));")});
    EXPECT_TRUE(has(noTarget, DiagnosticCode::S_InlineAsmDuplicateSymbolicName))
        << errorInventory(noTarget);
}
