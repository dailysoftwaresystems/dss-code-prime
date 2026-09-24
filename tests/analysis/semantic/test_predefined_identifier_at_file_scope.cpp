// ===========================================================================
// P68 round 9 (lane `cs`) — A PREDEFINED FUNCTION-NAME IDENTIFIER OUTSIDE EVERY
// FUNCTION BODY.
//
// THE PROPERTY THIS FILE OWNS: C 6.4.2.2 declares `__func__` (and DSS's configured
// alias `__FUNCTION__`) only inside a function definition, so outside every body
// ISO C has nothing to name and DSS refused the use S_UndeclaredIdentifier. That
// put DSS below the union:
//
// ✔REFERENCE VOTES, 2026-09-23, each case one translation unit probed SEPARATELY
// (the lane's `.temp/probe/r2f/`), every build RUN: gcc 13.3.0 at `-std=c2x`
// (a pedwarn — `-std=c17 -pedantic-errors` refuses), clang 18.1.3 at BOTH modes
// (-Wpredefined-identifier-outside-function) and mingw-w64 13.2.0 at `-std=c2x`
// ACCEPT `enum { N = sizeof(__func__) };` (exit 1), `static const char *g =
// __func__;` (g[0] == 0) and a file-scope array bound, and AGREE the object is the
// EMPTY string; MSVC 19.51 refuses (C2065). The union owes gcc's and clang's
// program, with their meaning and their warning.
//
// So one file-scope twin per configured spelling is bound in the language's
// builtin scope (text "", `const` elements, the same config element core as the
// per-function symbol); a function definition's own symbol shadows it inside the
// body, and every use outside reports S_PredefinedIdentifierOutsideFunction — a
// suppressible Warning that `--warnings-as-errors` turns into MSVC's refusal.
//
// ── RED-ON-DISABLE (the lane's transcript carries each build and its names) ──
//   * the file-scope twin not minted → every file-scope arm refuses
//     S_UndeclaredIdentifier again, the in-body control stays green;
//   * the use-site warning removed → the warning counts go to 0.
// ===========================================================================

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/unsuppressable_codes.hpp"

#include "semantic_test_fixture.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

using namespace dss;
using namespace dss::sem_test;

namespace {

[[nodiscard]] SymbolRecord const*
findSymbolNamed(SemanticModel const& model, std::string_view name) {
    for (std::size_t i = 1; i < model.symbols().size(); ++i) {
        if (model.symbols()[i].name == name) return &model.symbols()[i];
    }
    return nullptr;
}

// The layout every const-expression pin in this directory passes explicitly:
// `analyze()`'s direct-API default supplies NO `aggregateLayout`, and without it a
// `sizeof` in an enumeration constant or an array bound DECLINES to fold (the
// shape `test_generic_selection_constant_expression.cpp` states). `Natural` + 16 is
// what the shipped x86_64 / arm64 targets declare.
[[nodiscard]] SemanticModel analyzeWithLayout(std::string const& src) {
    auto cu = buildShippedUnit("c", {src});
    assertNoBuilderErrors(*cu);
    return analyze(cu, DiagnosticBudget::libraryDefault(), DataModel::Lp64,
                   AggregateLayoutParams{ScalarAlignmentRule::Natural, 16});
}

constexpr DiagnosticCode kOutside = DiagnosticCode::S_PredefinedIdentifierOutsideFunction;

}  // namespace

