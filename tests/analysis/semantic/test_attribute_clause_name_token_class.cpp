// P32 lane A — [[D-C-ATTRIBUTE-CLAUSE-NAME-ADMITS-ONLY-IDENTIFIER-SO-A-KEYWORD-NAMED-ATTRIBUTE-IS-REFUSED]]
//
// An attribute clause NAME may be spelled with a KEYWORD. glibc writes
// `__attribute__ ((__const__))`, whose name lexes as `ConstKeyword`, and ✔MEASURED
// (P31 lane G, re-measured here) ONE `gcc -E -P` `_GNU_SOURCE` TU carries FIFTY
// occurrences of that single spelling — it was the top parse blocker for real
// headers.
//
// ★★★ WHY EVERY ASSERTION BELOW IS ON AN **EFFECT** AND NOT ON "IT PARSED".
// The trap this row names is that the two halves are SEPARABLE. Widen only the
// grammar and `__attribute__((__const__))` parses beautifully, its name is then
// invisible to every semantic reader (each of which looked for `identifierToken`
// alone), it matches NO effect row, and it VANISHES — an attribute the program
// may depend on, dropped in silence. That is strictly WORSE than the parse error
// it replaced, because a refusal is loud. So "no parse error" is exactly the
// assertion that cannot tell the fix from the defect, and it appears here only as
// a precondition, never as a conclusion.
//
// What the pins below assert instead is the OUTCOME OF THE NAME→ROW LOOKUP,
// which can only have run if the reader SAW a keyword-spelled name:
//
//   * a keyword name the language MODELS is accepted (`__const__` → the `none`
//     effects row), and
//   * a keyword name it does NOT model is REFUSED, loudly, by the same gate that
//     refuses a misspelled identifier name (`__volatile__` → S_UnknownTypeAttribute
//     under `unknownStrictAttributeIsError`; `[[__volatile__]]` →
//     S_UnknownAttribute).
//
// Those two are the same mechanism answering differently for two keyword-spelled
// names. A reader that cannot see keyword names answers "no name here" to BOTH,
// so it is silent on both — and every refusal pin below goes red.
//
// ★★ THE REFERENCES DISAGREE, AND THE DISJUNCTION RULE PICKED THE SUPERSET.
// ✔MEASURED 2026-08-24, one TU per word, positive control per row:
//   * clang 19.1.7 accepts EVERY reserved word tested as an attribute name in
//     BOTH the GNU and the `[[…]]` spelling (`-Wunknown-attributes`, exit 0);
//   * gcc 13.3.0 accepts them all inside `[[…]]` (warning `-Wattributes`), and in
//     the GNU spelling accepts `const`/`__const__`, `volatile`, `int`, `static`,
//     `restrict`, `inline`, every type and storage keyword, `_Bool`, `_Complex`,
//     `_Atomic`, `_Noreturn`, `_Thread_local`, `bool`, `constexpr`,
//     `thread_local`, `__int128` — while REFUSING `sizeof`, `typeof`,
//     `__typeof__`, `_Alignof`, `__alignof__`, `struct`, `union`, `enum`,
//     `return`, `if`, `else`, `while`, `for`, `do`, `switch`, `case`, `default`,
//     `break`, `continue`, `goto`, `_Generic`, `_Static_assert`, `true`, `false`,
//     `nullptr`, `alignas`, `alignof`, `static_assert`, `typeof_unqual`,
//     `__attribute__`, `__extension__`, `__asm__`.
// ⚠ The registry row named only four gcc refusals; the measurement above is
// WIDER than that and the conclusion is unchanged — §A.3b takes the union, and
// clang's set is the superset, so DSS admits every keyword. `__volatile__` is
// used below as the UNMODELLED keyword name precisely because BOTH references
// accept it as a NAME (so DSS refusing the SPELLING would be a divergence) while
// neither DSS nor anything else gives it a meaning (so DSS refusing the MEANING
// is the fail-loud posture it applies to every unknown GNU name).

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "analysis/semantic/semantic_test_fixture.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/type_lattice/type_layout.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SemanticModel analyzeC(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

