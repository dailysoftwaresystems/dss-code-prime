// THE TREE OWNER'S OWN SELF-TEST, ONE CASE PER ARM — the ctest entry `owning_tree_selftest_guard`.
//
// WHAT THIS IS. `.harness-config/runner/actions/owning-tree/owning-tree.py` is the Python owner of
// "which DSS tree does this program belong to", of asking git about a tree without the caller's git
// environment, and of "what does a walk of the tree leave out" — what git ignores there, read from the
// tree's own `.gitignore` (`TreeIgnores`), the one answer every program that walks a tree from its root
// asks — and of what a path a command line names for writing may be (`written_path`: never one that
// begins with `-`). Run with `--self-test` it proves each answer, and that its root arms can fail, and
// prints one verdict line per arm.
//
// This binary RUNS that self-test — once, by the command line the top-level CMakeLists composed at
// configure — and reads each arm's verdict into a case of its own. IT RE-IMPLEMENTS NO ARM. Everything
// it does beyond naming the arms is tests/test_support/script_selftest.hpp, which says why a script
// entry is driven this way and what the four parts of the pattern are.
//
// ★ IT IS THE ENTRY, NOT A SECOND ONE. Until 2026-10-15 the program was started bare by ctest: one
// verdict for every arm, and no test binary — so no mutation of it could be an arm of the mutation
// sweep. There is one entry and one run per leg.
// ⓘ NO `repo-guard` label (it carried one as an entry of the top directory until 2026-10-15): it
// SELF-TESTS a program, whose verdict can depend on the host -- the rule is stated where the label is
// given, in the top-level CMakeLists -- so every leg runs it, the indirect ones included.

#include "dss_owning_tree_selftest_command.hpp"

#include "script_selftest.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

