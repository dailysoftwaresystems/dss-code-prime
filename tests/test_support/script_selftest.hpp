#pragma once

// A SCRIPT ENTRY'S SELF-TEST, ONE gtest CASE PER ARM — what every test of that kind shares.
//
// THE PROBLEM IT ANSWERS. A script that proves its own properties — a `cmake -P` module
// run in its self-test mode, a runner action's program run with `--self-test` — is a
// ctest entry with ONE verdict and no cases. The mutation sweep
// (`tests/mutations/arms.registry`) starts a TEST BINARY whole and judges the exact set
// of cases that go red, so a mutation of such a script could not be an arm while its
// only reader was the script itself.
//
// THE ANSWER, AND ITS FOUR PARTS. A test binary DRIVES the script's own entry and reads
// each of its arms into a case:
//   1. the entry's command line is composed ONCE, by the build, at configure
//      (`dss_declare_script_selftest` in the top-level CMakeLists), and reaches the test
//      as a generated header — so the binary starts exactly what the entry is, and a
//      copy of the tree configured where it stands runs ITS OWN script;
//   2. the script prints ONE VERDICT LINE PER ARM, in the grammar below;
//   3. the test holds a table of the arms' names, a case per name
//      (`expectTheScriptArmPassed`), a case that the script's arms and the table are the
//      same set (`expectEveryScriptArmIsACaseAndEveryCaseAnArm`), and a case that the
//      script's exit code says what its arms said
//      (`expectTheScriptsExitCodeAgreesWithItsArms`);
//   4. the build declares the object of the test's source as depending on the script's
//      files (`dss_use_script_selftest`), so a mutated script rebuilds it and a tool
//      asking the build graph "was an object that depends on the site rebuilt" has its
//      witness.
// A test of this kind RE-IMPLEMENTS NO ARM: an arm's logic, its fixtures and its message
// live in the script, and a case says nothing but "the script's arm of this name ran, and
// said ok". A SECOND SCRIPT ENTRY COSTS A TABLE, never a copy of this file.
//
// THE VERDICT LINE. Anywhere in a line of the script's standard output:
//     <marker>ok <spaces><the arm's name>
//     <marker>FAILED <spaces><the arm's name> -- <what failed, in the script's words>
// Any other line that holds the marker (a closing count, a timing) is no arm's verdict.
// A name may itself hold ` -- `, so the split is never guessed from the text: the
// LONGEST name of the test's table that fits is the arm.
//
// ★ IT FAILS, IT NEVER SKIPS. If the command line cannot be started, or its output
// cannot be read back, EVERY case fails with the reason: a self-test that did not run
// has proven nothing, and a skip would read as a pass on a host that lacks the script's
// interpreter.
//
// Standard output goes to a file, which the verdict lines are read from; standard error
// — a refusal's own text, the closing count of a red run — stays the test's own, so it
// is in the log beside the cases it explains.

#include "core/substrate/process_spawn.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dss::test_support {

// One script entry, as its test states it. Every member outlives the run: the command
// line and the two paths are the generated header's, the rest the test's own constants.
struct ScriptSelfTest {
    std::span<char const* const>      command;           // the entry's command line, argument by argument
    char const*                       workingDirectory;  // the directory it runs in
    char const*                       standardOutput;    // the file its standard output goes to
    std::string_view                  marker;            // what the script prints before each verdict
    std::span<std::string_view const> armNames;          // the test's table of the script's arms
    std::string_view                  tableName;         // that table's name, for the message that asks to add to it
};

inline constexpr std::string_view kScriptDetailOpener = " -- ";

struct ScriptArmVerdict {
    int         verdicts = 0;      // how many verdict lines named this arm
    bool        ok       = false;  // what the last of them said
    std::string detail;            // what a FAILED line said after the arm's name
};

struct ScriptSelfTestRun {
    bool        started  = false;  // the command line ran to an exit code AND its output was read
    int         exitCode = 0;
    std::string whyNot;            // non-empty exactly when `started` is false
    std::map<std::string, ScriptArmVerdict, std::less<>> arms;     // every name of the table is a key
    std::vector<std::string>                             unnamed;  // verdict lines that name no arm of the table
    int         verdictLines = 0;  // ok and FAILED lines, named or not
    int         failedLines  = 0;  // FAILED lines, named or not
};

[[nodiscard]] inline std::string scriptCommandLine(ScriptSelfTest const& entry) {
    std::string out;
    for (char const* argument : entry.command) {
        if (!out.empty()) {
            out += ' ';
        }
        out += argument;
    }
    return out;
}

// The arm of the table that `text` names: `text` IS the name (an `ok` line), or opens
// with it followed by the detail opener (a FAILED line).
[[nodiscard]] inline std::string_view scriptArmNamedBy(ScriptSelfTest const& entry, std::string_view text) {
    std::string_view best;
    for (std::string_view const name : entry.armNames) {
        bool const fits = text == name
                          || (text.size() >= name.size() + kScriptDetailOpener.size()
                              && text.substr(0, name.size()) == name
                              && text.substr(name.size(), kScriptDetailOpener.size()) == kScriptDetailOpener);
        if (fits && name.size() > best.size()) {
            best = name;
        }
    }
    return best;
}

