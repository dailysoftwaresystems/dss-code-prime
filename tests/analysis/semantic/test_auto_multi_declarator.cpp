// ═══════════════════════════════════════════════════════════════════════════
//  ONE INFERRED DECLARATION, MANY DECLARATORS, ONE DEDUCED TYPE
//  (subject: resolveAutoInferredDeclaration in
//   src/analysis/semantic/semantic_analyzer.cpp)
// ═══════════════════════════════════════════════════════════════════════════
//
// `auto a = 1, b = 2;` was REFUSED by DSS — `error[S_AutoRequiresSingleDeclarator]`
// at 1:29, at file scope and at block scope alike — on a program clang compiles
// and RUNS. That put DSS BELOW the union.
//
// ★★★ THE REFUSAL'S PREMISE WAS THE STANDARD, AND THE STANDARD REFUTES IT. The
// arm cited "C23 6.7.9p2 … a single declarator". Read at the primary source
// (N3220, the C23 final working draft, via `pdftotext`):
//   • type inference is §6.7.10, NOT 6.7.9 — 6.7.9 is Type DEFINITIONS
//     (typedef). Every in-tree citation of "C23 6.7.9" for inference is off by
//     one section, and this file is where that is written down.
//   • §6.7.10's ONE Constraint is "A declaration for which the type is inferred
//     shall contain the storage-class specifier auto". The declarator COUNT is
//     not constrained anywhere in the clause.
//   • Annex J.2(78) lists "A declaration for which a type is inferred contains
//     no or more than one declarators (6.7.10)" as UNDEFINED BEHAVIOUR — so no
//     diagnostic is required and accepting and refusing are BOTH conforming.
//   • Annex J.5.12 (Common extensions) says a declaration for which a type is
//     inferred "may … have more than one declarator" — the standard NAMING this
//     exact form as a sanctioned extension.
//   • footnote 164 recommends that an implementation accepting a wider form
//     "follow the syntax and semantics of the corresponding feature in ISO/IEC
//     14882" — C++ `auto`: ONE type deduced for the whole declaration, shared by
//     every declarator, hard error when two declarators disagree.
//
// ★★ THE REFERENCES, ✔MEASURED 2026-09-09, each probed SEPARATELY on its OWN
// translation unit:
//   clang 18.1.3 `-std=c23`  ACCEPTS, at file AND block scope, and SILENTLY at
//     `-Wall -Wextra -pedantic`. It is not shy about the neighbours: the sibling
//     `auto *a = &x, b = 2;` draws "type inference of a declaration other than a
//     plain identifier … is a Clang extension [-Wauto-decl-extensions]". About
//     the MULTI-DECLARATOR form it says nothing at all.
//     ✔ AND THE ACCEPTANCE WAS PROVED TO CARRY MEANING BY A RUN, not by reading:
//     a linked binary over `auto fa = 10, fb = 20;` (file scope) and
//     `auto a = 1, b = 2;` (block scope) printed `a=1 b=2 fa=10 fb=20`,
//     `sizeof` 4 for both, `_Generic` `int` for both, compiled `int *pa = &a;`
//     and `int *pb = &b;` clean, and EXITED 33.
//     It REFUSES `auto a = 1, b = 2.5;` — "'auto' deduced as 'int' in
//     declaration of 'a' and deduced as 'double' in declaration of 'b'" — which
//     is what PROVES the type is shared rather than deduced per declarator.
//   gcc 13.3.0 `-std=c2x`  REFUSES ("'auto' may only be used with a single
//     declarator"), file and block scope alike.
//   MSVC 19.51.36257 `/std:clatest`  ABSTAINS rather than voting. It reads the
//     SPELLING as the C89 storage class over an implicit `int`: it accepts
//     `auto a = 1, b = 2.5;` SILENTLY (b becomes int, a truncation), and
//     `auto a = &x, b = &y; … *a` fails C2100 "you cannot dereference an operand
//     of type 'int'". An acceptance that drops meaning casts no vote, so MSVC is
//     answering a different question and is recorded as an ABSTENTION.
// ⇒ ONE WORKING accepting reference makes the behaviour REQUIRED, and the split
//   is ACCEPT-vs-REFUSE (clang is the only reference that gives this form a
//   meaning AT ALL, so there is no meaning fork to pause on).
//
// ★ WHY NO SEPARATE WRITE SITE WAS NEEDED, which is the whole reason this is a
// small change. `resolveAutoInferredDeclaration` returns ONE type that its
// caller hands to the ORDINARY declarator fold as the head type — the same fold
// a written head drives. So "one deduced type applied to every declarator" is
// what the existing path already does; what changed is that the arm stopped
// refusing before reaching it, and gained the agreement check.
//
// ── WHAT EACH GROUP PINS ───────────────────────────────────────────────────
//   * the form is ACCEPTED at block AND file scope, and BOTH symbols are typed
//     — `hasErrors() == false` alone would stay green for an implementation
//     that accepted the declaration and typed only the first declarator;
//   * the type is SHARED and correct through decay (`auto p = a, q = a;` over an
//     array gives two `int *`), so acceptance is not "typed as something";
//   * a DISAGREEMENT is refused by NAME at the SECOND declarator — the arm that
//     separates "one type for all" from "a type each";
//   * every OTHER gate of the inference row still fires through a multi-
//     declarator list, at the declarator that violates it (a plain-identifier
//     violation in declarator 2, a missing initializer in declarator 2) — the
//     ladder runs per declarator rather than on the first alone;
//   * the SINGLE-declarator behaviour is untouched.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// The mutant is ENGINE SOURCE, so SOURCE and OBJECT md5 must both move and
// return: in `resolveAutoInferredDeclaration`, put the count gate back —
// replace `if (declarators.empty())` with `if (declarators.size() != 1)` and
// restore the "exactly one is required" text. Every accepting arm below goes
// red with `S_AutoRequiresSingleDeclarator`; the single-declarator control arm
// stays green. The transcript is in the lane report.

