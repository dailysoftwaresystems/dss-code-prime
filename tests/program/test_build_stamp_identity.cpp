// THE BUILD STAMP SCRIPT'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
// `build/build_stamp_identity`.
//
// WHAT THIS IS. `cmake/DssBuildStamp.cmake` computes the compiler's build stamp, and
// run with `-DDSS_STAMP_MODE=selftest` it proves its own properties in throwaway
// trees: what moves the value without git and with it, what a dependency's sources a
// configure was HANDED add to it, what is refused; and it configures fixture projects
// against `cmake/DssBuildStampDeclaration.cmake`, the configure-time check of the
// declaration. Each of those proofs is an ARM, and the script prints one verdict line
// per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level
// CMakeLists composed at configure — and reads each arm's verdict into a case of its
// own. IT RE-IMPLEMENTS NO ARM: an arm's logic, its fixtures and its message live in
// the script, and a case here says nothing but "the script's arm of this name ran, and
// said ok".
//
// ★ WHY IT EXISTS, when the entry was a bare `cmake -P` line that already failed on a
// red arm. A script entry has ONE verdict and no cases. The mutation sweep
// (`tests/mutations/arms.registry`) starts a TEST BINARY whole and judges the exact set
// of cases that go red, so a mutation of either stamp module could not be an arm while
// its only reader was a script: each had to be applied by hand, one harness call per
// mutant. With one case per arm, a mutant of the stamp or of its configure guard
// reddens exactly the cases of the arms that notice it.
//
// ★★ THE TWO TABLES CANNOT DRIFT APART IN SILENCE. The arms are named in the script
// and again below, so a totality case reads the script's own output and refuses an arm
// no case names, a case whose arm did not run, and an arm that ran twice. Adding an
// arm to the script without a case here is a red, by the arm's name.
//
// ★ IT FAILS, IT NEVER SKIPS. If the command line cannot be started, or its output
// cannot be read, EVERY case fails with the reason: a self-test that did not run has
// proven nothing, and a skip would read as a pass on a host with no cmake on its path.
//
// The command line, its working directory and the file its standard output goes to
// come from a header generated at configure, so a copy of this tree configured where
// it stands runs ITS OWN modules — which is what lets a worker copy's mutated module
// be the one this binary reads.

#include "dss_build_stamp_selftest_command.hpp"