// Every arm of the self-test: X(<the case>, "<the arm's name, exactly as the program prints it>").
// The order is the program's. GENERATED from the program's own verdict lines; an arm added to the
// program without a row here is a red of the set case below, by the arm's name.
#define DSS_OWNING_TREE_SELFTEST_ARMS(X)                                                                             \
    X(W1TheWalkNamesTheDirectoryHoldingBothPlansAndHarnessConfig,                                                      \
      "W1 the walk names the directory holding BOTH .plans/ and .harness-config/")                                     \
    X(W2PlansWithoutHarnessConfigIsNotATreeTheWalkContinuesToTheRealOne,                                               \
      "W2 .plans/ WITHOUT .harness-config/ is not a tree -- the walk continues to the real one")                       \
    X(W3HarnessConfigWithoutPlansIsNotATreeTheWalkContinuesToTheRealOne,                                               \
      "W3 .harness-config/ WITHOUT .plans/ is not a tree -- the walk continues to the real one")                       \
    X(W4ATreeCopiedInsideAnotherTreeIsItsOwnTreeTheNearestOneWins,                                                     \
      "W4 a tree copied INSIDE another tree is its own tree -- the NEAREST one wins")                                  \
    X(W5APathUnderNoTreeIsRefusedNamingWhatTheWalkLookedFor,                                                           \
      "W5 a path under NO tree is REFUSED, naming what the walk looked for")                                           \
    X(W6AnEmptyBlankOrNonePathIsAUsageRefusalAndAMissingPathIsRefusedNeverTheCallerSCwd,                               \
      "W6 an EMPTY, blank or None path is a UsageRefusal and a MISSING path is refused -- never the caller's cwd")     \
    X(R1ReadsGitATreeThatIsItsOwnGitTopLevelResolves, "R1 reads_git: a tree that IS its own git top level resolves")   \
    X(R2ReadsGitATreeNestedInsideAnotherCheckoutIsRefusedGitAnswersForTheEnclosingOne,                                 \
      "R2 reads_git: a tree nested inside another checkout is REFUSED -- git answers for the enclosing one")           \
    X(U1RunGitIgnoresACallerSGitDirGitWorkTreeGitIndexFile,                                                            \
      "U1 run_git IGNORES a caller's GIT_DIR + GIT_WORK_TREE + GIT_INDEX_FILE")                                        \
    X(U2RunUnsteeredTheChildSeesNoneOfTheThreeNamesItsExitCodePassesThroughAndTheCallerSEnviro,                        \
      "U2 run_unsteered: the child sees none of the three names, its exit code passes through, and the CALLER's environment is untouched") \
    X(U3RunUnsteeredRefusesAnEmptyArgvAndAnEmptyProgram,                                                               \
      "U3 run_unsteered REFUSES an empty argv and an empty program")                                                   \
    X(U4AnEmptyGitIndexFileIsRemovedTooAnEmptyValueStillSteersGit,                                                     \
      "U4 an EMPTY GIT_INDEX_FILE is removed too -- an empty value still steers git")                                  \
    X(A1RootArmsAgainstThisOwnerSResolverControlAnotherRepositoryNoRepositoryASteeringEnvironm,                        \
      "A1 root_arms against THIS owner's resolver: control, another repository, no repository, a steering environment and a nested copy all hold") \
    X(A2RootArmsCanFailACwdKeyedResolverPassesOnlyTheControl,                                                          \
      "A2 root_arms CAN FAIL: a cwd-keyed resolver passes only the CONTROL")                                           \
    X(A3RootArmsCanFailOnARelativeAnswerPassesOnlyTheArmsAskedFromTheTreeItself,                                       \
      "A3 root_arms CAN FAIL on a RELATIVE answer: `.` passes only the arms asked from the tree itself")               \
    X(A4RootArmsCanFailAResolverThatSkipsGitSAgreementFailsOnlyTheNestedCopyArm,                                       \
      "A4 root_arms CAN FAIL: a resolver that skips git's agreement fails only the nested-copy arm")                   \
    X(A5RootArmsCanFailGitSAgreementAskedInTheCallerSEnvironmentFailsOnlyTheSteeringArm,                               \
      "A5 root_arms CAN FAIL: git's agreement asked in the CALLER's environment fails only the steering arm")          \
    X(I1IdentityOfAPlainCheckoutGitAnswersAsItStandsNoGitdirHeadShaAndBranch,                                          \
      "I1 identity of a plain checkout: git answers as it stands (no gitdir), HEAD sha and branch")                    \
    X(I2IdentityOfADetachedWorktreeAGitFileItsHeadBranchReportedAsHead,                                                \
      "I2 identity of a detached worktree (a `.git` FILE): its HEAD, branch reported as HEAD")                         \
    X(I3AForeignAbsoluteGitdirIsJudgedByTheHostTranslatedWithWslpathOrRefusedByNameNeverJoined,                        \
      "I3 a FOREIGN absolute gitdir is judged by the HOST: translated with wslpath or refused by name, never joined under the worktree") \
    X(I4ARelativeGitdirResolvesAgainstTheWorktreeADecoyOfTheSameNameBesideTheProcessSCwdIsNotC,                        \
      "I4 a RELATIVE gitdir resolves against the WORKTREE -- a decoy of the same name beside the process's cwd is not consulted") \
    X(I5IdentityOfAPlainDirectoryIsRefused, "I5 identity of a plain directory is REFUSED")                             \
    X(I6AGitFileNamingAGitdirThatDoesNotExistIsRefused,                                                                \
      "I6 a `.git` file naming a gitdir that does not exist is REFUSED")                                               \
    X(I7IdentityIsAUsageRefusal, "I7 identity('') is a UsageRefusal")                                                  \
    X(G1TreeGitIgnoresAGitIndexFileNamingAnotherRepositorySIndex,                                                      \
      "G1 tree_git ignores a GIT_INDEX_FILE naming another repository's index")                                        \
    X(G2TreeGitIgnoresAGitDirNamingAnotherRepository, "G2 tree_git ignores a GIT_DIR naming another repository")       \
    X(G3AFailingGitItsExitCodeAndStderrComeBackAndTreeGitLinesRefusesItUnlessTheCallerDeclared,                        \
      "G3 a failing git: its exit code and stderr come back, and tree_git_lines REFUSES it unless the caller declared that code acceptable") \
    X(G4TreeGitWithoutAnIdentityIsAUsageRefusal, "G4 tree_git without an identity is a UsageRefusal")                  \
    X(O1StandingInAnotherTreeResolvePathReadsTheIdentitySBytes,                                                        \
      "O1 standing in ANOTHER tree, resolve_path reads the identity's bytes")                                          \
    X(O2ControlABareRelativeReadFromThereReadsTheOtherTreeSBytes,                                                      \
      "O2 CONTROL: a bare relative read from there reads the OTHER tree's bytes")                                      \
    X(O3AssertOneRootCheckCwdRefusesAProcessStandingInAnotherTree,                                                     \
      "O3 assert_one_root(check_cwd) REFUSES a process standing in another tree")                                      \
    X(O4EnterIdentityChangeIntoItAndTheTwoRootsAgree,                                                                  \
      "O4 enter(): identity, change into it, and the two roots agree")                                                 \
    X(O5ResolvePathRelativeUnderTheRootAbsoluteUnchangedEmptyRefused,                                                  \
      "O5 resolve_path: relative under the root, absolute unchanged, empty refused")                                   \
    X(T1OwningRootControlADssTreeThatIsItsOwnCheckout,                                                                 \
      "T1 owning_root CONTROL: a DSS tree that is its own checkout")                                                   \
    X(T2OwningRootUnderACallerSGitDirNamingAnotherRepositoryStillItsOwn,                                               \
      "T2 owning_root under a caller's GIT_DIR naming another repository -> still its own")                            \
    X(T3OwningRootUnderGitDirGitWorkTreeNamingAnotherRepositoryStillItsOwn,                                            \
      "T3 owning_root under GIT_DIR + GIT_WORK_TREE naming another repository -> still its own")                       \
    X(T4AnUntrackedDssTreeNestedInsideAnotherCheckoutIsRefused,                                                        \
      "T4 an untracked DSS tree nested inside another checkout is REFUSED")                                            \
    X(T5APlainRepositoryWithNoDssTreeAroundItIsItsOwnOwningRoot,                                                       \
      "T5 a plain repository with no DSS tree around it is its own owning root")                                       \
    X(T6ARepositoryInsideADssTreeIsItsOwnOwningRoot, "T6 a repository INSIDE a DSS tree is its own owning root")       \
    X(T7OwningRootIsAUsageRefusal, "T7 owning_root('') is a UsageRefusal")                                             \
    X(C1UnderASteeringGitIndexFileTheHelperConsumerStillNamesLooseTxt,                                                 \
      "C1 under a steering GIT_INDEX_FILE the helper consumer still names loose.txt")                                  \
    X(C2NegativeUnderTheSameSteeringThePlainConsumerDoesNotGetItRight,                                                 \
      "C2 NEGATIVE: under the same steering the plain consumer does NOT get it right")                                 \
    X(C3UnderASteeringGitDirGitWorkTreeTheHelperConsumerStillNamesLooseTxt,                                            \
      "C3 under a steering GIT_DIR + GIT_WORK_TREE the helper consumer still names loose.txt")                         \
    X(C4NegativeUnderTheSameSteeringThePlainConsumerDoesNotGetItRight,                                                 \
      "C4 NEGATIVE: under the same steering the plain consumer does NOT get it right")                                 \
    X(C5ACorruptIndexTheHelperConsumerSaysCannotRunExit2NeverClean,                                                    \
      "C5 a corrupt index: the helper consumer says CANNOT RUN (exit 2), never 'clean'")                               \
    X(C6NegativeThePlainConsumerReadsTheSameCorruptIndexAsNoLooseFiles,                                                \
      "C6 NEGATIVE: the plain consumer reads the same corrupt index as 'no loose files'")                              \
    X(P1ProbeChildReportsAChildSOutput, "P1 probe_child reports a child's output")                                     \
    X(P2ASilentFailingChildIsReportedExactlyNeverRaised,                                                               \
      "P2 a silent failing child is reported exactly, never raised")                                                   \
    X(J1StripJsoncCommentsAndTrailingCommasGoInsideAStringStays,                                                       \
      "J1 strip_jsonc: comments and trailing commas go, `//` inside a string stays")                                   \
    X(J2AnUnterminatedCommentIsRefusedNamingItsSource, "J2 an unterminated /* comment is REFUSED naming its source")   \
    X(J3LoadJsoncRefusesAFileThatDoesNotParseOrCannotBeReadNamingIt,                                                   \
      "J3 load_jsonc REFUSES a file that does not parse or cannot be read, naming it")                                 \
    X(J4ControlThisTreeSOwnConfigJsonParsesWithItsWorktreesAndLegs,                                                    \
      "J4 CONTROL: this tree's own config.json parses, with its worktrees and legs")                                   \
    X(J5TheInterpreterRunningThisSelfTestMeetsThePythonFloorTheTreeSConfigStates,                                      \
      "J5 the interpreter running this self-test meets the Python floor the tree's config states")                     \
    X(L1TheCopiesTheToolKeepsInATreeASessionSScratchAndABuildAreLeftOutTheRealFilesAndEachPlac,                        \
      "L1 the copies the tool keeps in a tree, a session's scratch and a build are left out; the real files and each placeholder stay") \
    X(L2ALaterRuleDecidesAndANegationReIncludes, "L2 a later rule decides, and a negation re-includes")                \
    X(L3ARuleWithNoSlashLeavesOutThatNameAtAnyDepth, "L3 a rule with no slash leaves out that name at any depth")      \
    X(L4ARuleWithASlashIsAnchoredToTheTreeSRootThatDirectoryGoesANamesakeElsewhereStays,                               \
      "L4 a rule with a slash is anchored to the tree's root: that directory goes, a namesake elsewhere stays")        \
    X(L5ARuleEndingInASlashMatchesADirectoryOnlyAFileOfThatNameStays,                                                  \
      "L5 a rule ending in a slash matches a directory only: a FILE of that name stays")                               \
    X(L6ADoubleStarSegmentIsAnyNumberOfDirectories, "L6 a double-star segment is any number of directories")           \
    X(L7ARuleEndingInASlashNeverMatchesASymbolicLink, "L7 a rule ending in a slash never matches a symbolic link")     \
    X(L8AStarAQuestionMarkAndAClassMatchTheNameSBytesAsGitSWildmatchDoes,                                              \
      "L8 a star, a question mark and a class match the name's bytes, as git's wildmatch does")                        \
    X(L9NothingUnderADirectoryLeftOutIsReIncluded, "L9 nothing under a directory left out is re-included")             \
    X(L10GitSOwnDirectoryAndAWorktreeSGitFileAreLeftOut,                                                               \
      "L10 git's own directory and a worktree's git file are left out")                                                \
    X(L11ARuleHoldingABackslashIsRefusedNamingItsLine, "L11 a rule holding a backslash is refused, naming its line")   \
    X(L12AClassOpenedAndNeverClosedIsRefusedNamingItsLine,                                                             \
      "L12 a class opened and never closed is refused, naming its line")                                               \
    X(L13ARuleNamingNothingBetweenTwoSeparatorsOrNothingAtAllIsRefused,                                                \
      "L13 a rule naming nothing between two separators, or nothing at all, is refused")                               \
    X(L14ATreeWithNoGitignoreLeavesOutGitSOwnDirectoryAlone,                                                           \
      "L14 a tree with no .gitignore leaves out git's own directory alone")                                            \
    X(L15ADirectoryKeptIsWalkedWholeWhateverTheRulesSayInsideIt,                                                       \
      "L15 a directory kept is walked whole, whatever the rules say inside it")                                        \
    X(L16AWalkWhoseTopIsLeftOutIsRefusedAndSoIsAPathOutsideTheTree,                                                    \
      "L16 a walk whose top is left out is refused, and so is a path outside the tree")                                \
    X(L17APathARuleMatchesUnderOneSettingOfCoreIgnorecaseAndNotTheOtherIsRefusedNamingIt,                              \
      "L17 a path a rule matches under one setting of core.ignorecase and not the other is refused, naming it")        \
    X(L18LiveThisTreeSOwnGitignoreLeavesOutEveryDirectoryTheToolKeepsInATreeAndKeepsEachPlaceh,                        \
      "L18 LIVE: this tree's own .gitignore leaves out every directory the tool keeps in a tree and keeps each placeholder") \
    X(L19TheWalkAndTheQuestionAboutOnePathGiveOneAnswerOnEveryPath,                                                    \
      "L19 the walk and the question about one path give one answer on every path")                                    \
    X(L20LiveNoFileGitTracksInThisTreeIsLeftOutOfAWalkARuleOfItsGitignoreCannotSilenceATracked,                        \
      "L20 LIVE: no file git tracks in this tree is left out of a walk -- a rule of its .gitignore cannot silence a tracked file") \
    X(L21ADirectoryTheWalkCannotListIsRefusedByNameNeverSkipped,                                                       \
      "L21 a directory the walk cannot list is refused by name, never skipped")                                        \
    X(L22TheTwelvePosixClassesReadAsGitReadsThem, "L22 the twelve POSIX classes read as git reads them")               \
    X(L23ADashABracketFirstARangeAndALoneInsideAClassReadAsGitReadsThem,                                               \
      "L23 a dash, a bracket first, a range and a lone [: inside a class read as git reads them")                      \
    X(L24APosixClassGitDoesNotNameIsRefusedNamingItsLine,                                                              \
      "L24 a POSIX class git does not name is refused, naming its line")                                               \
    X(L25ANameHoldingABackslashIsRefusedNamingIt, "L25 a name holding a backslash is refused, naming it")              \
    X(L26AByteOrderMarkACrLfEndingAndTrailingSpacesAreNoPartOfARuleATrailingTabIs,                                     \
      "L26 a byte order mark, a CR LF ending and trailing spaces are no part of a rule; a trailing tab is")            \
    X(L27ANameThatIsGitSOwnOnlyWhenCaseIsFoldedIsRefused,                                                              \
      "L27 a name that is git's own only when case is folded is refused")                                              \
    X(L28TheWalkNeverDescendsASymbolicLinkALinkToADirectoryIsJudgedAndYieldedAsAFile,                                  \
      "L28 the walk never descends a symbolic link: a link to a directory is judged, and yielded, as a file")          \
    X(L29AGitignoreThatIsADirectoryIsRefusedWhatAWalkLeavesOutIsUnknown,                                               \
      "L29 a .gitignore that is a directory is refused: what a walk leaves out is unknown")                            \
    X(L30LiveEveryScenarioSPathsAreAnsweredAsGitCheckIgnoreAnswersThemUnderEitherSettingOfCore,                        \
      "L30 LIVE: every scenario's paths are answered as git check-ignore answers them, under either setting of core.ignorecase") \
    X(L31EveryScenarioOfTheCaseTableIsAskedByOneArm, "L31 every scenario of the case table is asked by one arm")       \
    X(D1APathToWriteThatBeginsWithIsAnOptionTakenForAValueRefusedNamingTheFlag,                                        \
      "D1 a path to write that begins with `-` is an option taken for a value: refused, naming the flag")              \
    X(D2APathToWriteThatIsEmptyNamesNoPlaceRefusedNamingTheFlag,                                                       \
      "D2 a path to write that is empty names no place: refused, naming the flag")                                     \
    X(D3APathThatHoldsADashOrSpellsALeadingOneAsXIsKeptAsGivenTheControl,                                              \
      "D3 a path that holds a dash, or spells a leading one as `./-x`, is kept as given (the control)")                \
    X(D4WrittenPathArgArgparseRefusesAPathToWriteThatBeginsWithInEitherSpellingAndAnEmptyOneBy,                        \
      "D4 written_path_arg: argparse refuses a path to write that begins with `-`, in either spelling, and an empty one, by its usage error naming the flag") \
    X(D5WrittenPathArgAPathHoldingADashOrSpellingALeadingOneAsXPassesTheTypeAsGivenTheControl,                         \
      "D5 written_path_arg: a path holding a dash, or spelling a leading one as `./-x`, passes the type as given (the control)") \
    X(X2TheSandboxFenceRefusesABoxInsideTheTreeAndInsideARepositoryAndAcceptsOneOutsideBothThe,                        \
      "X2 the sandbox fence refuses a box inside the tree and inside a repository, and accepts one outside both (the control)") \
    X(X1TheSelfTestSTempTreesWereRemoved, "X1 the self-test's temp trees were removed")

