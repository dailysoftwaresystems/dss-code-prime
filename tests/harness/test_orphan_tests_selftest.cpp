// THE ORPHAN-TEST-SOURCE GUARD, ONE CASE PER VERDICT — the ctest entry `orphan_tests_guard`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-orphan-tests/check-orphan-tests.py`
// refuses a test source no CMake target compiles: every file under the tests root that a
// registration can name for a compiler is REGISTERED by a structural `dss_add_test` call,
// or exempted by a row the guard verifies on every run. Run with no argument it judges the
// tests root of the tree it lives in, and then — on a mirror of that root — proves each of
// its refusals can fire. Each of those judgements ends in ONE VERDICT LINE: one for the
// tree's own census, one per arm of the self-test.
//
// This binary RUNS that program — once, by the command line the top-level CMakeLists
// composed at configure — and reads each verdict into a case of its own. IT RE-IMPLEMENTS
// NO ARM. Everything it does beyond naming the verdicts is
// tests/test_support/script_selftest.hpp, which says why a script entry is driven this way
// and what the four parts of the pattern are.
//
// ★ IT IS THE ENTRY, NOT A SECOND ONE. The entry of this name was the program started bare
// by ctest: one verdict for everything it judges, and no test binary — so no mutation of
// the guard could be an arm of the mutation sweep. There is one entry and one run per leg.
//
// ★ A RED TREE IS ONE NAMED CASE, AND THE REST SAY WHY THEY DID NOT RUN. The program proves
// its refusals only on a tree whose own census is green — a red tree cannot seed the green
// mirror the arms mutate. So an orphan in the tree reddens `TheTestsRootsOwnCensus`, whose
// message is the guard's own digest of what it refused and whom it named, the whole report
// is in this binary's log (the program writes a red report to standard error), and every
// arm's case fails for having been given no verdict.
// ⓘ It carries the `repo-guard` label, set by hand beside its registration: what it judges
// is the source tree, which is the same bytes on every leg.

