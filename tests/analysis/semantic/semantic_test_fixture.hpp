#pragma once

// Shared helpers for the SE1+ semantic-analyzer tests. Mirrors the
// shape of tests/analysis/compilation_unit/toy_cu_fixture.hpp, scoped
// to the analysis/semantic suite.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dss::sem_test {

[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadShippedSchema(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"" << name << "\") failed";
        std::abort();
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
    UnitBuilder builder{schema};
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
    return analyze(cu);
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