#include "core/substrate/process_spawn.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the script
// hands it to `_dss_t_arm`>"). The order is the script's.
#define DSS_BUILD_STAMP_SELFTEST_ARMS(X)                                                             \
    X(NoGitAnUnchangedTreeStampsTheSameValueTwiceAndLeavesTheHeaderAlone,                            \
      "no git: an unchanged tree stamps the same value twice, and the second run leaves the header alone") \
    X(NoGitTheValueIsVersionPlusSrcSixteenHexOneToken,                                               \
      "no git: the value is <VERSION>+src<16 hex>, one token")                                       \
    X(NoGitAByteAddedToACompilerSourceMovesIt,                                                       \
      "no git: a byte added to a compiler source moves it")                                          \
    X(NoGitANewFileAmongTheCompilerSourcesMovesIt,                                                   \
      "no git: a new file among the compiler sources moves it")                                      \
    X(NoGitACompilerSourceRenamedMovesIt,                                                            \
      "no git: a compiler source renamed, its bytes unchanged, moves it")                            \
    X(NoGitATestsSourceADocumentAPlanAndAHarnessFileLeaveIt,                                         \
      "no git: a test's source, a document, a plan and a harness file leave it")                     \
    X(NoGitADeclaredRecordAndCheckerLeaveIt,                                                         \
      "no git: a declared record and checker under a whole input leave it")                          \
    X(NoGitAnUndeclaredFileBesideThemMovesIt,                                                        \
      "no git: an UNDECLARED file beside them is read, and moves it")                                \
    X(NoGitACMakeScriptUnderATestDirectoryMovesIt,                                                   \
      "no git: a CMake script under a test directory moves it")                                      \
    X(NoGitTheVersionFileIsAnInput,                                                                  \
      "no git: the VERSION file is an input")                                                        \
    X(GitATreeWhoseInputsMatchHeadStampsVersionPlusCommit,                                           \
      "git: a tree whose inputs match HEAD stamps <VERSION>+g<12 hex>")                              \
    X(GitDirtOutsideTheInputsLeavesItClean,                                                          \
      "git: dirt outside the inputs (a test's source, documents, a plan, a harness file) leaves it clean") \
    X(GitAnEditedDeclaredRecordAndCheckerLeaveItClean,                                               \
      "git: an edited declared record and checker leave it clean")                                   \
    X(GitAnUndeclaredFileBesideThemMakesItDirty,                                                     \
      "git: an UNDECLARED file beside them makes it dirty")                                          \
    X(GitAnEditedCompilerSourceMakesItDirty,                                                         \
      "git: an edited compiler source makes it .dirty<16 hex>")                                      \
    X(GitAnUntrackedFileAmongTheCompilerSourcesMakesItDirty,                                         \
      "git: an untracked file among the compiler sources makes it dirty")                            \
    X(GitACMakeScriptUnderATestDirectoryMakesItDirty,                                                \
      "git: a CMake script under a test directory makes it dirty")                                   \
    X(HandedADependencyAddsTheDepsComponentTheSameTwice,                                             \
      "handed: a handed dependency adds .deps<16 hex> to the value the tree has without it, the same twice") \
    X(HandedNothingStampsNoDepsComponent,                                                            \
      "handed: a configure handed nothing stamps the value with no .deps component")                 \
    X(HandedTheSameBytesFromAnotherDirectoryStampTheSameValue,                                       \
      "handed: the same bytes handed from another directory stamp the same value")                   \
    X(HandedAClonesOwnGitDirectoryLeavesIt,                                                          \
      "handed: a clone's own .git under the handed directory leaves it")                             \
    X(HandedAByteAddedToADependencysSourceMovesTheDepsComponentAndOnlyIt,                            \
      "handed: a byte added to a handed dependency's source moves the .deps component, and only it") \
    X(HandedTheSameBytesUnderAnotherFileNameStampAnotherValue,                                       \
      "handed: the same bytes under another file name stamp another value")                          \
    X(HandedTheSameBytesAsAnotherDependencyStampAnotherValue,                                        \
      "handed: the same bytes handed as ANOTHER dependency stamp another value")                     \
    X(HandedAWorkTreeCarriesTheSameDepsComponent,                                                    \
      "handed: a work tree carries the same .deps component after its own commit and dirt")          \
    X(HandedAHandOverThatNamesNoDirectoryIsRefused,                                                  \
      "handed: a hand-over that names no directory is refused, by the dependency's name")            \
    X(HandedAHandOverThatIsNoPairIsRefused,                                                          \
      "handed: a hand-over that is no NAME=directory pair is refused as written")                    \
    X(HandedARunNotToldWhatItsConfigureWasHandedIsRefused,                                           \
      "handed: a run not told what its configure was handed is refused, and writes no header")       \
    X(TheRealTreesDeclaredInputsAllExistAndStampItWithoutGit,                                        \
      "the real tree's declared inputs all exist and stamp it without git")                          \
    X(ConfigureEveryDirectoryUnderTheDeclarationPasses,                                              \
      "configure: every directory it processes lies under the declaration, so it passes")            \
    X(ConfigureADirectoryUnderNoDeclarationIsRefused,                                                \
      "configure: a directory under no declaration is refused, by its name")                         \
    X(ConfigureOneAddedFromInsideADeclaredDirectoryIsRefusedToo,                                     \
      "configure: one added from inside a declared directory is refused too -- the walk descends")   \
    X(ConfigureADirectoryOutsideTheSourceTreeIsNotJudged,                                            \
      "configure: a directory outside the source tree, where a fetched dependency lives, is not judged") \
    X(ConfigureHandedSourcesInsideTheSourceTreeAreNotJudged,                                         \
      "configure: a dependency's sources it was handed, held inside the source tree, are not judged") \
    X(ConfigureTheSameDirectoriesWithNoHandOverAreRefused,                                           \
      "configure: the same directories with no hand-over are refused, each by its name")             \
    X(ConfigureAnEmptyHandOverEntryHandsNothingOver,                                                 \
      "configure: an EMPTY hand-over entry hands nothing over, so an undeclared directory is still refused") \
    X(ConfigureAHandedDependencyExcusesItsOwnDirectoriesOnly,                                        \
      "configure: a handed dependency excuses its own directories only -- one beside it is refused, alone") \
    X(ConfigureAHandOverRecordedAfterTheStampWasWiredIsRefused,                                      \
      "configure: a hand-over recorded after the stamp was wired is refused, by the dependency's name") \
    X(ConfigureACheckNotToldWhatTheStampWasWiredWithIsRefused,                                       \
      "configure: a check not told what the stamp was wired with is refused, naming the variable")   \
    X(ConfigureAHandOverWhosePathHoldsASeparatorIsRefused,                                           \
      "configure: a hand-over whose path holds a separator of the stamp's list is refused, by name") \
    X(ADeclaredInputThatNamesNothingIsRefused,                                                       \
      "a declared input that names nothing is refused, by its name")                                 \
    X(AnInputsDeclarationThatWasNotPassedIsRefused,                                                  \
      "an inputs declaration that was not passed is refused")                                        \
    X(ADeclaredRecordThatNamesNoFileIsRefused,                                                       \
      "a declared record that names no file is refused, by its name")                                \
    X(ARecordOutsideEveryDirectoryReadWholeIsRefused,                                                \
      "a record outside every directory read whole is refused")

