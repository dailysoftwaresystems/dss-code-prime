#pragma once

// A REFERENCE TOOLCHAIN'S LIVE ANSWER IS A CONTROL, NEVER THE SPECIFICATION (P69 fold 3c; the review of fold 2,
// F2). A native cell runs a reference toolchain (gcc with GNU ld, cl with link.exe, Apple's ld, ...) on the
// cell's programs, and a TABLE says what that toolchain answered when the row was measured. What the cell
// asserts of DSS is the TABLE — the measured answer — and the live run is a CONTROL of the table. Where the
// control disagrees, three things can be true, and they must never read as the same red:
//   * the live toolchain IS the one the table was measured with: its answer moved under the table — RED,
//     `reference moved:`, and the table and its row are restated from what it now does;
//   * it is ANOTHER toolchain (a CI image's, a newer one on a leg): nobody measured what it answers, so its
//     disagreement is said on a printed, named, COUNTED line (`[reference-unmeasured]`) and is no failure — the
//     run's LAST line gives the count (`[reference-control]`, printed when the process ends);
//   * the run is STRICT (`DSS_STRICT_ARM_VERDICTS`): an unmeasured toolchain is RED, `measure it:` — a strict run
//     is the one that may not leave a disagreement unmeasured.
// Which toolchain is live is read from the toolchain's own version lines, asked only when a control disagrees
// (a green run pays nothing for it); "the measured one" is every version token of the table's `measuredWith`
// words (`13.3.0`, `2.42`, `ld-1267`) found in those lines.

#include "arm_verdict_ledger.hpp"   // readStrictArmVerdicts, kStrictArmVerdictsEnv

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// The version tokens of a table's `measuredWith` words: each dotted number (`13.3.0`, `19.44`) and each ld64
// project name (`ld-1267`). A token-free text has none, and no live toolchain is then "the measured one".
[[nodiscard]] inline std::vector<std::string> measuredVersionMarks(std::string_view measuredWith) {
    static std::regex const kMark{R"(ld-\d+|\d+(?:\.\d+)+)"};
    std::vector<std::string> marks;
    std::string const        text{measuredWith};
    for (auto it = std::sregex_iterator(text.begin(), text.end(), kMark); it != std::sregex_iterator(); ++it) {
        marks.push_back(it->str());
    }
    return marks;
}

// Whether the live version lines are the toolchain `measuredWith` names: every mark found in them.
[[nodiscard]] inline bool onTheMeasuredToolchain(std::string_view measuredWith, std::string_view liveVersion) {
    auto const marks = measuredVersionMarks(measuredWith);
    if (marks.empty()) return false;
    for (auto const& m : marks) {
        if (liveVersion.find(m) == std::string_view::npos) return false;
    }
    return true;
}

// How a disagreeing control reads.
enum class ControlReading : std::uint8_t {
    ReferenceMoved,   // on the measured toolchain: RED
    MeasureIt,        // on another, in a strict run: RED
    Unmeasured,       // on another: a counted line
};

[[nodiscard]] inline ControlReading readDisagreement(bool onMeasured, StrictArmVerdictsSetting const& strict) {
    if (onMeasured) return ControlReading::ReferenceMoved;
    if (strict.on || strict.malformed) return ControlReading::MeasureIt;
    return ControlReading::Unmeasured;
}

namespace reference_control_detail {
inline std::atomic<unsigned>& unmeasuredCount() {
    static std::atomic<unsigned> n{0};
    return n;
}
inline void printTheCount() {
    std::printf("[reference-control] %u control(s) disagreed with their table on a toolchain no table was measured "
                "with (each named above on a [reference-unmeasured] line)\n",
                unmeasuredCount().load());
    std::fflush(stdout);
}
inline void printTheCountWhenTheProcessEnds() {
    static bool const registered = (std::atexit(&printTheCount), true);
    (void)registered;
}
}  // namespace reference_control_detail

// How many disagreements this process counted (not failed) so far.
[[nodiscard]] inline unsigned unmeasuredControls() { return reference_control_detail::unmeasuredCount().load(); }

// A control DISAGREED with its table: `label` names the toolchain as the cell's lines do, `liveVersion` is its
// own version lines, `measuredWith` the table's words; `answered` and `tableSays` what each said. Judged as the
// header states — a failure, or a counted line, never silence.
inline void controlDisagrees(std::string const& label, std::string const& measuredWith, std::string const& liveVersion,
                             std::string const& answered, std::string const& tableSays) {
    reference_control_detail::printTheCountWhenTheProcessEnds();
    std::string const what = label + " " + liveVersion + " answered " + answered + ", the table (measured with "
                           + measuredWith + ") says " + tableSays;
    switch (readDisagreement(onTheMeasuredToolchain(measuredWith, liveVersion), readStrictArmVerdicts())) {
        case ControlReading::ReferenceMoved:
            ADD_FAILURE() << "reference moved: " << what
                          << " — on the toolchain the table was measured with, the REFERENCE's answer changed "
                             "under the table, which is NOT DSS differing from the reference: read what the live "
                             "tool does, then restate the table and the row it was measured for";
            return;
        case ControlReading::MeasureIt:
            ADD_FAILURE() << "measure it: " << what << " — a toolchain no table was measured with answered "
                          << "otherwise, and a strict run (" << kStrictArmVerdictsEnv
                          << ") fails on that: measure this toolchain and state its answer in the table";
            return;
        case ControlReading::Unmeasured:
            ++reference_control_detail::unmeasuredCount();
            std::cout << "[reference-unmeasured] " << what
                      << " — not the toolchain the table was measured with: said and counted, not failed\n";
            return;
    }
}

}  // namespace dss::test_support
