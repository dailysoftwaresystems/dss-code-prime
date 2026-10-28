// ===========================================================================
// Cycle P69 L2 (lane `lm`) — THE COMMA OPERATOR IN AN INTEGER CONSTANT EXPRESSION.
//
// C 6.6p3 keeps the comma operator out of a constant expression (outside an unevaluated
// subexpression — a constraint, so a diagnostic is owed), and 6.6p10 lets an implementation
// accept other forms. The references take that latitude PER POSITION. ✔MEASURED 2026-10-20
// (lane `lm`'s probes/comma: WSL clang 18.1.3 and gcc 13.3.0 at the default mode, -pedantic,
// -pedantic-errors and -std=c2x -pedantic-errors, every build RUN):
//   * clang FOLDS `(0, 42)` — a warning, refused only at -pedantic-errors — as a static
//     assertion, an array bound, an enumerator's value, a case label, a bit-field's width, an
//     index designator and a `_BitInt` width; gcc refuses every one of those;
//   * gcc FOLDS it as `__builtin_object_size`'s type operand, where clang refuses;
//   * NEITHER folds it in `_Alignas` / `aligned(...)`, a `__builtin_choose_expr` condition, or as
//     a null pointer constant (`int *p = (0, 0);` is an integer-to-pointer conversion on both);
//   * both refuse `(y, 1)` for a non-constant `y`; both accept `1 ? 1 : (0, 1)` and
//     `sizeof((0, 1))`, where nothing is owed;
//   * a BARE top-level comma is a SYNTAX error on both as an array bound, a case label and an
//     index designator — the grammar's job (`constantExpr` / `assignmentExpr`).
// So `semantics.integerConstantExpressions.otherConstantForms` names, per form, the positions
// that admit it, and each admitted comma draws S_ConstantExpressionUsesTheCommaOperator once.
//
// Perturbs the SHIPPED c JSON (the block erased, malformed shapes), so it links nlohmann_json.
// ===========================================================================

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/constant_form.hpp"
#include "core/types/data_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "repo_root.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

constexpr DiagnosticCode kComma = DiagnosticCode::S_ConstantExpressionUsesTheCommaOperator;

[[nodiscard]] std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode c) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == c) ++n;
    return n;
}

[[nodiscard]] std::size_t countErrors(DiagnosticReporter const& r) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.severity == DiagnosticSeverity::Error) ++n;
    return n;
}

[[nodiscard]] std::string dump(DiagnosticReporter const& r) {
    std::string out;
    for (auto const& d : r.all()) {
        out += "\n    ";
        out += diagnosticCodeName(d.code);
        out += " @";
        out += std::to_string(d.span.start());
        out += ": ";
        out += d.actual;
    }
    return out.empty() ? std::string{" <no diagnostics>"} : out;
}

[[nodiscard]] std::size_t offsetOf(std::string const& src, std::string const& needle) {
    std::size_t const at = src.find(needle);
    if (at == std::string::npos) throw std::runtime_error("needle not in the source: " + needle);
    return at;
}