#define DSS_BUILD_STAMP_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_BUILD_STAMP_SELFTEST_ARMS(DSS_BUILD_STAMP_SELFTEST_ARM_NAME)};
#undef DSS_BUILD_STAMP_SELFTEST_ARM_NAME

// What the script prints before each verdict: `message(STATUS ...)` puts `-- ` in
// front of it, which is not matched, so the line is found wherever the marker stands.
constexpr std::string_view kMarker       = "dss-build-stamp selftest: ";
constexpr std::string_view kDetailOpener = " -- ";

struct ArmVerdict {
    int         verdicts = 0;      // how many verdict lines named this arm
    bool        ok       = false;  // what the last of them said
    std::string detail;            // what a FAILED line said after the arm's name
};

struct SelfTestRun {
    bool        started  = false;  // the command line ran to an exit code AND its output was read
    int         exitCode = 0;
    std::string whyNot;            // non-empty exactly when `started` is false
    std::map<std::string, ArmVerdict, std::less<>> arms;     // every name of kArmNames is a key
    std::vector<std::string>                       unnamed;  // verdict lines that name no arm of the table
    int         verdictLines = 0;  // ok and FAILED lines, named or not
    int         failedLines  = 0;  // FAILED lines, named or not
};

[[nodiscard]] std::string commandLineForMessages() {
    std::string out;
    for (char const* argument : dss::build_stamp_selftest::kCommand) {
        if (!out.empty()) {
            out += ' ';
        }
        out += argument;
    }
    return out;
}

// The arm of the table that `text` names: `text` IS the name (an `ok` line), or opens
// with it followed by the detail opener (a FAILED line). Two names hold ` -- `
// themselves, so the split is never guessed from the text: the LONGEST name that fits
// is the arm.
[[nodiscard]] std::string_view armNamedBy(std::string_view text) {
    std::string_view best;
    for (std::string_view const name : kArmNames) {
        bool const fits =
            text == name
            || (text.size() >= name.size() + kDetailOpener.size() && text.substr(0, name.size()) == name
                && text.substr(name.size(), kDetailOpener.size()) == kDetailOpener);
        if (fits && name.size() > best.size()) {
            best = name;
        }
    }
    return best;
}

void readVerdictLine(std::string_view line, SelfTestRun& run) {
    std::size_t const at = line.find(kMarker);
    if (at == std::string_view::npos) {
        return;
    }
    std::string_view rest = line.substr(at + kMarker.size());
    bool             ok   = false;
    if (rest.substr(0, 3) == "ok ") {
        ok   = true;
        rest = rest.substr(3);
    } else if (rest.substr(0, 7) == "FAILED ") {
        rest = rest.substr(7);
    } else {
        return;  // the timing line, the closing count: no arm's verdict
    }
    while (!rest.empty() && rest.front() == ' ') {
        rest.remove_prefix(1);
    }
    ++run.verdictLines;
    if (!ok) {
        ++run.failedLines;
    }
    std::string_view const name = armNamedBy(rest);
    if (name.empty()) {
        run.unnamed.emplace_back(rest);
        return;
    }
    ArmVerdict& verdict = run.arms[std::string(name)];
    ++verdict.verdicts;
    verdict.ok = ok;
    verdict.detail =
        rest.size() > name.size() ? std::string(rest.substr(name.size() + kDetailOpener.size())) : std::string();
}