#include "dss_orphan_tests_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every verdict of a no-argument run: X(<the case>, "<the verdict's name, exactly as the
// program prints it>"). The order is the program's.
#define DSS_ORPHAN_TESTS_SELFTEST_ARMS(X)                                                            \
    X(TheTestsRootsOwnCensus, "LIVE TESTS-ROOT-CENSUS")                                              \
    X(GreenControl, "0 GREEN-CONTROL")                                                               \
    X(Orphan, "1 ORPHAN")                                                                            \
    X(CommentIsNotCoverage, "1b COMMENT-IS-NOT-COVERAGE")                                            \
    X(QuotedHashIsContent, "P1 QUOTED-HASH-IS-CONTENT")                                              \
    X(WholeWordCallName, "P2 WHOLE-WORD-CALL-NAME")                                                  \
    X(LowercaseKeywordsDoNotRegister, "P3 LOWERCASE-KEYWORDS-DO-NOT-REGISTER")                       \
    X(UppercaseCommandRegisters, "P4 UPPERCASE-COMMAND-REGISTERS")                                   \
    X(BracketCommentIsNotCoverage, "P5 BRACKET-COMMENT-IS-NOT-COVERAGE")                             \
    X(EscapedQuoteDoesNotToggle, "P6 ESCAPED-QUOTE-DOES-NOT-TOGGLE")                                 \
    X(EscapedHashIsNotAComment, "P7 ESCAPED-HASH-IS-NOT-A-COMMENT")                                  \
    X(CollapseEmptyRoot, "2 COLLAPSE-EMPTY-ROOT")                                                    \
    X(CollapseMissingRoot, "3 COLLAPSE-MISSING-ROOT")                                                \
    X(CollapseSourceFloor, "4 COLLAPSE-SOURCE-FLOOR")                                                \
    X(CollapseCmakelistsFloor, "5 COLLAPSE-CMAKELISTS-FLOOR")                                        \
    X(CollapseRegistrationFloor, "6 COLLAPSE-REGISTRATION-FLOOR")                                    \
    X(ParseIncompleteUnresolvedVariable, "E1 PARSE-INCOMPLETE-UNRESOLVED-VARIABLE")                  \
    X(ParseIncompleteAbsolutePath, "E2 PARSE-INCOMPLETE-ABSOLUTE-PATH")                              \
    X(ParseIncompleteOutsideTheRoot, "E3 PARSE-INCOMPLETE-OUTSIDE-THE-ROOT")                         \
    X(ParseIncompleteNoName, "E4 PARSE-INCOMPLETE-NO-NAME")                                          \
    X(ParseIncompleteNoSource, "E5 PARSE-INCOMPLETE-NO-SOURCE")                                      \
    X(ParseIncompleteUnterminatedCall, "E6 PARSE-INCOMPLETE-UNTERMINATED-CALL")                      \
    X(ARegistrationInABlockCMakeMayNotRunClearsNothing,                                              \
      "V1 A-REGISTRATION-IN-A-BLOCK-CMAKE-MAY-NOT-RUN-CLEARS-NOTHING")                               \
    X(ACMakeListsNoAddSubdirectoryReachesClearsNothing,                                              \
      "V2 A-CMAKELISTS-NO-ADD-SUBDIRECTORY-REACHES-CLEARS-NOTHING")                                  \
    X(AnAddSubdirectoryInABlockOrExcludedHoldsItsTree,                                               \
      "V3 AN-ADD-SUBDIRECTORY-IN-A-BLOCK-OR-EXCLUDED-HOLDS-ITS-TREE")                                \
    X(ABlockOrAReachThatCannotBeReadIsACollapse,                                                     \
      "V4 A-BLOCK-OR-A-REACH-THAT-CANNOT-BE-READ-IS-A-COLLAPSE")                                     \
    X(AnOptInRegistrationIsClearedByAnAllowlistRow,                                                  \
      "V5 AN-OPT-IN-REGISTRATION-IS-CLEARED-BY-AN-ALLOWLIST-ROW")                                    \
    X(StaleAllowFileGone, "7 STALE-ALLOW-FILE-GONE")                                                 \
    X(StaleAllowNowRegistered, "8 STALE-ALLOW-NOW-REGISTERED")                                       \
    X(StaleAllowSiteDroppedReference, "9 STALE-ALLOW-SITE-DROPPED-REFERENCE")                        \
    X(PhantomRegistration, "11 PHANTOM-REGISTRATION")                                                \
    X(StaleAllowSiteGone, "12 STALE-ALLOW-SITE-GONE")                                                \
    X(PrecedenceCollapseOutranksOrphanAndStale, "X1 PRECEDENCE-COLLAPSE-OUTRANKS-ORPHAN-AND-STALE")  \
    X(PrecedenceOrphanOutranksStale, "X2 PRECEDENCE-ORPHAN-OUTRANKS-STALE")                          \
    X(AllowlistMissingField, "A1 ALLOWLIST-MISSING-FIELD")                                           \
    X(AllowlistEmptyField, "A2 ALLOWLIST-EMPTY-FIELD")                                               \
    X(AllowlistUnknownKey, "A3 ALLOWLIST-UNKNOWN-KEY")                                               \
    X(AllowlistDuplicateSource, "A4 ALLOWLIST-DUPLICATE-SOURCE")                                     \
    X(RegisteredCSourceIsCleared, "K1 REGISTERED-C-SOURCE-IS-CLEARED")                               \
    X(UnregisteredCSourceIsAnOrphan, "K2 UNREGISTERED-C-SOURCE-IS-AN-ORPHAN")                        \
    X(RegisteredCSourceNotThereIsAPhantom, "K3 REGISTERED-C-SOURCE-NOT-THERE-IS-A-PHANTOM")          \
    X(RegisteredFileSpeltAsNoSourceIsUnwalked, "K4 REGISTERED-FILE-SPELT-AS-NO-SOURCE-IS-UNWALKED")  \
    X(EveryStatedExtensionIsWalkedAndJudged, "K5 EVERY-STATED-EXTENSION-IS-WALKED-AND-JUDGED")       \
    X(SiteTokenOfACSourceIsStructuralEvidence, "K6 SITE-TOKEN-OF-A-C-SOURCE-IS-STRUCTURAL-EVIDENCE") \
    X(RegisteredSourceSpeltInAnotherCaseIsMisspelt,                                                  \
      "K7 REGISTERED-SOURCE-SPELT-IN-ANOTHER-CASE-IS-MISSPELT")                                      \
    X(EveryExtensionCMakeTakesIsStated, "K8 EVERY-EXTENSION-CMAKE-TAKES-IS-STATED")                  \
    X(SubjectRootExemptsOnlyItsExtensions, "S1 SUBJECT-ROOT-EXEMPTS-ONLY-ITS-EXTENSIONS")            \
    X(SubjectRegisteredIsStale, "S2 SUBJECT-REGISTERED-IS-STALE")                                    \
    X(SubjectRootGoneIsStale, "S3 SUBJECT-ROOT-GONE-IS-STALE")                                       \
    X(SubjectExtensionHoldingNoFileIsStale, "S4 SUBJECT-EXTENSION-HOLDING-NO-FILE-IS-STALE")         \
    X(SubjectsExtensionNotStatedIsRefused, "S5 SUBJECTS-EXTENSION-NOT-STATED-IS-REFUSED")            \
    X(SubjectsRootInsideAnotherIsRefused, "S6 SUBJECTS-ROOT-INSIDE-ANOTHER-IS-REFUSED")              \
    X(SubjectsRootNotPlainIsRefused, "S7 SUBJECTS-ROOT-NOT-PLAIN-IS-REFUSED")                        \
    X(SubjectAnyTargetNamesIsStale, "S8 SUBJECT-ANY-TARGET-NAMES-IS-STALE")                          \
    X(GreenAfterRestore, "10 GREEN-AFTER-RESTORE")                                                   \
    X(CrashIsACollapse, "C1 CRASH-IS-A-COLLAPSE")                                                    \
    X(AFailingArmsEvidenceGoesToStandardError, "C2 A-FAILING-ARMS-EVIDENCE-GOES-TO-STANDARD-ERROR")  \
    X(UsageExtraOrEmptyArgumentRefused, "U1 USAGE-EXTRA-OR-EMPTY-ARGUMENT-REFUSED")                  \
    X(BomIsNotContent, "P8 BOM-IS-NOT-CONTENT")                                                      \
    X(OwnTreeFromItsOwnTree, "R root CONTROL: cwd = its own tree -> its own tree")                   \
    X(OwnTreeFromInsideAnotherRepository,                                                            \
      "R root: cwd inside ANOTHER git repository -> still its own tree")                             \
    X(OwnTreeFromInsideNoRepository, "R root: cwd inside NO git repository -> still its own tree")   \
    X(TempTreesRemoved, "T1 TEMP-TREES-REMOVED")

