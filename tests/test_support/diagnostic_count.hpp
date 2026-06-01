#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <cstddef>

// `countCode(reporter, code)` — count how many diagnostics in
// `reporter.all()` carry `code`. Hoisted at FF3+FF4 post-fold #2
// (2026-06-01) and consolidated at post-fold #3. Five known
// consumers across the test tree:
//   * tests/ffi/test_binary_reader.cpp
//   * tests/ffi/test_c_header_parser.cpp
//   * tests/ffi/test_abi_catalog.cpp
//   * tests/mir/test_mir_verifier.cpp
//   * tests/hir/test_hir_verifier.cpp
//
// Matches the `scratch_dir.hpp` hoist precedent (helper extracted
// once the count of structurally-identical duplicates crossed the
// hoist threshold). Pure read over the reporter's span — no state,
// no per-consumer variance.
//
// Anchored D-TEST-DIAG-SEVERITY-EXACT-PIN (0f7d714 audit-fold
// 2026-06-01): the `sawX bool + EXPECT_EQ severity + EXPECT_EQ
// errorCount` pattern at
// tests/ffi/test_binary_reader.cpp::Elf64PartialCorruptionFires...
// is a singleton today; a second test combining the same trio
// triggers hoisting an `expectExactlyOneDiagWith(rep, code, severity,
// expectedErrorCount)` helper here. The 97 `bool sawX = false`
// occurrences across the codebase use disjoint assertion combinations
// (payload fields / spans / multi-code) and do NOT match this trio.

namespace dss::test_support {

[[nodiscard]] inline std::size_t
countCode(DiagnosticReporter const& r, DiagnosticCode c) noexcept {
    std::size_t n = 0;
    for (auto const& d : r.all()) {
        if (d.code == c) ++n;
    }
    return n;
}

} // namespace dss::test_support