[[nodiscard]] nlohmann::json shippedCDocument() {
    std::filesystem::path const path = dss::test::configRoot() / "sources" / "c.lang.json";
    std::ifstream in{path, std::ios::binary};
    if (!in.good()) throw std::runtime_error("cannot open the shipped C document: " + path.string());
    return nlohmann::json::parse(in);
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> loaded(nlohmann::json const& doc) {
    auto r = GrammarSchema::loadFromText(doc.dump(), "<integer-constant-expressions>");
    if (!r.has_value()) throw std::runtime_error("the perturbed c document failed to load");
    return std::move(*r);
}

// The shipped c with `semantics.integerConstantExpressions` erased — the standard's forms at
// every position. The block must be there to erase.
[[nodiscard]] std::shared_ptr<GrammarSchema const> withoutTheBlock() {
    auto doc = shippedCDocument();
    auto& sem = doc.at("semantics");
    if (!sem.contains("integerConstantExpressions"))
        throw std::runtime_error("the shipped c declares no integerConstantExpressions");
    sem.erase("integerConstantExpressions");
    return loaded(doc);
}

[[nodiscard]] std::shared_ptr<GrammarSchema const> shippedC() {
    return dss::test_support::shippedSchemaOrThrow("c");
}

// One translation unit through the front end and the lowering.
struct Lowered {
    std::shared_ptr<CompilationUnit> cu;
    std::unique_ptr<SemanticModel>   model;
    DiagnosticReporter               reporter;   // the lowering's
    std::unique_ptr<CstToHirResult>  result;
    [[nodiscard]] std::size_t parseErrors() const {
        std::size_t n = 0;
        for (auto const& t : cu->trees())
            for (auto const& d : t.diagnostics().all())
                if (d.severity == DiagnosticSeverity::Error) ++n;
        return n;
    }
};

[[nodiscard]] std::unique_ptr<Lowered> run(std::shared_ptr<GrammarSchema const> schema,
                                           std::string const& src, bool lower = true) {
    auto out = std::make_unique<Lowered>();
    UnitBuilder builder{std::move(schema), DiagnosticBudget::libraryDefault()};
    builder.addInMemory(src, "<mem>");
    out->cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    // A data model and layout parameters, as the driver passes: several rows read `sizeof`.
    out->model = std::make_unique<SemanticModel>(
        analyze(out->cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                AggregateLayoutParams{ScalarAlignmentRule::Natural, 16}));
    if (lower && out->parseErrors() == 0 && !out->model->hasErrors())
        out->result = lowerToHir(*out->model, out->reporter);
    return out;
}

// One admitted position: a source whose ONE comma is in that position, the comma's text, and
// a check the position took the folded value (a static assertion over it, or the lowering).
struct Admitted {
    IntegerConstantPosition position;
    std::string             src;
    std::string             comma;   // the comma expression's text, where the warning points
};

[[nodiscard]] std::vector<Admitted> admittedRows() {
    return {
        {IntegerConstantPosition::StaticAssertion,
         "_Static_assert((0, 1), \"sa\");\nint main(void) { return 0; }\n", "0, 1"},
        {IntegerConstantPosition::ArrayBound,
         "int a[(0, 2)];\n_Static_assert(sizeof a == 2 * sizeof(int), \"bound\");\n"
         "int main(void) { return 0; }\n", "0, 2"},
        // At block scope an unfolded bound is a VLA, so the static assertion over `sizeof b`
        // is what says the bound FOLDED.
        {IntegerConstantPosition::ArrayBound,
         "int main(void) {\n  int b[(0, 3)];\n  _Static_assert(sizeof b == 3 * sizeof(int), \"b\");\n"
         "  return (int)sizeof b;\n}\n", "0, 3"},
        {IntegerConstantPosition::EnumeratorValue,
         "enum { E = (0, 42) };\n_Static_assert(E == 42, \"E\");\nint main(void) { return E; }\n",
         "0, 42"},
        {IntegerConstantPosition::CaseLabel,
         "int f(int x) {\n  switch (x) {\n  case (0, 42): return 2;\n  }\n  return 0;\n}\n"
         "int main(void) { return f(42); }\n", "0, 42"},
        {IntegerConstantPosition::BitFieldWidth,
         "struct S { unsigned w : (0, 5); };\nint main(void) { struct S s = { 31 }; return s.w; }\n",
         "0, 5"},
        {IntegerConstantPosition::IndexDesignator,
         "int t[3] = { [(0, 1)] = 40 };\nint main(void) { return t[1]; }\n", "0, 1"},
        {IntegerConstantPosition::BitPreciseIntegerWidth,
         "_BitInt((0, 8)) b;\n_Static_assert(sizeof b == 1, \"b\");\nint main(void) { return 0; }\n",
         "0, 8"},
        {IntegerConstantPosition::ObjectSizeQueryType,
         "static char g[10];\nenum { O = __builtin_object_size(g, (0, 1)) };\n"
         "_Static_assert(O == 10, \"O\");\nint main(void) { return O; }\n", "0, 1"},
    };
}

} // namespace

