#pragma once

// Shared REJECT-assertion vocabulary for `ObjectFormatSchema` negative-load
// tests. Hoisted once the 3rd consumer landed (the precedent set by
// macho_test_support.hpp and link_test_support.hpp); today SEVEN files use it
// — test_elf_exec_writer, test_pe_writer, test_pe_dll_writer,
// test_codesign_placeholder, test_macho_codesign, test_macho_writer,
// test_macho_arm64_exit.
//
// ★ WHY THIS EXISTS AT ALL — the failure it makes structurally impossible.
//
// A negative-load test builds a deliberately-broken schema and asserts the
// loader rejects it. Written as a bare `ASSERT_FALSE(r.has_value())`, that
// assertion says only "SOMETHING was wrong" — it never says WHICH rule fired.
// `ObjectFormatData::validate()` ACCUMULATES diagnostics (there is no early
// return before its `return problems;`), and `loadFromText` rejects if ANY
// accumulated diagnostic is an error, so the moment a NEW load rule starts
// rejecting the same fixture for an unrelated reason, the old test keeps
// passing while the rule it was written to pin can be deleted outright.
//
// That is not hypothetical. D-LK10-ENTRY §2.13 added "an exec-flavored format
// MUST declare `processExit`", and every exec-flavored negative fixture in
// this tree omitted `processExit` — so each was instantly rejected for the NEW
// reason and went vacuous in place, silently, with the suite still green.
//
// The repair has two halves and BOTH are required:
//   (A) the fixture declares the entry cluster (`processExit` +
//       `entryCallingConvention`, values copied verbatim from the shipped
//       `src/dss-config/object-formats/*-exec.format.json`), so the only
//       remaining reason to reject it is its own defect; and
//   (B) the test asserts THAT SPECIFIC diagnostic — `countAtPath` on the
//       rule's own JSON pointer, plus `countAtPath(r, "/processExit") == 0`,
//       which is the anti-subsumption half: it fails the moment the fixture
//       starts being rejected for the entry-cluster reason again.
//
// (A) alone re-arms the same trap for the next rule; (B) alone leaves a
// second rejection reason confounding the pin. Together, deleting the rule
// under test turns the load GREEN and the `countAtPath(..., 1u)` line red.

#include "link/object_format_schema.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace dss::link_format::test {

using FormatLoadResult = LoadResult<std::shared_ptr<ObjectFormatSchema>>;

// How many ERROR diagnostics of a failed load carry `needle` inside their
// JSON-pointer path. Zero for a load that SUCCEEDED — so a rule that stops
// firing reads as `0`, never as "vacuously satisfied".
[[nodiscard]] inline std::size_t countAtPath(FormatLoadResult const& r,
                                             std::string_view needle) {
    if (r.has_value()) return 0;
    std::size_t n = 0;
    for (auto const& d : r.error()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.path.find(needle) != std::string::npos) ++n;
    }
    return n;
}

// How many ERROR diagnostics carry `needle` inside their MESSAGE. Needed
// wherever two DIFFERENT rules share one JSON pointer (e.g. `/elf/pageAlign`
// is both "must be a positive power of two" from the loader and "must declare
// 'elf.pageAlign'" from validate()): the path alone cannot tell them apart.
[[nodiscard]] inline std::size_t countWithMessage(FormatLoadResult const& r,
                                                  std::string_view needle) {
    if (r.has_value()) return 0;
    std::size_t n = 0;
    for (auto const& d : r.error()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.message.find(needle) != std::string::npos) ++n;
    }
    return n;
}

// How many ERROR diagnostics a failed load produced, in total. This is the
// machine-checked form of "(A) the fixture is rejected ONLY for its own
// defect": a comment claiming isolation rots, `EXPECT_EQ(errorCount(r), 1u)`
// goes red the day a second reason appears. MEASURED per fixture — never
// assumed — because several defects legitimately raise a knock-on second
// diagnostic (a rejected value is not stored, so a downstream
// "required"/"congruent" rule then sees the zero default).
[[nodiscard]] inline std::size_t errorCount(FormatLoadResult const& r) {
    if (r.has_value()) return 0;
    std::size_t n = 0;
    for (auto const& d : r.error()) {
        if (d.severity == DiagnosticSeverity::Error) ++n;
    }
    return n;
}

// Every diagnostic of a failed load, rendered `path: message` one per line —
// the `<<` payload that makes a mismatch self-diagnosing instead of printing
// a bare `0 != 1`. Says so explicitly when the load SUCCEEDED, which is the
// red-on-disable shape: delete the rule under test and the load goes green.
[[nodiscard]] inline std::string rejectSummary(FormatLoadResult const& r) {
    if (r.has_value()) {
        return "load SUCCEEDED (no diagnostics) -- the rule under test did "
               "not fire";
    }
    std::string out = "diagnostics:";
    for (auto const& d : r.error()) {
        out += "\n  [";
        out += severityName(d.severity);
        out += "] ";
        out += d.path;
        out += ": ";
        out += d.message;
    }
    return out;
}

}  // namespace dss::link_format::test