// Every Error-severity diagnostic the TREE BUILDER produced. A clause name the
// grammar refuses dies here, before any semantic reader runs — so a pin that
// counted only semantic codes would report "no error" for a source that never
// parsed at all, which is the shape of green that means nothing.
[[nodiscard]] std::size_t parseErrorsFor(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    std::size_t n = 0;
    for (auto const& t : cu->trees())
        for (auto const& d : t.diagnostics().all())
            if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

} // namespace


// ── the GRAMMAR half: the clause name may be a keyword ───────────────────────

// The shape that motivated the row, verbatim from a preprocessed glibc TU.
// PRECONDITION only — the effect pins below are what distinguish the fix from a
// grammar-only widening, which would also pass this one.
TEST(AttributeClauseNameTokenClass, GlibcKeywordNamedAttributeParses) {
    EXPECT_EQ(parseErrorsFor(
                  "extern double atan (double __x) __attribute__ ((__const__));\n"),
              0u)
        << "gcc 13.3.0 and clang 19.1.7 both accept this exact declaration";
}

// The clause-name position is the SAME position in every attribute spelling and
// at every depth: the GNU form's FIRST clause, its TRAILING clauses, and both
// segments of a C23 `[[ns::name]]`. Widening only the first clause would have
// made `__attribute__((a, __const__))` a parse error while
// `__attribute__((__const__, a))` parsed — one construct meaning two things
// depending on where it is written.
TEST(AttributeClauseNameTokenClass, KeywordNameIsAdmittedInEveryClausePosition) {
    EXPECT_EQ(parseErrorsFor(
                  "extern int f1(int) __attribute__((__const__, __nothrow__));\n"),
              0u) << "keyword name in the FIRST clause of a multi-clause run";
    EXPECT_EQ(parseErrorsFor(
                  "extern int f2(int) __attribute__((__nothrow__, __const__));\n"),
              0u) << "keyword name in a TRAILING clause";
    EXPECT_EQ(parseErrorsFor("[[__const__]] int g1 = 1;\n"), 0u)
        << "keyword name in the C23 spelling";
    EXPECT_EQ(parseErrorsFor("[[gnu::const]] int g2 = 1;\n"), 0u)
        << "keyword name as the FINAL segment of a namespaced C23 attribute";
    EXPECT_EQ(parseErrorsFor(
                  "extern int f3(int) __attribute__((__const__, "
                  "__format__(__printf__, 1, 2)));\n"),
              0u) << "a keyword-named clause beside an argument-bearing one";
    EXPECT_EQ(parseErrorsFor(
                  "int main(void){ __attribute__((__const__)) int x = 3; return x; }\n"),
              0u) << "block scope reaches the same clause-name position";
}


// ── the SEMANTIC half: the name is READ, and the row lookup DISCRIMINATES ────

// ★★ THE PAIR THAT SEPARATES THE FIX FROM A GRAMMAR-ONLY WIDENING. `typedefDecl`'s
// attribute run REPORTS any GNU clause name absent from
// `semantics.attributeSemantics.effects`. Two KEYWORD-spelled names, one modelled
// and one not, must therefore come out DIFFERENT — and they can only differ if
// the reader read the name at all. A reader that still looks for `identifierToken`
// alone finds NO name in either, drops both clauses, and is silent on both: the
// report pin goes red and the pair collapses.
// ⚠ P44 (D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY): the unmodelled half is now a
// WARNING (`S_UnknownAttribute`), not an Error — `typedefDecl` declares
// `unknownStrictAttributeIsError: false` because both references compile the shape
// at rc=0. THE PAIR IS UNAFFECTED: what it proves is that the two names come out
// DIFFERENT, and "one report vs none" says that exactly as well as "error vs
// none". A pin that depended on the SEVERITY rather than on the DISCRIMINATION
// would have been testing the wrong property.
TEST(AttributeClauseNameTokenClass, ModelledKeywordNameIsAcceptedAtTheStrictGate) {
    std::string const src = "typedef __attribute__((__const__)) int T;\n"
                            "int main(void){ T x = 0; return x; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u)
        << "'const' IS in the effects table (the deliberately-inert `none` row), "
           "so the strict gate must recognise it — a keyword name reaching the "
           "gate and matching a row is the whole point";
}