#define DSS_ORPHAN_TESTS_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_ORPHAN_TESTS_SELFTEST_ARMS(DSS_ORPHAN_TESTS_SELFTEST_ARM_NAME)};
#undef DSS_ORPHAN_TESTS_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::orphan_tests_selftest::kCommand,
    .workingDirectory = dss::orphan_tests_selftest::kWorkingDirectory,
    .standardOutput   = dss::orphan_tests_selftest::kStandardOutput,
    .marker           = "orphan-tests verdict: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_ORPHAN_TESTS_SELFTEST_ARMS",
};

// ONE run for the whole binary: the program builds its mirror once, and every case reads
// the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_ORPHAN_TESTS_SELFTEST_CASE(caseName, armName) \
    TEST(OrphanTestsSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_ORPHAN_TESTS_SELFTEST_ARMS(DSS_ORPHAN_TESTS_SELFTEST_CASE)
#undef DSS_ORPHAN_TESTS_SELFTEST_CASE

// The program's verdicts and this file's cases are the same set: an arm added to the
// program without a case here is a red, by the arm's name.
TEST(OrphanTestsSelfTest, EveryVerdictTheProgramGaveIsACaseAndEveryCaseAVerdict) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when a verdict said FAILED; a traceback between two
// arms, or an arm count other than its own ratchet, is an exit code no verdict's case
// reports, and this one does.
TEST(OrphanTestsSelfTest, TheProgramsExitCodeAgreesWithItsVerdicts) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
