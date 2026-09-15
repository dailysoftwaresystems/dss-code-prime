// ═══════════════════════════════════════════════════════════════════════════
//  WHERE A C23 `[[…]]` ATTRIBUTE SEQUENCE MAY SIT IN A DECLARATION
//  (subject: the declaration-specifier runs in src/dss-config/sources/c.lang.json)
// ═══════════════════════════════════════════════════════════════════════════
//
// `static [[maybe_unused]] int m = 1;` compiled at rc 0 with ZERO bytes on
// stderr. ✔MEASURED through the shipped CLI at the pre-change HEAD, and it was
// not one position but TEN: file scope after `static` / after `const` / after
// `static const` / after `extern`, block scope after `static` and after
// `const`, both sides of a `typedef`, a parameter after `register` and after
// `const`, and both sides of an inferred `auto`. The C23 `[[…]]` spelling was an
// ordinary member of five different specifier-run alternatives
// (`singleDeclSpecifier`, `localDeclSpecifier`, `topLevelAutoSpecifier`,
// `typedefDeclSpecifiers`, and `paramDeclSpecifier` via `compositeAttr`), so it
// could appear anywhere one could.
//
// ★★★ C23 6.7 PUTS IT IN EXACTLY TWO PLACES, AND NEITHER IS "ANYWHERE":
//     declaration:
//         declaration-specifiers init-declarator-list_opt ;
//         attribute-specifier-sequence declaration-specifiers init-declarator-list ;
//     declaration-specifiers:
//         declaration-specifier attribute-specifier-sequence_opt
//         declaration-specifier declaration-specifiers
// So the sequence may LEAD the whole declaration, or FOLLOW the complete
// specifier run — and the second production is what forbids the middle: an
// attribute-specifier-sequence inside declaration-specifiers TERMINATES them, so
// nothing that is itself a declaration specifier may come after it.
//
// ★★ THE REFERENCES AGREE, UNANIMOUSLY, IN BOTH DIRECTIONS. ✔MEASURED
// 2026-09-09, each probed SEPARATELY on its own translation unit — gcc 13.3.0
// `-std=c2x`, clang 18.1.3 `-std=c23`, MSVC 19.51.36257 `/std:clatest`:
//   • ELEVEN mid-run spellings (the ten above plus `auto [[…]] static g = 1;`):
//     ALL THREE REFUSE, every one. gcc/clang say `expected identifier or '('`
//     / `a type specifier is required`, MSVC `C2059 syntax error: 'type'` or
//     `'attribute specifier'`.
//   • TEN leading spellings — file, block, typedef, parameter (with and without
//     `register`), qualifier-led, `auto`-led, `static auto`, a struct member,
//     and two sequences in a row: ALL THREE ACCEPT, every one.
//   • FIVE GNU `__attribute__` mid-run spellings: gcc AND clang ACCEPT every
//     one (MSVC ABSTAINS — it has no such syntax at all, and an abstention is
//     recorded as an abstention). ⚠ SO THE FIX MUST TOUCH ONLY THE `[[…]]`
//     SPELLING. Narrowing `attrSpec` alongside `stdAttr` would have moved DSS
//     BELOW the union on five programs two references compile.
//   • The END-of-specifiers position (`static int [[maybe_unused]] m = 1;`) —
//     gcc ACCEPTS (warning: attribute ignored), clang REFUSES semantically
//     ("cannot be applied to types"), MSVC REFUSES syntactically.
//     ~~ This bullet used to conclude "A split on ACCEPT-vs-REFUSE, so the
//     disjunction requires DSS to PARSE it — a PRE-EXISTING Direction-A gap".
//     P66 lane `ca` MEASURED that and it is NOT a gap: the three reference
//     verdicts above are right and the conclusion drawn from them is wrong. ~~
//     C23 6.7p9 makes a sequence TERMINATING the declaration specifiers appertain
//     to the **TYPE**, and 6.7.13.4 / 6.7.13.5 constrain `maybe_unused` and
//     `deprecated` to a declaration of a structure, union, typedef name, object,
//     member, function, enumeration or enumerator — never a type. Every
//     standard attribute in that slot is therefore a CONSTRAINT VIOLATION which a
//     conforming implementation SHALL diagnose, and all three references do (gcc
//     with -Wattributes, clang and MSVC with an error). DSS's positioned parse
//     error is a fourth diagnosis, INSIDE the union. ✔MEASURED 2026-09-09,
//     gcc's acceptance also DROPS the effect: `static int [[maybe_unused]] m = 1;`
//     still warns `'m' defined but not used` where the LEADING spelling is
//     silent, and `static int [[deprecated]] m = 1;` warns nothing at a use.
//     ⚠ The `typedef` spelling of that position (`typedef int [[…]] T;`) IS
//     accepted by DSS, via `typedefAttrRun`, and is pinned below so this change
//     cannot cost it — but DSS also CONFERS from it (`S_DeprecatedSymbolUsed`
//     at a use of `T`) where gcc IGNORES it and clang and MSVC refuse it, so that
//     acceptance is a separate ABOVE-the-union defect, anchored by P66 lane `ca`.
//
// ── THE SHAPE, AND WHY IT IS THIS SHAPE ────────────────────────────────────
// Each run became `{alt: [<a leading [[…]] run>, <specifier>, …]}` followed by a
// repeat that admits only specifiers, and `stdAttr` left the specifier
// alternatives. Three properties were required and each is measured:
//   ★ THE LEADING RUN IS AN **INLINE** `{sequence: [stdAttr, {repeat: stdAttr}]}`
//     AND NOT A NAMED RULE. An inline sequence materialises no CST node — node
//     identity is keyed on `RuleId` and only named shapes have one — so the
//     children of a specifier-run node are the SAME `stdAttr` / specifier nodes
//     at the SAME depth as before, for every input that already parsed. Nothing
//     that reads a specifier prefix (the attribute scan, the linkage scan, the
//     qualifier scan, `decl_prefix_strip.hpp`'s positional counting) sees a new
//     level, and no `semantics.declarations` index moved.
//   ★ FIRST-SET DISJOINTNESS IS WHAT MAKES IT LEGAL AT ALL: `FIRST(stdAttr)` is
//     `{BracketOpen}` and, once `stdAttr` leaves the specifier alternatives, no
//     other branch of any of these alts can begin with it. Had one, the loader
//     would refuse the whole document — `C_AmbiguousAlternatives … alt branches
//     share FIRST token`.
//   ★ A HEAD-LESS ROW NEEDS THE **TRAILING** POSITION TOO, and it cannot live in
//     a run a HEADED row shares. `auto [[maybe_unused]] g = 1;` is the
//     end-of-specifiers position for an inferred declaration (there is no type
//     specifier, so `auto` IS the complete run) and gcc accepts it. But a
//     trailing slot inside `localDeclSpecifiers` — shared with `varDecl` — would
//     re-admit `static [[…]] int m;`, since a headed row's type sits OUTSIDE the
//     run. So the two head-less block-scope rows got their own
//     `localAutoSpecifiers` (leading AND trailing), `topLevelAutoSpecifiers`
//     grew both slots in place (it is head-less and unshared), and every headed
//     row's run took the leading slot alone.
//
// ── RED-ON-DISABLE, REMOVE DIRECTION ───────────────────────────────────────
// ⚠ THE MUTANT IS THE CONFIG DOCUMENT AND **NO OBJECT md5 IS INVOLVED**:
// `c.lang.json` is copied into the build tree's `dss-config-snapshot` at ctest
// RUN time and never at build time (cmake/DssConfigSnapshot.cmake), so nothing
// recompiles and no binary moves. The moved artefact is the CONFIG FILE's own
// md5, and it must move and RETURN.
//   put `"stdAttr"` back into `singleDeclSpecifier`'s `alt` in
//   src/dss-config/sources/c.lang.json → every `…IsRefused` arm below that
//   involves a FILE-SCOPE declaration goes red while the leading-position and
//   GNU controls stay green. Repeat per run (`localDeclSpecifier`,
//   `topLevelAutoSpecifier`, `typedefDeclSpecifiers`, `paramDeclSpecifier`) for
//   the disjoint red set of each. The transcripts are in the lane report.