#include "analysis/semantic/semantic_model.hpp"
#include "core/types/parse_diagnostic.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SymbolRecord const*
symbolNamed(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return &model.symbols()[i];
    }
    return nullptr;
}

} // namespace

// ── ACCEPTED, AND BOTH DECLARATORS TYPED ───────────────────────────────────

TEST(AutoMultiDeclarator, BlockScopePairIsAcceptedAndBothSymbolsAreTyped) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1, b = 2; return a + b; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* a = symbolNamed(model, "a");
    auto const* b = symbolNamed(model, "b");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->type.valid()) << "the first declarator must be typed";
    ASSERT_TRUE(b->type.valid())
        << "the SECOND declarator must be typed too — a fix that types only the "
           "first passes a no-errors check while leaving b to the Pass-2 backfill";
    EXPECT_EQ(a->type.v, b->type.v) << "ONE deduced type (C23 6.7.10 fn.164)";
    auto const& in = model.lattice().interner();
    EXPECT_EQ(in.kind(a->type), TypeKind::I32);
}

TEST(AutoMultiDeclarator, FileScopePairIsAcceptedAndBothSymbolsAreTyped) {
    auto model = analyzeShipped("c", {
        "auto fa = 10, fb = 20;\n"
        "int main(void) { return fa + fb; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* fa = symbolNamed(model, "fa");
    auto const* fb = symbolNamed(model, "fb");
    ASSERT_NE(fa, nullptr);
    ASSERT_NE(fb, nullptr);
    ASSERT_TRUE(fa->type.valid());
    ASSERT_TRUE(fb->type.valid());
    EXPECT_EQ(fa->type.v, fb->type.v);
}

TEST(AutoMultiDeclarator, ThreeDeclaratorsShareTheOneDeducedType) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1, b = 2, c = 3; return a + b + c; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* a = symbolNamed(model, "a");
    auto const* c = symbolNamed(model, "c");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(c, nullptr);
    ASSERT_TRUE(a->type.valid());
    ASSERT_TRUE(c->type.valid());
    EXPECT_EQ(a->type.v, c->type.v);
}

// The shared type is the NORMALIZED one — C 6.3.2.1p3 array-to-pointer decay
// runs per declarator and both land on the same `int *`. An implementation that
// deduced the first declarator and then simply copied the id would pass the
// equality above without ever resolving the second initializer; this arm's value
// is that the SECOND initializer is a different expression that must decay too.
TEST(AutoMultiDeclarator, BothDeclaratorsDecayAndAgreeOnThePointerType) {
    auto model = analyzeShipped("c", {
        "int main(void) {\n"
        "    int arr[3]; int other[3];\n"
        "    auto p = arr, q = other;\n"
        "    p[0] = 1; q[0] = 2;\n"
        "    return p[0] + q[0];\n"
        "}\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* p = symbolNamed(model, "p");
    auto const* q = symbolNamed(model, "q");
    ASSERT_NE(p, nullptr);
    ASSERT_NE(q, nullptr);
    ASSERT_TRUE(p->type.valid());
    ASSERT_TRUE(q->type.valid());
    auto const& in = model.lattice().interner();
    ASSERT_EQ(in.kind(p->type), TypeKind::Ptr);
    EXPECT_EQ(p->type.v, q->type.v);
}

// ── THE ONE REMAINING CONSTRAINT: THE DECLARATORS MUST AGREE ───────────────

TEST(AutoMultiDeclarator, DisagreeingDeductionsAreRefusedByName) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1, b = 2.5; return (int)b; }\n",
    });
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AutoDeclaratorsInferDifferentTypes),
              1u)
        << "clang 18.1.3 refuses this exact form and names both deductions";
    EXPECT_FALSE(hasCode(model.diagnostics(),
                         DiagnosticCode::S_AutoRequiresSingleDeclarator))
        << "the retired constraint must not fire for a multi-declarator list";
}

