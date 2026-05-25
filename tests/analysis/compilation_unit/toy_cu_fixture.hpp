#pragma once

// Shared toy-CompilationUnit helpers for the CU3 test files
// (test_unit_attribute.cpp + test_symbol_population.cpp). Kept here rather
// than copied per-file so the two suites can't drift.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

namespace dss::cu_test {

[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadToySchema() {
    auto loaded = GrammarSchema::loadShipped("toy");
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"toy\") failed";
        std::abort();
    }
    return *loaded;
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