#include "analysis/syntactic/parser.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"

#include "tokenizer/tokenizer.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

using namespace dss;

namespace {

struct Parsed {
    std::shared_ptr<SourceBuffer> src;
    Tree                          tree;
};

} // namespace

class DeclAttributeSequencePosition : public ::testing::Test {
  protected:
    static std::shared_ptr<GrammarSchema const> schema;

    static void SetUpTestSuite() {
        auto loaded = GrammarSchema::loadShipped("c");
        if (loaded.has_value()) schema = *loaded;
    }
    static void TearDownTestSuite() { schema.reset(); }

    void SetUp() override {
        ASSERT_NE(schema, nullptr) << "GrammarSchema::loadShipped(\"c\") failed";
    }

    [[nodiscard]] static Parsed parseC(std::string source) {
        auto src = SourceBuffer::fromString(std::move(source), "<attr-pos>");
        Tokenizer tk{src, schema, DiagnosticBudget::libraryDefault()};
        auto [stream, _] = std::move(tk).tokenize();
        Parser p{src, schema, std::move(stream), DiagnosticBudget::libraryDefault()};
        auto result = std::move(p).parse();
        return Parsed{std::move(src), std::move(result.tree)};
    }

    static void expectAccepted(char const* source) {
        auto p = parseC(source);
        EXPECT_FALSE(p.tree.diagnostics().hasErrors())
            << "all three references ACCEPT this: " << source;
    }