// ── the empty string, at every file-scope position the references run ───────
TEST(PredefinedIdentifierAtFileScope, NamesTheEmptyStringWithAWarningAtEachUse) {
    // An enumeration constant: sizeof "" is 1.
    {
        auto model = analyzeWithLayout(std::string{
            "enum { N = sizeof(__func__) };\nint main(void) { return N; }\n"});
        EXPECT_FALSE(model.hasErrors()) << "an enum constant sized by __func__";
        EXPECT_EQ(countCode(model.diagnostics(), kOutside), 1u);
        auto const* n = findSymbolNamed(model, "N");
        ASSERT_NE(n, nullptr);
        EXPECT_EQ(n->enumValue, 1) << "sizeof of the file-scope __func__ is sizeof \"\"";
    }
    // The configured GNU alias, the same way.
    {
        auto model = analyzeWithLayout(std::string{
            "enum { M = sizeof(__FUNCTION__) };\nint main(void) { return M; }\n"});
        EXPECT_FALSE(model.hasErrors()) << "__FUNCTION__ at file scope";
        EXPECT_EQ(countCode(model.diagnostics(), kOutside), 1u);
        auto const* m = findSymbolNamed(model, "M");
        ASSERT_NE(m, nullptr);
        EXPECT_EQ(m->enumValue, 1);
    }
    // A file-scope array bound: 1 + 41 elements.
    {
        auto model = analyzeWithLayout(std::string{
            "static char buf[sizeof(__func__) + 41];\n"
            "int main(void) { return (int)sizeof buf; }\n"});
        EXPECT_FALSE(model.hasErrors()) << "a file-scope array bound";
        EXPECT_EQ(countCode(model.diagnostics(), kOutside), 1u);
        auto const* buf = findSymbolNamed(model, "buf");
        ASSERT_NE(buf, nullptr);
        auto const& in = model.lattice().interner();
        ASSERT_EQ(in.kind(buf->type), TypeKind::Array);
        ASSERT_FALSE(in.scalars(buf->type).empty());
        EXPECT_EQ(in.scalars(buf->type)[0], 42);
    }
    // An address constant initializing a file-scope pointer, and two uses: two
    // warnings, one per use.
    {
        auto model = analyzeWithLayout(std::string{
            "static const char *g = __func__;\n"
            "static const char *h = __func__;\n"
            "int main(void) { return g[0] == 0 && h[0] == 0 ? 42 : 1; }\n"});
        EXPECT_FALSE(model.hasErrors()) << "a file-scope pointer to __func__";
        EXPECT_EQ(countCode(model.diagnostics(), kOutside), 2u);
    }
}

// ── the controls: inside a body it is still the function's own name ─────────
TEST(PredefinedIdentifierAtFileScope, InsideABodyItIsTheFunctionsOwnNameAndSilent) {
    auto model = analyzeWithLayout(std::string{
        "int main(void) { return (int)sizeof(__func__) + (int)sizeof __FUNCTION__; }\n"});
    EXPECT_FALSE(model.hasErrors());
    EXPECT_EQ(countCode(model.diagnostics(), kOutside), 0u)
        << "a use inside a function body resolves to that function's own symbol";
    // A block-scope enumeration constant sized by it: the function's name, 5.
    auto inBody = analyzeWithLayout(std::string{
        "int main(void) { enum { K = sizeof(__func__) }; return K; }\n"});
    EXPECT_FALSE(inBody.hasErrors());
    EXPECT_EQ(countCode(inBody.diagnostics(), kOutside), 0u);
    auto const* k = findSymbolNamed(inBody, "K");
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->enumValue, 5) << "\"main\" plus its NUL";
}

// ── a WARNING the policy can suppress or promote ─────────────────────────────
TEST(PredefinedIdentifierAtFileScope, TheWarningIsSuppressibleAndWarningsAsErrorsRefuses) {
    auto model = analyzeWithLayout(std::string{
        "enum { N = sizeof(__func__) };\nint main(void) { return N; }\n"});
    bool sawWarning = false;
    for (auto const& d : model.diagnostics().all()) {
        if (d.code != kOutside) continue;
        EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
        sawWarning = true;
    }
    EXPECT_TRUE(sawWarning);
    EXPECT_FALSE(isUnsuppressable(kOutside))
        << "a request to suppress it must be honoured";
    DiagnosticReporter::Config strict{};
    strict.policy.warningsAsErrors = true;
    DiagnosticReporter const strictReporter{strict};
    EXPECT_EQ(strictReporter.effectiveSeverity(kOutside, DiagnosticSeverity::Warning),
              DiagnosticSeverity::Error)
        << "--warnings-as-errors turns the warning into MSVC's refusal";
    DiagnosticReporter::Config quiet{};
    quiet.policy.suppress.insert(kOutside);
    DiagnosticReporter const quietReporter{quiet};
    EXPECT_FALSE(quietReporter.effectiveSeverity(kOutside, DiagnosticSeverity::Warning)
                     .has_value())
        << "--suppress drops it";
}