#define DSS_OWNING_TREE_SELFTEST_ARM_NAME(caseName, armName) armName,
constexpr std::string_view kArmNames[] = {DSS_OWNING_TREE_SELFTEST_ARMS(DSS_OWNING_TREE_SELFTEST_ARM_NAME)};
#undef DSS_OWNING_TREE_SELFTEST_ARM_NAME

constexpr dss::test_support::ScriptSelfTest kEntry{
    .command          = dss::owning_tree_selftest::kCommand,
    .workingDirectory = dss::owning_tree_selftest::kWorkingDirectory,
    .standardOutput   = dss::owning_tree_selftest::kStandardOutput,
    .marker           = "owning-tree self-test: ",
    .armNames         = kArmNames,
    .tableName        = "DSS_OWNING_TREE_SELFTEST_ARMS",
};

// ONE run for the whole binary: every case reads the same output.
[[nodiscard]] dss::test_support::ScriptSelfTestRun const& theRun() {
    static dss::test_support::ScriptSelfTestRun const run = dss::test_support::runScriptSelfTest(kEntry);
    return run;
}

}  // namespace

#define DSS_OWNING_TREE_SELFTEST_CASE(caseName, armName) \
    TEST(OwningTreeSelfTest, caseName) { dss::test_support::expectTheScriptArmPassed(kEntry, theRun(), armName); }
DSS_OWNING_TREE_SELFTEST_ARMS(DSS_OWNING_TREE_SELFTEST_CASE)
#undef DSS_OWNING_TREE_SELFTEST_CASE

// The program's arms and this file's cases are the same set: an arm added to the program without a
// case here is a red, by the arm's name.
TEST(OwningTreeSelfTest, EveryArmTheProgramRanIsACaseAndEveryCaseAnArm) {
    dss::test_support::expectEveryScriptArmIsACaseAndEveryCaseAnArm(kEntry, theRun());
}

// The program exits non-zero exactly when an arm said FAILED; a traceback between two arms, or an arm
// count other than its own ratchet, is an exit code no arm's case reports, and this one does.
TEST(OwningTreeSelfTest, TheProgramsExitCodeAgreesWithItsArms) {
    dss::test_support::expectTheScriptsExitCodeAgreesWithItsArms(kEntry, theRun());
}