    static void expectRefused(char const* source) {
        auto p = parseC(source);
        EXPECT_TRUE(p.tree.diagnostics().hasErrors())
            << "gcc, clang and MSVC all three REFUSE this: " << source;
    }
};

std::shared_ptr<GrammarSchema const> DeclAttributeSequencePosition::schema;

// ── THE CONTROLS FIRST: the LEADING position, in every declaration kind ────
//
// Pinned before the refusals, and pinned wide. A tightening that also refuses
// the legal leading position would be a far worse defect than the one being
// fixed, and every arm below is a program all three references accept.

TEST_F(DeclAttributeSequencePosition, LeadingSequenceIsAcceptedInEveryDeclarationKind) {
    expectAccepted("[[maybe_unused]] int m = 1;\n");
    expectAccepted("[[maybe_unused]] static int m = 1;\n");
    expectAccepted("[[maybe_unused]] const int m = 1;\n");
    expectAccepted("[[maybe_unused]] extern int m;\n");
    expectAccepted("[[maybe_unused]] typedef int T;\n");
    expectAccepted("[[maybe_unused]] static int g(void){ return 1; }\n");
    expectAccepted("int h([[maybe_unused]] int p);\n");
    expectAccepted("int h([[maybe_unused]] register int p);\n");
    expectAccepted("int f(void){ [[maybe_unused]] static int m = 1; return m; }\n");
    expectAccepted("int f(void){ [[maybe_unused]] const int m = 1; return m; }\n");
}

// C23's attribute-specifier-SEQUENCE is a sequence: more than one `[[…]]` may
// lead. The run has to be a RUN, not a single slot.
TEST_F(DeclAttributeSequencePosition, TwoLeadingSequencesAreAccepted) {
    expectAccepted("[[maybe_unused]] [[deprecated]] static int m = 1;\n");
    expectAccepted("int f(void){ [[maybe_unused]] [[deprecated]] int m = 1; return m; }\n");
}