TEST(AttributeClauseNameTokenClass, UnmodelledKeywordNameIsReportedAtTheGate) {
    std::string const src = "typedef __attribute__((__volatile__)) int T;\n"
                            "int main(void){ T x = 0; return x; }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u)
        << "the GRAMMAR admits the keyword name; the verdict below is the "
           "SEMANTIC tier's, which is the tier that knows what names mean";
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 1u)
        << "a keyword-spelled name the language does not model must be REPORTED, "
           "exactly as a misspelled identifier name is — silence here IS the "
           "silent attribute drop this row exists to refuse";
    EXPECT_FALSE(model.hasErrors())
        << "…and reported is not refused: both references compile this at rc=0";
}

// The identifier-named control for the pair above: the gate's behaviour must be
// a property of the NAME, not of which token kind spelled it.
TEST(AttributeClauseNameTokenClass, IdentifierNamedControlIsReportedToo) {
    auto model = analyzeC("typedef __attribute__((frobnicate_xyz)) int T;\n"
                          "int main(void){ T x = 0; return x; }\n");
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 1u);
    EXPECT_FALSE(model.hasErrors());
}

// ★ The C23 spelling has its OWN reader arm (`stdAttrItem` clauses), its own
// diagnostic (a SUPPRESSIBLE Warning, because an unknown standard attribute is
// ignorable per C23 6.7.13.1), and therefore its own way to go silent. Pinned
// as a pair for the same reason as the GNU one above.
TEST(AttributeClauseNameTokenClass, C23UnmodelledKeywordNameWarnsAndModelledDoesNot) {
    auto unmodelled = analyzeC("[[__volatile__]] int gv = 1;\n"
                               "int main(void){ return gv - 1; }\n");
    EXPECT_EQ(countCode(unmodelled.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 1u)
        << "the reader must have READ '__volatile__' to know it models nothing";

    auto modelled = analyzeC("[[__const__]] int gc = 1;\n"
                             "int main(void){ return gc - 1; }\n");
    EXPECT_EQ(countCode(modelled.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 0u)
        << "'const' matches a row, so the same reader must stay silent — the two "
           "answers together are what prove the lookup ran";
}

// ★ `collectAttrClauses`'s TRAILING-clause detector is a SEPARATE reader from
// `extractOneAttrClause`'s name scan, and it can regress on its own: it decides
// whether a nested node IS a clause by asking whether it owns a name token. With
// the identifier-only test it never enumerates a keyword-named trailing clause at
// all, so the clause is not merely unnamed — it is never visited, and every
// diagnostic it could have produced is lost.
TEST(AttributeClauseNameTokenClass, KeywordNamedTrailingClauseIsEnumerated) {
    std::string const src =
        "struct __attribute__((packed, __volatile__)) S { int a; char b; };\n"
        "int main(void){ return (int)sizeof(struct S); }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    // P44: the composite scan's unknown-name report is the row's severity now
    // (`structSpec` declares `unknownStrictAttributeIsError: false`), so this is
    // `S_UnknownAttribute` at Warning. What is being pinned — that the SECOND,
    // keyword-spelled clause is VISITED AT ALL — is unchanged: a reader that
    // still requires an `identifierToken` never enumerates it and the count is 0.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 1u)
        << "the SECOND clause is keyword-spelled; it must be enumerated and "
           "judged on its own merits, not skipped because its name is not an "
           "Identifier";
}

// The composite `packed` sink reads clause names through the same predicate, so
// a keyword-named FIRST clause must not cost the identifier-named trailing one
// its effect. `packed` is a LAYOUT effect — losing it silently produces a struct
// of the wrong size, which is wrong bytes rather than a missing warning.
// ⚠ RE-FIXTURED 2026-08-27 (P42, D-CSUBSET-GNU-UNKNOWN-NAME-GATE-ASYMMETRY).
// THE ASSERTION IS UNCHANGED IN INTENT AND STRICTLY STRONGER IN FORM; ONLY THE
// FIXTURE MOVED, BECAUSE THE OLD ONE MANUFACTURED ITS PRECONDITION OUT OF A BUG.
// It read the count of `S_UnknownTypeAttribute` and expected ONE — and that one
// came from `__const__` being REFUSED by the composite scan's drifted second
// roster, a refusal of legal C that gcc and clang both compile (✔MEASURED, each
// separately). The moment that defect was fixed the count went to 0 and the pin
// went red WITHOUT ITS SUBJECT HAVING CHANGED AT ALL: `packed` was recognised
// before and is recognised now.
// ★ The comment above already named the honest fixture — "losing it silently
// produces a struct of the WRONG SIZE, which is wrong bytes rather than a
// missing warning" — so the pin now asserts THE SIZE, in the source, where no
// diagnostic-roster change can reach it. `_Static_assert` fails loud at the
// semantic tier if the layout is wrong, so a swallowed `packed` (sizeof 8, not
// 5) turns this test red no matter what the diagnostics do.
TEST(AttributeClauseNameTokenClass, KeywordFirstClauseDoesNotCostTheTrailingPackedEffect) {
    std::string const src =
        "struct __attribute__((__const__, packed)) S { int a; char b; };\n"
        "_Static_assert(sizeof(struct S) == 5, \"packed lost beside a "
        "keyword-named clause\");\n"
        "int main(void){ return (int)sizeof(struct S); }\n";
    ASSERT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_FALSE(model.hasErrors())
        << "'packed' must still be recognised beside a keyword-named clause — "
           "the _Static_assert is what proves the LAYOUT, not a diagnostic count";
    // And the keyword-named clause is still ENUMERATED and judged on its own
    // merits — it is now reported as a MODELLED attribute this scan cannot honor
    // rather than refused as unknown, which is the one code both references emit
    // here. Exactly ONE: the trailing `packed` is consumed, never swept in.
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_AttributeIgnoredForDeclarationKind), 1u)
        << "the keyword-named clause must be enumerated and judged, not skipped";
}