// Each position the shipped c names folds the comma — the program is clean through the
// lowering — and draws S_ConstantExpressionUsesTheCommaOperator ONCE, at the comma, however
// many passes fold the expression again. Every position of the vocabulary has a row.
// RED-ON-DISABLE: refuse the comma in the CST engine's binary frame (every row errs); drop
// `sayAdmittedForms` (no row warns); drop the dedupe (a bound folded twice warns twice); drop one
// call site's position (that row errs).
TEST(ConstantExpressionComma, EachNamedPositionFoldsItAndWarnsOnce) {
    std::vector<bool> covered(kIntegerConstantPositionCount, false);
    for (Admitted const& row : admittedRows()) {
        covered[static_cast<std::size_t>(row.position)] = true;
        SCOPED_TRACE(std::string{integerConstantPositionName(row.position)} + ":\n" + row.src);
        auto const l = run(shippedC(), row.src);
        EXPECT_EQ(l->parseErrors(), 0u);
        EXPECT_EQ(countErrors(l->model->diagnostics()), 0u) << dump(l->model->diagnostics());
        std::size_t warned = 0;
        for (auto const& d : l->model->diagnostics().all()) {
            if (d.code != kComma) continue;
            ++warned;
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
            EXPECT_EQ(d.span.start(), offsetOf(row.src, row.comma));
        }
        EXPECT_EQ(warned, 1u) << dump(l->model->diagnostics());
        ASSERT_NE(l->result, nullptr) << "the lowering did not run";
        EXPECT_EQ(countErrors(l->reporter), 0u) << dump(l->reporter);
    }
    for (std::size_t i = 0; i < kIntegerConstantPositionCount; ++i)
        EXPECT_TRUE(covered[i]) << "no row for position "
                                << integerConstantPositionName(static_cast<IntegerConstantPosition>(i));
}

// Without the block, every one of those positions takes the standard's forms: the comma is
// refused (an error at that position) and nothing warns. RED-ON-DISABLE: fold the comma
// unconditionally in the CST engine (every row folds clean).
// An index designator's refusal is the lowering's (the analysis leaves an unfolded index
// unplaced, ✔MEASURED 2026-10-20: no analysis error there), so each row is lowered when the
// analysis lets it, and the errors counted are the front end's, both tiers.
TEST(ConstantExpressionComma, WithoutTheBlockEveryPositionRefusesIt) {
    for (Admitted const& row : admittedRows()) {
        SCOPED_TRACE(std::string{integerConstantPositionName(row.position)} + ":\n" + row.src);
        auto const l = run(withoutTheBlock(), row.src);
        EXPECT_EQ(l->parseErrors(), 0u);
        EXPECT_GE(countErrors(l->model->diagnostics()) + countErrors(l->reporter), 1u)
            << dump(l->model->diagnostics()) << dump(l->reporter);
        EXPECT_EQ(countCode(l->model->diagnostics(), kComma) + countCode(l->reporter, kComma), 0u)
            << dump(l->model->diagnostics()) << dump(l->reporter);
    }
}

// Every comma the expression EVALUATES is one warning: `((0, 1), (0, 2))` holds three, as
// clang's -Wunused-value says three times. RED-ON-DISABLE: warn once per expression.
TEST(ConstantExpressionComma, EveryEvaluatedCommaIsItsOwnWarning) {
    std::string const src = "_Static_assert(((0, 1), (0, 2)) == 2, \"n\");\n"
                            "_Static_assert((1, 2, 3) == 3, \"c\");\n";
    auto const l = run(shippedC(), src, /*lower=*/false);
    EXPECT_EQ(countErrors(l->model->diagnostics()), 0u) << dump(l->model->diagnostics());
    EXPECT_EQ(countCode(l->model->diagnostics(), kComma), 5u) << dump(l->model->diagnostics());
}

