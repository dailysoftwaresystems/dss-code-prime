#pragma once

// Shared helpers for the SE1+ semantic-analyzer tests. Mirrors the
// shape of tests/analysis/compilation_unit/toy_cu_fixture.hpp, scoped
// to the analysis/semantic suite.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace dss::sem_test {

// ⚠ THROWS, NEVER `abort()` — D-TEST-SEMANTIC-FIXTURE-ABORTS-THE-WHOLE-BINARY,
// fixed 2026-08-17. It used to `ADD_FAILURE()` and then `std::abort()`, which
// kills the whole test PROCESS: every sibling test in that executable loses its
// verdict and the harness cannot even report which unit failed. ✔MEASURED that
// this is not hypothetical — a config-mutating pin in this very directory drove
// `loadShipped` to a legitimate refusal and the binary died with
// `0xc0000409` mid-suite, taking nine passing tests' results with it and
// reporting the cause as an exception code rather than as the load error it was.
// The project already made this exact correction one layer down and recorded the
// reason: `test_support/repo_root.hpp` — "`std::abort()` kills the whole test
// BINARY ... `repoRoot()` throws instead (GoogleTest reports a throw as a
// failure of that ONE test)". Same fault, same fix, same rationale.
// ★ The diagnostics are carried IN the message: a load refusal that says only
// "failed" sends the reader to guess which key broke.
[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadShippedSchema(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) {
        std::string why;
        for (auto const& d : loaded.error()) {
            why += "\n  "; why += d.path; why += ": "; why += d.message;
        }
        throw std::runtime_error("loadShipped(\"" + std::string{name}
                                 + "\") failed:" + why);
    }
    return *loaded;
}

[[nodiscard]] inline bool hasCode(DiagnosticReporter const& r, DiagnosticCode code) {
    auto all = r.all();
    return std::any_of(all.begin(), all.end(),
                       [code](ParseDiagnostic const& d) { return d.code == code; });
}

[[nodiscard]] inline std::size_t countCode(DiagnosticReporter const& r, DiagnosticCode code) {
    auto all = r.all();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(),
                      [code](ParseDiagnostic const& d) { return d.code == code; }));
}

[[nodiscard]] inline std::shared_ptr<CompilationUnit const>
buildShippedUnit(std::string_view langName,
                 std::initializer_list<std::string> sources) {
    auto schema = loadShippedSchema(langName);
    UnitBuilder builder{schema, DiagnosticBudget::libraryDefault()};
    unsigned index = 0;
    for (auto const& src : sources) {
        builder.addInMemory(src, "<mem" + std::to_string(index++) + ">");
    }
    return std::make_shared<CompilationUnit>(std::move(builder).finish());
}

[[nodiscard]] inline SemanticModel analyzeShipped(
    std::string_view langName,
    std::initializer_list<std::string> sources) {
    auto cu = buildShippedUnit(langName, sources);
    return analyze(cu, DiagnosticBudget::libraryDefault());
}

// Pre-emit a hint when a CU has tree-builder errors so the test author
// notices their corpus is itself malformed instead of chasing a phantom
// semantic miss.
inline void assertNoBuilderErrors(CompilationUnit const& cu) {
    for (auto const& t : cu.trees()) {
        for (auto const& d : t.diagnostics().all()) {
            if (d.severity == DiagnosticSeverity::Error) {
                ADD_FAILURE() << "tree-builder error in fixture: code="
                              << diagnosticCodeName(d.code)
                              << " actual=" << d.actual;
            }
        }
    }
}

} // namespace dss::sem_test
