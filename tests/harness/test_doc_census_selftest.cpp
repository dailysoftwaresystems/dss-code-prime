// THE DOCUMENTED-FIGURE CENSUS'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry
// `doc_census_selftest_guard`.
//
// WHAT THIS IS. `.harness-config/runner/actions/check-doc-census/check-doc-census.py` holds every
// documented figure to the census that measures it, and every quantified claim about the corpus in
// config prose to its own measurement; its walk of the tree leaves out what git ignores there, asked
// of the one reader (`owning-tree`'s `TreeIgnores`). Run with `--selftest` it proves each refusal and
// each exclusion in both directions on trees of its own, and prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp.
//
// ★ IT IS THE ENTRY, NOT A SECOND ONE. Until 2026-10-15 the program was started bare by ctest: one
// verdict for every arm, so no mutation of it could be an arm of the mutation sweep. The census of the
// tree itself stays the bare entry `doc_census_guard`: one verdict, one subject.
// ⓘ NO `repo-guard` label (it carried one as an entry of the top directory until 2026-10-15): it
// SELF-TESTS a program, whose verdict can depend on the host -- the rule is stated where the label is
// given, in the top-level CMakeLists -- so every leg runs it, the indirect ones included.

#include "dss_doc_census_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_DOC_CENSUS_SELFTEST_ARMS(X)                                                                              \
    X(Arm0ControlAgrees, "0 CONTROL-AGREES")                                                                           \
    X(Arm1FigureDrifted, "1 FIGURE-DRIFTED")                                                                           \
    X(Arm1bRemedyIsTheHarnessStep, "1b REMEDY-IS-THE-HARNESS-STEP")                                                    \
    X(Arm2WriteRepairs, "2 WRITE-REPAIRS")                                                                             \
    X(Arm2bVerifiesAfterWrite, "2b VERIFIES-AFTER-WRITE")                                                              \
    X(Arm2cSetup, "2c-setup")                                                                                          \
    X(Arm2cWrite, "2c-write")                                                                                          \
    X(Arm2cSeparatorStyle, "2c SEPARATOR-STYLE")                                                                       \
    X(Arm3UnknownKey, "3 UNKNOWN-KEY")                                                                                 \
    X(Arm4UnknownProvider, "4 UNKNOWN-PROVIDER")                                                                       \
    X(Arm5NoNumberAfterMarker, "5 NO-NUMBER-AFTER-MARKER")                                                             \
    X(Arm6FloorCollapse, "6 FLOOR-COLLAPSE")                                                                           \
    X(Arm7ProviderFailed, "7 PROVIDER-FAILED")                                                                         \
    X(Arm8ProviderEmpty, "8 PROVIDER-EMPTY")                                                                           \
    X(Arm9ProviderNotJson, "9 PROVIDER-NOT-JSON")                                                                      \
    X(Arm10NoDocuments, "10 NO-DOCUMENTS")                                                                             \
    X(Arm11aHarnessCopiesUndeclaredRed, "11a HARNESS-COPIES-UNDECLARED-RED")                                           \
    X(Arm11WorktreeIgnored, "11 WORKTREE-IGNORED")                                                                     \
    X(Arm11bOrchestratorsIgnored, "11b ORCHESTRATORS-IGNORED")                                                         \
    X(Arm11cLegCopyIgnored, "11c LEG-COPY-IGNORED")                                                                    \
    X(Arm11dMutationWorkerIgnored, "11d MUTATION-WORKER-IGNORED")                                                      \
    X(Arm12LiveTreeStillCaught, "12 LIVE-TREE-STILL-CAUGHT")                                                           \
    X(Arm13QuotationIgnored, "13 QUOTATION-IGNORED")                                                                   \
    X(Arm14QuotationKeyStillChecked, "14 QUOTATION-KEY-STILL-CHECKED")                                                 \
    X(Arm15LiveClaimBesideQuotation, "15 LIVE-CLAIM-BESIDE-QUOTATION")                                                 \
    X(Arm16ScratchUndeclaredReds, "16 SCRATCH-UNDECLARED-REDS")                                                        \
    X(Arm17ScratchDeclaredIgnored, "17 SCRATCH-DECLARED-IGNORED")                                                      \
    X(Arm18LiveTreeStillCaughtBesideScratch, "18 LIVE-TREE-STILL-CAUGHT-BESIDE-SCRATCH")                               \
    X(Arm19LaterNegationReIncludes, "19 LATER-NEGATION-RE-INCLUDES")                                                   \
    X(Arm20SourceFigureStaleReds, "20 SOURCE-FIGURE-STALE-REDS")                                                       \
    X(Arm21SourceFigureCorrectPasses, "21 SOURCE-FIGURE-CORRECT-PASSES")                                               \
    X(Arm22Setup, "22-setup")                                                                                          \
    X(Arm22Write, "22-write")                                                                                          \
    X(Arm22WriteRepairsSource, "22 WRITE-REPAIRS-SOURCE")                                                              \
    X(Arm23SourceUnknownProvider, "23 SOURCE-UNKNOWN-PROVIDER")                                                        \
    X(Arm24SourceUnknownKey, "24 SOURCE-UNKNOWN-KEY")                                                                  \
    X(Arm25SourceFloorCollapse, "25 SOURCE-FLOOR-COLLAPSE")                                                            \
    X(Arm26ProviderControlCounts, "26 PROVIDER-CONTROL-COUNTS")                                                        \
    X(Arm27ProviderGlobMatchesNothing, "27 PROVIDER-GLOB-MATCHES-NOTHING")                                             \
    X(Arm28ClaimUnpinnedReds, "28 CLAIM-UNPINNED-REDS")                                                                \
    X(Arm29ClaimDatePinPasses, "29 CLAIM-DATE-PIN-PASSES")                                                             \
    X(Arm30ClaimRevisionPinPasses, "30 CLAIM-REVISION-PIN-PASSES")                                                     \
    X(Arm31ClaimMeasuredWordAloneReds, "31 CLAIM-MEASURED-WORD-ALONE-REDS")                                            \
    X(Arm32ClaimDateAloneReds, "32 CLAIM-DATE-ALONE-REDS")                                                             \
    X(Arm33ClaimHistoryPasses, "33 CLAIM-HISTORY-PASSES")                                                              \
    X(Arm34InstanceEReplantedReds, "34 INSTANCE-E-REPLANTED-REDS")                                                     \
    X(Arm35UsageNounPasses, "35 USAGE-NOUN-PASSES")                                                                    \
    X(Arm36UsageVerbReds, "36 USAGE-VERB-REDS")                                                                        \
    X(Arm37NonDollarValuePasses, "37 NON-DOLLAR-VALUE-PASSES")                                                         \
    X(Arm38DollarSubtreeReds, "38 DOLLAR-SUBTREE-REDS")                                                                \
    X(Arm39WindowEdgeReds, "39 WINDOW-EDGE-REDS")                                                                      \
    X(Arm40WindowPastEdgePasses, "40 WINDOW-PAST-EDGE-PASSES")                                                         \
    X(Arm41NoConfigCollapses, "41 NO-CONFIG-COLLAPSES")                                                                \
    X(Arm41bPartialCollapseCollapses, "41b PARTIAL-COLLAPSE-COLLAPSES")                                                \
    X(Arm42ConfigNotJsonCollapses, "42 CONFIG-NOT-JSON-COLLAPSES")                                                     \
    X(Arm43AnchoredIgnoreSkipsOneDir, "43 ANCHORED-IGNORE-SKIPS-ONE-DIR")                                              \
    X(Arm44RelativeThatIsNoArticle, "44 RELATIVE-THAT-IS-NO-ARTICLE")                                                  \
    X(Arm45BacktickedCorpusWord, "45 BACKTICKED-CORPUS-WORD")                                                          \
    X(Arm46PossessiveCorpusWord, "46 POSSESSIVE-CORPUS-WORD")                                                          \
    X(Arm47PathCorpusWordByBasename, "47 PATH-CORPUS-WORD-BY-BASENAME")                                                \
    X(Arm48EmphasizedCorpusWord, "48 EMPHASIZED-CORPUS-WORD")                                                          \
    X(Arm49IncludeIsAUsageVerb, "49 INCLUDE-IS-A-USAGE-VERB")                                                          \
    X(Arm50HistoryAfterTheClaim, "50 HISTORY-AFTER-THE-CLAIM")                                                         \
    X(Arm51BareVerifiedIsNoPin, "51 BARE-VERIFIED-IS-NO-PIN")                                                          \
    X(Arm52DigitsAreNoRevision, "52 DIGITS-ARE-NO-REVISION")                                                           \
    X(Arm53NegatedMeasuredIsNoPin, "53 NEGATED-MEASURED-IS-NO-PIN")                                                    \
    X(Arm54CapitalMeasuredDatePasses, "54 CAPITAL-MEASURED-DATE-PASSES")                                               \
    X(Arm55ReMeasuredRevisionPasses, "55 RE-MEASURED-REVISION-PASSES")

#define DSS_DOC_CENSUS_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_DOC_CENSUS_SELFTEST_ARMS(DSS_DOC_CENSUS_SELFTEST_ARM_NAME)};
#undef DSS_DOC_CENSUS_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::doc_census_selftest::kCommand,
    .workingDirectory = dss::doc_census_selftest::kWorkingDirectory,
    .standardOutput   = dss::doc_census_selftest::kStandardOutput,
    .marker           = "check-doc-census self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_DOC_CENSUS_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_DOC_CENSUS_SELFTEST_CASE(caseName, armName) \
    TEST(DocCensusSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_DOC_CENSUS_SELFTEST_ARMS(DSS_DOC_CENSUS_SELFTEST_CASE)
#undef DSS_DOC_CENSUS_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(DocCensusSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(DocCensusSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