// The head-less inferred rows, both scopes and both legal positions. The
// TRAILING arms are the ones a leading-only fix would have broken: gcc 13.3.0
// accepts `auto [[maybe_unused]] g = 1;` (it is the end-of-specifiers position
// for a declaration that has no type specifier), and DSS accepted it at HEAD.
TEST_F(DeclAttributeSequencePosition, InferredDeclarationsKeepBothLegalPositions) {
    expectAccepted("[[maybe_unused]] auto g = 1;\n");
    expectAccepted("[[maybe_unused]] static auto g = 1;\n");
    expectAccepted("auto [[maybe_unused]] g = 1;\n");
    expectAccepted("static auto [[maybe_unused]] g = 1;\n");
    expectAccepted("int f(void){ [[maybe_unused]] auto g = 1; return g; }\n");
    expectAccepted("int f(void){ auto [[maybe_unused]] g = 1; return g; }\n");
}

// The two positions this change does not touch and must not cost: the
// after-the-type slot a `typedef` has (`typedefAttrRun`) and the
// after-the-declarator slot every declaration has (`initDeclarator`'s run).
// Both are accepted by gcc and clang.
TEST_F(DeclAttributeSequencePosition, ThePostHeadAndPostDeclaratorSlotsAreUntouched) {
    expectAccepted("typedef int [[maybe_unused]] T;\n");
    expectAccepted("int m [[maybe_unused]] = 1;\n");
    expectAccepted("auto g [[maybe_unused]] = 1;\n");
}

// ⚠ THE GNU SPELLING IS A DIFFERENT SURFACE AND KEEPS ITS FREEDOM. gcc AND
// clang accept `__attribute__((…))` between two declaration specifiers in every
// one of these positions (MSVC abstains — no such syntax). A fix that narrowed
// `attrSpec` alongside `stdAttr` would have put DSS below the union here.
TEST_F(DeclAttributeSequencePosition, TheGnuSpellingStaysLegalMidRun) {
    expectAccepted("static __attribute__((unused)) int m = 1;\n");
    expectAccepted("typedef __attribute__((unused)) int T;\n");
    expectAccepted("int h(register __attribute__((unused)) int p);\n");
    expectAccepted("static __attribute__((unused)) auto g = 1;\n");
    expectAccepted("int f(void){ static __attribute__((unused)) int m = 1; return m; }\n");
}

// ── THE DEFECT: a `[[…]]` sequence BETWEEN two declaration specifiers ──────

TEST_F(DeclAttributeSequencePosition, MidRunSequenceIsRefusedAtFileScope) {
    expectRefused("static [[maybe_unused]] int m = 1;\n");
    expectRefused("const [[maybe_unused]] int m = 1;\n");
    expectRefused("static const [[maybe_unused]] int m = 1;\n");
    expectRefused("extern [[maybe_unused]] int m;\n");
    expectRefused("static [[maybe_unused]] int g(void){ return 1; }\n");
}

TEST_F(DeclAttributeSequencePosition, MidRunSequenceIsRefusedAtBlockScope) {
    expectRefused("int f(void){ static [[maybe_unused]] int m = 1; return m; }\n");
    expectRefused("int f(void){ const [[maybe_unused]] int m = 1; return m; }\n");
}

TEST_F(DeclAttributeSequencePosition, MidRunSequenceIsRefusedOnEitherSideOfTypedef) {
    expectRefused("typedef [[maybe_unused]] int T;\n");
    expectRefused("const [[maybe_unused]] typedef int T;\n");
}

TEST_F(DeclAttributeSequencePosition, MidRunSequenceIsRefusedInAParameter) {
    expectRefused("int h(register [[maybe_unused]] int p);\n");
    expectRefused("int h(const [[maybe_unused]] int *p);\n");
}

TEST_F(DeclAttributeSequencePosition, MidRunSequenceIsRefusedAroundAnInferredAuto) {
    expectRefused("static [[maybe_unused]] auto g = 1;\n");
    expectRefused("auto [[maybe_unused]] static g = 1;\n");
    expectRefused("int f(void){ static [[maybe_unused]] auto g = 1; return g; }\n");
    expectRefused("int f(void){ auto [[maybe_unused]] static g = 1; return g; }\n");
}
