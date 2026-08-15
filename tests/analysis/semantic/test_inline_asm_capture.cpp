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

// Analyze one c-subset source WITH a target in scope, so the target-dependent
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
    a.cu     = buildShippedUnit("c-subset", {std::move(src)});
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
// shipped c-subset + asm grammars: it does NOT — the prefix is three bare
// tokens, and the operand has EXACTLY TWO Internal children, constraint first
// and value expression last.
//
// If a grammar edit ever mints a node for the prefix, this pin reds FIRST and
// names the shape, instead of the capture silently promoting the prefix node to
// a constraint.
TEST(InlineAsmCapture, AnOperandHasExactlyTwoCompositeChildrenConstraintThenValue) {
    auto cu = buildShippedUnit(
        "c-subset", {wrap("__asm__ (\"m %0,%1\" : [o] \"=r\"(lo) : \"r\"(hi));")});
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
        "c-subset", {wrap("__asm__ (\"m %0,%1\" : [out] \"=r\"(lo) : \"r\"(hi));")});
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
            auto const f = gatherInlineAsmFacts(t, n, ia, schema.semantics().identifierToken,
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
    // `w` is arm64's vector class and is UNDECLARED on x86_64.
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

// S0067 — the width-view modifier, with the plain placeholder as its control.
TEST(InlineAsmRefusals, AnOperandWidthModifierIsRefusedRatherThanWidened) {
    auto bad = analyzeFor("arm64", wrap("__asm__ (\"mov %w0, %w1\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(bad.model.has_value());
    EXPECT_TRUE(has(*bad.model, DiagnosticCode::S_InlineAsmOperandModifierUnsupported))
        << errorInventory(*bad.model);

    // POSITIVE CONTROL — the same template WITHOUT the modifier letters.
    auto good = analyzeFor("arm64", wrap("__asm__ (\"mov %0, %1\" : \"=r\"(lo) : \"r\"(hi));"));
    ASSERT_TRUE(good.model.has_value());
    EXPECT_FALSE(good.model->hasErrors()) << errorInventory(*good.model);
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
    auto m1 = analyzeShipped("c-subset", {wrap("__asm__ (\"\" : \"=w\"(lo));")});
    EXPECT_FALSE(has(m1, DiagnosticCode::S_InlineAsmConstraintLetterUndeclared));
    EXPECT_FALSE(m1.hasErrors()) << errorInventory(m1);

    // Nor can a clobber be resolved against a register file that is not there.
    auto m2 = analyzeShipped("c-subset", {wrap("__asm__ (\"\" ::: \"nosuchregister\");")});
    EXPECT_FALSE(has(m2, DiagnosticCode::S_InlineAsmClobberUnknown));
    EXPECT_FALSE(m2.hasErrors()) << errorInventory(m2);

    // But the GRAMMAR half needs no processor and still fires.
    auto m3 = analyzeShipped("c-subset", {wrap("__asm__ (\"\" : \"=r,m\"(lo));")});
    EXPECT_TRUE(has(m3, DiagnosticCode::S_InlineAsmConstraintUnsupportedForm))
        << errorInventory(m3);
    auto m4 = analyzeShipped("c-subset", {wrap("__asm__ (\"m %9\" : \"=r\"(lo));")});
    EXPECT_TRUE(has(m4, DiagnosticCode::S_InlineAsmPlaceholderOutOfRange))
        << errorInventory(m4);
    auto m5 = analyzeShipped("c-subset", {wrap("__asm__ (\"m %0\");")});
    EXPECT_TRUE(has(m5, DiagnosticCode::S_InlineAsmPlaceholderInBasicTemplate))
        << errorInventory(m5);
}