// 6.6p3 exempts an UNEVALUATED subexpression: a conditional's unselected arm (the engine folds
// it for its type alone) and `sizeof`'s operand draw nothing. Another object's initializer is
// its own expression: `k`'s comma is S_StaticInitializerUsesTheCommaOperator's, said once, not
// this code's again at the assertion that reads `k`. RED-ON-DISABLE: report under
// `notTheAskedExpression` (the first and third assertions warn).
TEST(ConstantExpressionComma, AnUnevaluatedOrAnotherObjectsCommaIsNotThisExpressions) {
    std::string const src = "_Static_assert(1 ? 1 : (0, 1), \"unselected\");\n"
                            "_Static_assert(sizeof((0, 1)) == sizeof(int), \"sizeof\");\n"
                            "static const int k = (0, 3);\n"
                            "_Static_assert(k == 3, \"k\");\n"
                            "int main(void) { return k; }\n";
    auto const l = run(shippedC(), src, /*lower=*/false);
    EXPECT_EQ(countErrors(l->model->diagnostics()), 0u) << dump(l->model->diagnostics());
    EXPECT_EQ(countCode(l->model->diagnostics(), kComma), 0u) << dump(l->model->diagnostics());
    EXPECT_EQ(countCode(l->model->diagnostics(),
                        DiagnosticCode::S_StaticInitializerUsesTheCommaOperator), 1u)
        << dump(l->model->diagnostics());
}

// The DISCARDED operand must fold too: both references refuse `(y, 1)` for a non-constant `y`.
// RED-ON-DISABLE: take the right operand without the left's value (the assertion passes).
TEST(ConstantExpressionComma, TheDiscardedOperandMustFold) {
    std::string const src = "int y;\n_Static_assert((y, 1), \"y\");\n";
    auto const l = run(shippedC(), src, /*lower=*/false);
    EXPECT_EQ(countCode(l->model->diagnostics(), DiagnosticCode::S_StaticAssertFailed), 1u)
        << dump(l->model->diagnostics());
    EXPECT_EQ(countCode(l->model->diagnostics(), kComma), 0u) << dump(l->model->diagnostics());
}

// A position no reference folds the comma in is not named, and refuses it BY NAME: an
// alignment (both spellings), a `__builtin_choose_expr` condition, and a null pointer
// constant — `(0, 0)` is no null pointer constant on either reference, so it converts as an
// integer, diagnosed. RED-ON-DISABLE: admit the forms without a position (each folds).
TEST(ConstantExpressionComma, APositionNoReferenceFoldsItInRefusesItByName) {
    struct Refused { char const* src; DiagnosticCode code; };
    for (Refused const r : {
             Refused{"_Alignas((0, 16)) int x;\n", DiagnosticCode::S_AlignasNonConstant},
             Refused{"int x __attribute__((aligned((0, 16))));\n",
                     DiagnosticCode::S_AlignasNonConstant},
             Refused{"int main(void) { return __builtin_choose_expr((0, 1), 0, 7); }\n",
                     DiagnosticCode::S_BuiltinChooseExprNonConstant},
             Refused{"int main(void) { int *p = (0, 0); return p != 0; }\n",
                     DiagnosticCode::S_IntegerPointerConversion},
         }) {
        SCOPED_TRACE(r.src);
        auto const l = run(shippedC(), r.src, /*lower=*/false);
        EXPECT_EQ(countCode(l->model->diagnostics(), r.code), 1u) << dump(l->model->diagnostics());
        EXPECT_EQ(countCode(l->model->diagnostics(), kComma), 0u) << dump(l->model->diagnostics());
    }
}

