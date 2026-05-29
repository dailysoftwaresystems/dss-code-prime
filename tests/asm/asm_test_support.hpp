#pragma once

// Shared helpers for asm tests. Promoted from per-test-file duplicates
// (simplifier review of AS2 cycle 2) so a future tests/asm/* file
// gains the same convenience without re-rolling the boilerplate.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstddef>

namespace dss::test_support::asm_ {

// Count how many diagnostics in the reporter carry the given code.
// Tests use this for "exactly N of code X were emitted" assertions
// (the parallel-index continuity guarantee in AS1 cycle 1; the
// per-instruction fail-loud check in AS2 cycle 2).
[[nodiscard]] inline std::size_t
countDiagnostics(DiagnosticReporter const& rep, DiagnosticCode code) noexcept {
    std::size_t n = 0;
    for (auto const& d : rep.all()) {
        if (d.code == code) ++n;
    }
    return n;
}

} // namespace dss::test_support::asm_