// ── the CONFIG half: one declaration, and the loader refuses a half-move ─────

// The shipped `c` document must actually declare the class and wire BOTH sides
// to it. A load that succeeds while one side is unwired is precisely the state
// the drift guard exists to make impossible, so pinning that the shipped
// document loads clean IS pinning that both sides agree.
TEST(AttributeClauseNameTokenClass, ShippedCDocumentDeclaresTheClassOnBothSides) {
    auto schema = loadShippedSchema("c");   // throws with the loader's reasons
    ASSERT_NE(schema, nullptr);
    auto const& tokens = schema->semantics().attributeClauseNameTokens;
    EXPECT_EQ(schema->semantics().attributeClauseNameTokenClassName,
              "attributeClauseName");
    // Identifier plus every distinct kind the `keywords` table produces. The
    // exact count is deliberately NOT pinned — it grows with the language, and a
    // pin on the number would red every time a keyword is added, teaching the
    // next author to edit the test instead of reading it. What must hold is that
    // the class is a genuine SUPERSET of the identifier-only reading.
    EXPECT_GT(tokens.size(), 1u)
        << "a one-element class is indistinguishable from naming the token "
           "directly — the widening would be inert";
}

// The type QUALIFIER `const` is a different thing that happens to share a
// spelling, and the vocabulary additions this row needed (`const` in the effects
// table and in the file-scope linkage-ignored names) must not have touched it.
TEST(AttributeClauseNameTokenClass, TheConstQualifierIsUnaffected) {
    std::string const src = "const int cg = 7;\n"
                            "int main(void){ return cg - 7; }\n";
    EXPECT_EQ(parseErrorsFor(src), 0u);
    auto model = analyzeC(src);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownTypeAttribute), 0u);
    EXPECT_EQ(countCode(model.diagnostics(),
                        DiagnosticCode::S_UnknownAttribute), 0u);
}