// A BARE top-level comma is a syntax error as an array bound, a case label and an index
// designator — on both references — so the grammar states those positions' expression levels
// (`assignmentExpr`, `constantExpr`). RED-ON-DISABLE: put `expression` back in any of the three
// rules (that row parses and folds with a warning).
TEST(ConstantExpressionComma, ABareCommaIsNoBoundCaseOrDesignator) {
    for (char const* src : {
             "int a[0, 2];\n",
             "int main(void) { int a[0, 2]; return (int)sizeof a; }\n",
             "int f(int x) { switch (x) { case 0, 42: return 2; } return 0; }\n",
             "int t[3] = { [0, 1] = 40 };\n",
         }) {
        SCOPED_TRACE(src);
        auto const l = run(shippedC(), src, /*lower=*/false);
        EXPECT_GE(l->parseErrors(), 1u);
        EXPECT_EQ(countCode(l->model->diagnostics(), kComma), 0u) << dump(l->model->diagnostics());
    }
}

// The C 6.8.4.2p3 constraint: a case label's value is an integer constant expression.
// ✔MEASURED (probes/comma g_case_nonconst; gcc 13.3.0, clang 18.1.3, every mode): both refuse
// `case y:` ("case label does not reduce to an integer constant" / "expression is not an integer
// constant expression"); a float and an address are no integer constant either, while an
// unsigned value past INT64_MAX is one, and so are a `sizeof` and a cast to an unsigned type,
// which the evaluator holds unsigned (✔MEASURED 2026-10-22, probe cl_sizeof_case: both run it, 42,
// at -std=gnu2x and -std=c2x -pedantic-errors; a literal folds through the signed arm's bit
// pattern). RED-ON-DISABLE: drop pass2Post's case-label arm (`case y:` and `case 1.5:` draw
// nothing); read the value through the int64 bridge alone (the `sizeof` and cast labels are
// refused).
TEST(ConstantExpressionComma, ACaseLabelIsAnIntegerConstantExpression) {
    struct Row { char const* src; char const* at; std::size_t refused; };
    for (Row const r : {
             Row{"int f(int x, int y) {\n  switch (x) {\n  case y: return 42;\n  }\n  return 1;\n}\n",
                 "y: return", 1},
             Row{"int f(int x) {\n  switch (x) {\n  case 1.5: return 42;\n  }\n  return 1;\n}\n",
                 "1.5", 1},
             Row{"int f(unsigned long long x) {\n  switch (x) {\n  case 0xFFFFFFFFFFFFFFFFULL: return 42;\n"
                 "  case 'a': return 7;\n  default: return 1;\n  }\n}\n",
                 "0xFFFF", 0},
             Row{"int f(unsigned long long x) {\n  switch (x) {\n  case sizeof(int): return 40;\n"
                 "  case (unsigned long long)-1: return 2;\n  default: return 1;\n  }\n}\n",
                 "sizeof", 0},
         }) {
        SCOPED_TRACE(r.src);
        std::string const src = r.src;
        auto const l = run(shippedC(), src, /*lower=*/false);
        std::size_t refused = 0;
        for (auto const& d : l->model->diagnostics().all()) {
            if (d.code != DiagnosticCode::S_CaseLabelNotConstant) continue;
            ++refused;
            EXPECT_EQ(d.severity, DiagnosticSeverity::Error);
            EXPECT_EQ(d.span.start(), offsetOf(src, r.at));
        }
        EXPECT_EQ(refused, r.refused) << dump(l->model->diagnostics());
        if (r.refused == 0)
            EXPECT_EQ(countErrors(l->model->diagnostics()), 0u) << dump(l->model->diagnostics());
    }
}