[[nodiscard]] SelfTestRun runTheSelfTest() {
    SelfTestRun run;
    for (std::string_view const name : kArmNames) {
        run.arms.emplace(std::string(name), ArmVerdict{});
    }
    std::vector<std::string> argv;
    for (char const* argument : dss::build_stamp_selftest::kCommand) {
        argv.emplace_back(argument);
    }
    std::filesystem::path const output = dss::build_stamp_selftest::kStandardOutput;
    // Standard output goes to a file, which the script's verdict lines are read from;
    // standard error — a refusal's own text, the closing count of a red run — stays
    // this process's, so it is in the log beside the cases it explains.
    dss::substrate::SpawnResult const spawned = dss::substrate::spawnAndWaitRedirectStdout(
        argv, std::filesystem::path(dss::build_stamp_selftest::kWorkingDirectory), output);
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
        readVerdictLine(line, run);
        if (end == std::string::npos) {
            break;
        }
        from = end + 1;
    }
    run.started = true;
    return run;
}

// ONE run for the whole binary: the self-test builds its fixtures once, and every case
// reads the same output.
[[nodiscard]] SelfTestRun const& theRun() {
    static SelfTestRun const run = runTheSelfTest();
    return run;
}

// The one assertion every arm's case makes. A red says what the SCRIPT said: the arm's
// name and its own detail, in the words of the script's own FAILED line.
void expectTheArmPassed(std::string_view armName) {
    SelfTestRun const& run = theRun();
    ASSERT_TRUE(run.started) << run.whyNot << "\n  command line: " << commandLineForMessages();
    auto const found = run.arms.find(armName);
    ASSERT_TRUE(found != run.arms.end()) << "no arm of this name in the table: " << armName;
    ArmVerdict const& verdict = found->second;
    ASSERT_EQ(verdict.verdicts, 1)
        << "the self-test gave this arm " << verdict.verdicts << " verdict(s), not one (it exited " << run.exitCode
        << " after " << run.verdictLines << " verdict line(s)): " << armName;
    EXPECT_TRUE(verdict.ok) << kMarker << "FAILED  " << armName << kDetailOpener << verdict.detail;
}

}  // namespace

#define DSS_BUILD_STAMP_SELFTEST_CASE(caseName, armName) \
    TEST(BuildStampIdentity, caseName) { expectTheArmPassed(armName); }
DSS_BUILD_STAMP_SELFTEST_ARMS(DSS_BUILD_STAMP_SELFTEST_CASE)
#undef DSS_BUILD_STAMP_SELFTEST_CASE

// The script's arms and this file's cases are the same set. Read from the script's
// OWN output, so an arm added there and not here is refused by its name — the one way
// a self-test arm could run on every leg and be a case of nothing.
TEST(BuildStampIdentity, EveryArmTheSelfTestRanIsACaseAndEveryCaseAnArm) {
    SelfTestRun const& run = theRun();
    ASSERT_TRUE(run.started) << run.whyNot << "\n  command line: " << commandLineForMessages();
    std::ostringstream unnamed;
    for (std::string const& line : run.unnamed) {
        unnamed << "\n  " << line;
    }
    EXPECT_TRUE(run.unnamed.empty())
        << "the self-test gave a verdict for " << run.unnamed.size()
        << " arm(s) no case of this file names — add each to DSS_BUILD_STAMP_SELFTEST_ARMS:" << unnamed.str();
    std::ostringstream wrong;
    int                wrongCount = 0;
    for (std::string_view const name : kArmNames) {
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
    EXPECT_EQ(static_cast<std::size_t>(run.verdictLines), std::size(kArmNames))
        << "the self-test printed " << run.verdictLines << " verdict line(s) for " << std::size(kArmNames)
        << " case(s)";
}

// The script's own exit code says what its arms said: zero exactly when none failed.
// It also fails on an arm COUNT other than its own ratchet, with every arm green —
// which no arm's case could notice, and this one does.
TEST(BuildStampIdentity, TheSelfTestsExitCodeAgreesWithItsArms) {
    SelfTestRun const& run = theRun();
    ASSERT_TRUE(run.started) << run.whyNot << "\n  command line: " << commandLineForMessages();
    EXPECT_EQ(run.exitCode == 0, run.failedLines == 0)
        << "the self-test exited " << run.exitCode << " with " << run.failedLines << " FAILED verdict line(s) of "
        << run.verdictLines << ": an exit code that disagrees with the arms is the script refusing something no "
        << "arm reports (its own arm-count ratchet, a fatal error between two arms) — its standard error, above, "
        << "says what.";
}
