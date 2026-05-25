#pragma once

// Shared toy-CompilationUnit helpers for the CU3 test files
// (test_unit_attribute.cpp + test_symbol_population.cpp). Kept here rather
// than copied per-file so the two suites can't drift.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dss::cu_test {

[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadShippedSchema(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"" << name << "\") failed";
        std::abort();
    }
    return *loaded;
}

[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadToySchema() {
    return loadShippedSchema("toy");
}

// True if any diagnostic in `reporter` carries `code`.
[[nodiscard]] inline bool hasCode(DiagnosticReporter const& reporter, DiagnosticCode code) {
    auto all = reporter.all();
    return std::any_of(all.begin(), all.end(),
                       [code](ParseDiagnostic const& d) { return d.code == code; });
}

[[nodiscard]] inline std::size_t countCode(DiagnosticReporter const& reporter, DiagnosticCode code) {
    auto all = reporter.all();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(),
                      [code](ParseDiagnostic const& d) { return d.code == code; }));
}

// Build a CompilationUnit from one in-memory toy source per entry. Each source
// must be a clean toy program so the resulting Tree has a valid root.
[[nodiscard]] inline CompilationUnit makeToyUnit(std::initializer_list<std::string> sources) {
    UnitBuilder builder{loadToySchema()};
    unsigned index = 0;
    for (auto const& source : sources) {
        builder.addInMemory(source, "<mem" + std::to_string(index++) + ">");
    }
    return std::move(builder).finish();
}

} // namespace dss::cu_test