// The other half of 6.8.4.2p1: the CONTROLLING expression has integer type. A floating case
// value reached the ENCODER on the base (✔MEASURED by the orchestrator at eb6f22a3: `case 1.5:`
// failed in the x86_64 encoder, "A_NoMatchingEncodingVariant … 'xmm4'"), so this reads that a
// float or a pointer CONTROLLING expression is refused by the C rule in the front end — the
// lowering's `switchPrologue` check — and never reaches code generation.
TEST(ConstantExpressionComma, ASwitchOnAValueOfNoIntegerTypeIsRefusedByItsRule) {
    for (char const* const src : {
             "int f(void) { switch (1.5) { case 1: return 42; } return 1; }\n",
             "int f(float x) { switch (x) { default: return 1; } }\n",
             "int f(int *p) { switch (p) { default: return 1; } }\n",
         }) {
        SCOPED_TRACE(src);
        auto const l = run(shippedC(), src);
        EXPECT_EQ(countErrors(l->model->diagnostics()), 0u) << dump(l->model->diagnostics());
        ASSERT_NE(l->result, nullptr) << "the lowering did not run";
        EXPECT_FALSE(l->result->ok);
        bool named = false;
        for (auto const& d : l->reporter.all())
            if (d.severity == DiagnosticSeverity::Error && d.actual.find("C 6.8.4.2p1") != std::string::npos)
                named = true;
        EXPECT_TRUE(named) << "refused by the C rule" << dump(l->reporter);
    }
}

// The block's every malformed shape fails the load, loud — a dropped name would silently turn a
// position a reference folds into a refusal, a misspelt one would read as "not admitted", a form
// the integer-constant evaluator does not read would load and change nothing.
// RED-ON-DISABLE: accept any one shape (its load succeeds).
TEST(ConstantExpressionComma, EveryMalformedBlockFailsTheLoad) {
    struct Bad { char const* json; char const* message; };
    for (Bad const b : {
             Bad{R"([1])", "'integerConstantExpressions' must be an object"},
             Bad{R"({"otherForms": {}})", "otherForms"},
             Bad{R"({"otherConstantForms": ["commaOperator"]})", "must be an object mapping"},
             Bad{R"({"otherConstantForms": {"noSuchForm": ["caseLabel"]}})",
                 "unknown constant form 'noSuchForm'"},
             Bad{R"({"otherConstantForms": {"constObjectRead": ["caseLabel"]}})",
                 "is not read at an integer-constant position"},
             Bad{R"({"otherConstantForms": {"commaOperator": "caseLabel"}})",
                 "must be a non-empty array"},
             Bad{R"({"otherConstantForms": {"commaOperator": []}})", "must be a non-empty array"},
             Bad{R"({"otherConstantForms": {"commaOperator": ["noSuchPosition"]}})",
                 "unknown integer-constant position 'noSuchPosition'"},
             Bad{R"({"otherConstantForms": {"commaOperator": [42]}})",
                 "unknown integer-constant position '42'"},
             Bad{R"({"otherConstantForms": {"commaOperator": ["caseLabel", "caseLabel"]}})",
                 "position 'caseLabel' is listed twice"},
         }) {
        SCOPED_TRACE(b.json);
        auto doc = shippedCDocument();
        doc.at("semantics")["integerConstantExpressions"] = nlohmann::json::parse(b.json);
        auto const r = GrammarSchema::loadFromText(doc.dump(), "<malformed>");
        ASSERT_FALSE(r.has_value());
        bool named = false;
        for (auto const& d : r.error()) {
            if (d.code == DiagnosticCode::C_InvalidSemantics
                && d.message.find(b.message) != std::string::npos)
                named = true;
        }
        EXPECT_TRUE(named);
    }
    // The shipped block loads, and admits the comma at exactly its eight positions.
    auto const shipped = shippedC();
    for (std::size_t i = 0; i < kIntegerConstantPositionCount; ++i) {
        auto const p = static_cast<IntegerConstantPosition>(i);
        EXPECT_TRUE(shipped->semantics().integerConstantExpressions.at(p).admits(
            ConstantForm::CommaOperator)) << integerConstantPositionName(p);
    }
}