// The message must name BOTH deductions and BOTH declarators. ⓘ The types are
// spelled in the interner's KIND vocabulary (`I32`, `F64`) rather than in C
// words, and that is measured rather than settled for: `TypeSpecifierRule`
// carries a `tokenNames` field commented "source spellings, for diagnostics"
// and it in fact holds TOKEN-KIND names — rendering through it produced
// `deduces 'IntKeyword' … and 'DoubleKeyword'`. Kind spellings are also what
// every parser refusal here already prints.
TEST(AutoMultiDeclarator, TheDisagreementDiagnosticNamesBothTypesAndBothNames) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1, b = 2.5; return (int)b; }\n",
    });
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AutoDeclaratorsInferDifferentTypes)
            continue;
        found = true;
        EXPECT_NE(d.actual.find("I32"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("F64"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("a = 1"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("b = 2.5"), std::string::npos) << d.actual;
    }
    EXPECT_TRUE(found);
}

// Two DIFFERENT pointer types must not both print as `Ptr`: the pointee is
// rendered through, so the message discriminates.
TEST(AutoMultiDeclarator, TwoDifferentPointerDeductionsAreNamedDistinctly) {
    auto model = analyzeShipped("c", {
        "int main(void) { int x = 1; double d = 2.0; auto a = &x, b = &d;\n"
        "                 return *a; }\n",
    });
    bool found = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != DiagnosticCode::S_AutoDeclaratorsInferDifferentTypes)
            continue;
        found = true;
        EXPECT_NE(d.actual.find("I32 *"), std::string::npos) << d.actual;
        EXPECT_NE(d.actual.find("F64 *"), std::string::npos) << d.actual;
    }
    EXPECT_TRUE(found);
}

// ── THE LADDER RUNS PER DECLARATOR, NOT ON THE FIRST ONE ───────────────────
//
// Each of these puts the violation in the SECOND declarator. Under a fix that
// inferred from declarator 1 and applied the result to the rest, all three would
// compile clean and silently type an object from an initializer nobody looked at.

TEST(AutoMultiDeclarator, APlainIdentifierViolationInASiblingStillFires) {
    auto model = analyzeShipped("c", {
        "int main(void) { int x = 1; auto a = 1, *p = &x; return a + *p; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresPlainIdentifier));
}

TEST(AutoMultiDeclarator, AMissingInitializerInASiblingStillFires) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1, b; return a; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresInitializer));
}

TEST(AutoMultiDeclarator, AVoidInitializerInASiblingStillFires) {
    auto model = analyzeShipped("c", {
        "void nothing(void) {}\n"
        "int main(void) { auto a = 1, b = nothing(); return a; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid));
}

// ── CONTROLS: the single-declarator behaviour is untouched ─────────────────

TEST(AutoMultiDeclarator, SingleDeclaratorIsUnchanged) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto a = 1; return a; }\n",
    });
    EXPECT_FALSE(model.hasErrors());
    auto const* a = symbolNamed(model, "a");
    ASSERT_NE(a, nullptr);
    ASSERT_TRUE(a->type.valid());
    EXPECT_EQ(model.lattice().interner().kind(a->type), TypeKind::I32);
}

TEST(AutoMultiDeclarator, ASingleDerivedDeclaratorIsStillRefused) {
    auto model = analyzeShipped("c", {
        "int main(void) { auto *p = 0; return 0; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_AutoRequiresPlainIdentifier));
}

// The C89 implicit-int shapes that PARSE into the head-less rule must stay
// errors through a multi-declarator list too: the ★C1 required-specifier gate
// runs BEFORE the declarator set is even collected, so a list cannot smuggle
// one past it.
TEST(AutoMultiDeclarator, ImplicitIntWithTwoDeclaratorsIsStillRefused) {
    auto model = analyzeShipped("c", {
        "int main(void) { static x = 5, y = 6; return x + y; }\n",
    });
    EXPECT_TRUE(hasCode(model.diagnostics(),
                        DiagnosticCode::S_AutoInferenceInvalid)
                || model.hasErrors())
        << "a declaration with no type specifier is not implicit-int in C23";
}