inline void readScriptVerdictLine(ScriptSelfTest const& entry, std::string_view line, ScriptSelfTestRun& run) {
    std::size_t const at = line.find(entry.marker);
    if (at == std::string_view::npos) {
        return;
    }
    std::string_view rest = line.substr(at + entry.marker.size());
    bool             ok   = false;
    if (rest.substr(0, 3) == "ok ") {
        ok   = true;
        rest = rest.substr(3);
    } else if (rest.substr(0, 7) == "FAILED ") {
        rest = rest.substr(7);
    } else {
        return;  // a closing count, a timing: no arm's verdict
    }
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    ++run.verdictLines;
    if (!ok) {
        ++run.failedLines;
    }
    std::string_view const name = scriptArmNamedBy(entry, rest);
    if (name.empty()) {
        run.unnamed.emplace_back(rest);
        return;
    }
    ScriptArmVerdict& verdict = run.arms[std::string(name)];
    ++verdict.verdicts;
    verdict.ok     = ok;
    verdict.detail = rest.size() > name.size() ? std::string(rest.substr(name.size() + kScriptDetailOpener.size()))
                                               : std::string();
}

// Runs the entry ONCE and reads every verdict line. A test keeps the result in a
// function-local static: the script builds its fixtures once, and every case reads the
// same output.
[[nodiscard]] inline ScriptSelfTestRun runScriptSelfTest(ScriptSelfTest const& entry) {
    ScriptSelfTestRun run;
    for (std::string_view const name : entry.armNames) {
        run.arms.emplace(std::string(name), ScriptArmVerdict{});
    }
    std::vector<std::string> argv;
    for (char const* argument : entry.command) {
        argv.emplace_back(argument);
    }
    std::filesystem::path const            output  = entry.standardOutput;
    dss::substrate::SpawnResult const spawned =
        dss::substrate::spawnAndWaitRedirectStdout(argv, std::filesystem::path(entry.workingDirectory), output);
    if (!spawned.spawned) {
        run.whyNot = "the self-test's command line could not be started: " + spawned.diagnostic;
        return run;
    }
    run.exitCode = spawned.exitCode;
    std::ifstream in(output, std::ios::binary);
    if (!in) {
        run.whyNot = "the self-test ran (exit " + std::to_string(spawned.exitCode)
                     + ") and its standard output could not be read back from " + output.string();
        return run;
    }
    std::string const text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::size_t       from = 0;
    while (from <= text.size()) {
        std::size_t const end  = text.find('\n', from);
        std::size_t const stop = end == std::string::npos ? text.size() : end;
        std::string_view  line(text.data() + from, stop - from);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        readScriptVerdictLine(entry, line, run);
        if (end == std::string::npos) {
            break;
        }
        from = end + 1;
    }
    run.started = true;
    return run;
}

// The one assertion every arm's case makes. A red says what the SCRIPT said: the arm's
// name and its own detail, in the words of the script's own FAILED line.
inline void expectTheScriptArmPassed(ScriptSelfTest const& entry, ScriptSelfTestRun const& run,
                                     std::string_view armName) {
    ASSERT_TRUE(run.started) << run.whyNot << "\n  command line: " << scriptCommandLine(entry);
    auto const found = run.arms.find(armName);
    ASSERT_TRUE(found != run.arms.end()) << "no arm of this name in the table: " << armName;
    ScriptArmVerdict const& verdict = found->second;
    ASSERT_EQ(verdict.verdicts, 1) << "the self-test gave this arm " << verdict.verdicts
                                   << " verdict(s), not one (it exited " << run.exitCode << " after "
                                   << run.verdictLines << " verdict line(s)): " << armName;
    EXPECT_TRUE(verdict.ok) << entry.marker << "FAILED  " << armName << kScriptDetailOpener << verdict.detail;
}

// The script's arms and the test's cases are the same set. Read from the script's OWN
// output, so an arm added there and not in the table is refused by its name — the one
// way a self-test arm could run on every leg and be a case of nothing.
inline void expectEveryScriptArmIsACaseAndEveryCaseAnArm(ScriptSelfTest const& entry, ScriptSelfTestRun const& run) {
    ASSERT_TRUE(run.started) << run.whyNot << "\n  command line: " << scriptCommandLine(entry);
    std::ostringstream unnamed;
    for (std::string const& line : run.unnamed) {
        unnamed << "\n  " << line;
    }
    EXPECT_TRUE(run.unnamed.empty()) << "the self-test gave a verdict for " << run.unnamed.size()
                                     << " arm(s) no case of this file names — add each to " << entry.tableName
                                     << ":" << unnamed.str();
    std::ostringstream wrong;
    int                wrongCount = 0;
    for (std::string_view const name : entry.armNames) {
        auto const found = run.arms.find(name);
        int const  seen  = found == run.arms.end() ? 0 : found->second.verdicts;
        if (seen != 1) {
            ++wrongCount;
            wrong << "\n  " << seen << " verdict(s): " << name;
        }
    }
    EXPECT_EQ(wrongCount, 0) << "the self-test did not give exactly one verdict to " << wrongCount
                             << " arm(s) this file holds a case for (it exited " << run.exitCode
                             << "):" << wrong.str();
    EXPECT_EQ(static_cast<std::size_t>(run.verdictLines), entry.armNames.size())
        << "the self-test printed " << run.verdictLines << " verdict line(s) for " << entry.armNames.size()
        << " case(s)";
}

// The script's own exit code says what its arms said: zero exactly when none failed. A
// script may also refuse something no arm reports — an arm count other than its own
// ratchet, a fatal error between two arms — which no arm's case could notice, and this
// one does.
inline void expectTheScriptsExitCodeAgreesWithItsArms(ScriptSelfTest const& entry, ScriptSelfTestRun const& run) {
    ASSERT_TRUE(run.started) << run.whyNot << "\n  command line: " << scriptCommandLine(entry);
    EXPECT_EQ(run.exitCode == 0, run.failedLines == 0)
        << "the self-test exited " << run.exitCode << " with " << run.failedLines << " FAILED verdict line(s) of "
        << run.verdictLines << ": an exit code that disagrees with the arms is the script refusing something no "
        << "arm reports — its standard error, above, says what.";
}

}  // namespace dss::test_support
