// The `.dss-deps` git dependency cache — `src/program/{git_acquire,
// dependency_lockfile, dependency_cache}.{hpp,cpp}`.
//
// ★ EVERY TEST IN THIS FILE DRIVES A FAKE `IGitRunner`. ZERO NETWORK, ZERO
// DEPENDENCE ON `git` BEING INSTALLED. That is B.7 layer 1 and it is not
// optional: B.4's machine has FOUR outcomes and TWO of them are network
// FAILURES, which a real-git test could not reach deterministically — you
// cannot ask a working network to fail on cue, and a CI leg that tried would
// be flaky in the direction that reads as green. The seam exists so the state
// machine can be driven, and this file is the reason it exists. The ONE
// exception is `SystemGitRunnerIsolation`: B.7 layer 2, real `git` and still no
// network, because what it pins is what the REAL runner hands the real tool
// (which repository, which environment). Skipped, and saying why, where `git`
// is not on PATH.
//
// ── WHAT BREAKS SILENTLY HERE, WHICH IS WHY THE PINS LOOK PARANOID ──────────
//
//   * A CACHE HIT THAT STILL TOUCHES THE NETWORK. B.4's guarantee is "NO
//     NETWORK ACCESS AT ALL. Not a conditional request, not an `ls-remote`,
//     nothing." A test asserting only `cloneCallCount == 0` lets a stray
//     `fetch` straight through, and the build still succeeds — it is just no
//     longer offline, and nobody finds out until a train. Every hit assertion
//     here is `clone == 0 && fetch == 0 && revParse == 1`.
//   * `--force-git-cache` THAT FETCHES AND DOES NOT CHECK OUT. A fetch moves
//     remote-tracking refs and leaves HEAD alone, so `rev-parse HEAD` returns
//     the OLD commit and the flag is a network round trip that changes
//     nothing. A test asserting `fetchCallCount == 1` goes GREEN over an
//     entirely absent mechanism. `ForceRefreshMovesHeadAndRewritesTheLockfile`
//     asserts the NEW COMMIT and the REWRITTEN LOCKFILE instead.
//   * THE TWO FAILURE CODES KEYED ON THE WRONG THING. 0xD01E and 0xD01F are
//     discriminated by "is there a usable checkout" and by NOTHING else — not
//     the git exit status, not clone-vs-fetch — or an offline build's outcome
//     depends on how git happened to phrase the failure. The force arm proves
//     it from the other side: with the flag ON and the fetch failing, a
//     present checkout still gives 0xD01F.
//   * 0xD01F PROMOTED TO Warning BY A TIDY-UP. `--warnings-as-errors` promotes
//     every Warning code-agnostically, so at Warning severity the notice that
//     exists to KEEP offline builds working would fail every one of them. The
//     pin is THREE-SIDED (present exactly once, severity Info, errorCount 0),
//     because zero Infos also makes a two-sided version green.
//   * A CORRUPT LOCKFILE READ AS AN EMPTY ONE. That is the tolerant fallback
//     U-4 forbids: the build would silently re-acquire everything, overwrite
//     the damaged file, and never say the state it was asked to reproduce was
//     unreadable. `CorruptLockfileRefusesToOpenTheCache` asserts
//     `cloneCallCount == 0` as well as the failure, because "it failed" is
//     also true of a version that failed AFTER cloning.
//   * A DERIVED NAME THAT ESCAPES THE CACHE. `.` and `..` are spelled entirely
//     from the legal character set and are derivable from a real URL, and
//     `depsDir / ".."` is the consumer's own project directory. Nothing about
//     the character check catches them, so the reserved-name arm is separate.

#include "core/substrate/process_spawn.hpp"  // the offline real-git witness
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/dependency_cache.hpp"
#include "program/dependency_lockfile.hpp"
#include "program/git_acquire.hpp"

#include "diagnostic_count.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// `getpid` — the lockfile's scratch names carry the process id, and the
// occupied-name pin must name the FIRST one this process would claim.
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using dss::CacheOutcome;
using dss::DependencyCache;
using dss::DependencyLockfile;
using dss::DerivedNameStatus;
using dss::DiagnosticCode;
using dss::DiagnosticDelivery;
using dss::DiagnosticReporter;
using dss::DiagnosticSeverity;
using dss::GitCommandResult;
using dss::LockedDependency;
using dss::deriveDependencyCacheName;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace fs = std::filesystem;

namespace {

// ── THE FAKE ────────────────────────────────────────────────────────────────
//
// It keeps its state ON THE FILESYSTEM — a `HEAD` file inside the checkout —
// rather than in a path-keyed map, and that is deliberate rather than cute: the
// answer to "what is checked out here" must come from the tree the cache is
// actually looking at, or the fake would be testing its own bookkeeping. (The
// cache used to clone into a staging directory and rename it into place, which
// made a path-keyed map go stale mid-mechanism; it now clones in place, and the
// on-disk state is still the honest answer.)
//
// `onClone` runs inside a SUCCESSFUL clone, after the tree is written — the seam
// the landing and concurrency pins use to hold a file open inside the fresh
// checkout, or to pause one acquisition mid-clone by handshake.
class FakeGitRunner final : public dss::IGitRunner {
public:
    // ── scripted outcomes ──
    bool available        = true;
    bool cloneSucceeds    = true;
    bool fetchSucceeds    = true;
    bool checkoutSucceeds = true;
    // What a successful clone leaves HEAD at (the remote's default branch).
    std::string clonedCommit = "commit-default";
    // Per-rev answers for `checkout`; anything unlisted resolves to
    // "commit-<rev>", which keeps a test that does not care from having to
    // populate the map.
    std::map<std::string, std::string> commitByRev;
    std::function<void(fs::path const& dest)> onClone;

    // ── observations ──
    int                      isAvailableCalls = 0;
    int                      cloneCalls       = 0;
    int                      fetchCalls       = 0;
    int                      checkoutCalls    = 0;
    int                      revParseCalls    = 0;
    std::vector<std::string> clonedUrls;
    std::vector<std::string> fetchedRefs;
    std::vector<std::string> checkedOutRevs;

    bool isAvailable() override {
        ++isAvailableCalls;
        return available;
    }

    GitCommandResult clone(std::string const& url, fs::path const& dest) override {
        ++cloneCalls;
        clonedUrls.push_back(url);
        if (!cloneSucceeds) return failure("clone refused by the fake");
        std::error_code ec;
        fs::create_directories(dest, ec);
        if (ec) return failure("fake clone could not create " + dest.string());
        writeHead(dest, clonedCommit);
        if (onClone) onClone(dest);
        return success();
    }

    GitCommandResult fetch(fs::path const& checkoutDir,
                           std::string const& ref) override {
        ++fetchCalls;
        fetchedRefs.push_back(ref);
        (void)checkoutDir;
        // ★ A SUCCESSFUL FETCH DELIBERATELY DOES NOT MOVE HEAD. That is what
        // real git does, and it is the whole reason the cache must follow a
        // fetch with a checkout — a fake that moved HEAD here would make the
        // missing-checkout bug invisible.
        return fetchSucceeds ? success() : failure("fetch refused by the fake");
    }

    GitCommandResult checkout(fs::path const& checkoutDir,
                              std::string const& rev) override {
        ++checkoutCalls;
        checkedOutRevs.push_back(rev);
        if (!checkoutSucceeds) return failure("checkout refused by the fake");
        writeHead(checkoutDir, commitFor(rev));
        return success();
    }

    GitCommandResult revParse(fs::path const& checkoutDir,
                              std::string const& rev) override {
        ++revParseCalls;
        (void)rev;
        std::ifstream in{checkoutDir / "HEAD"};
        if (!in) {
            // The honest answer for a directory that is not a repository —
            // which is exactly what the "usable checkout" probe is asking.
            return failure("fatal: not a git repository: "
                           + checkoutDir.generic_string());
        }
        GitCommandResult out;
        out.ok = true;
        std::getline(in, out.output);
        return out;
    }

    [[nodiscard]] std::string commitFor(std::string const& rev) const {
        auto const it = commitByRev.find(rev);
        return it == commitByRev.end() ? ("commit-" + rev) : it->second;
    }

    static void writeHead(fs::path const& dir, std::string const& commit) {
        std::ofstream out{dir / "HEAD", std::ios::trunc};
        out << commit << "\n";
    }

private:
    static GitCommandResult success() {
        GitCommandResult out;
        out.ok = true;
        return out;
    }
    static GitCommandResult failure(std::string detail) {
        GitCommandResult out;
        out.detail = std::move(detail);
        return out;
    }
};

constexpr char const* kUrl = "https://example.invalid/org/bar.git";

// Arrange a populated cache: one successful acquisition plus a persisted
// lockfile, i.e. the state every HIT test needs to start from.
struct PopulatedCache {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter arrangeRep;

    void seed(std::optional<std::string> const& ref = std::nullopt) {
        auto cache = DependencyCache::open(scratch.path(), git, false, arrangeRep);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, ref, arrangeRep);
        ASSERT_TRUE(name.has_value());
        auto const got = cache->acquire(*name, arrangeRep);
        ASSERT_EQ(got.outcome, CacheOutcome::Miss);
        ASSERT_TRUE(cache->save(arrangeRep));
        ASSERT_EQ(arrangeRep.errorCount(), 0u) << "the arrange step must be clean";
    }
};

[[nodiscard]] std::string readFile(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{}};
}

void writeFile(fs::path const& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
}

// Every entry beside `lockPath` whose name starts `<lockfile name>.tmp` — both
// the unique `<lock>.tmp-<pid>-<n>` scheme and the old fixed `<lock>.tmp`. Sorted.
[[nodiscard]] std::vector<std::string> scratchFilesBeside(fs::path const& lockPath) {
    std::string const        prefix = lockPath.filename().string() + ".tmp";
    std::vector<std::string> found;
    std::error_code          ec;
    for (auto const& e : fs::directory_iterator(lockPath.parent_path(), ec)) {
        auto const name = e.path().filename().string();
        if (name.rfind(prefix, 0) == 0) found.push_back(name);
    }
    EXPECT_FALSE(static_cast<bool>(ec)) << ec.message();
    std::sort(found.begin(), found.end());
    return found;
}

[[nodiscard]] unsigned long thisProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// U-5 — THE DERIVATION. It is the SPEC, not an implementation detail, because
// 0xD020 makes collisions between DERIVED names a diagnostic and a diagnostic
// about a derived value is meaningless unless the derivation is pinned.
// ═══════════════════════════════════════════════════════════════════════════

TEST(DependencyCacheName, LastSegmentWithOneGitSuffixStripped) {
    auto const ok = [](std::string_view url) {
        auto const d = deriveDependencyCacheName(url);
        EXPECT_EQ(d.status, DerivedNameStatus::Ok) << url;
        return d.value;
    };
    EXPECT_EQ(ok("https://example.invalid/org/bar.git"), "bar");
    EXPECT_EQ(ok("https://example.invalid/org/bar"), "bar");
    EXPECT_EQ(ok("https://example.invalid/org/bar/"), "bar");
    EXPECT_EQ(ok("https://example.invalid/org/bar///"), "bar");
    // The common scp spelling has a slash after the colon, so it derives
    // normally. (The pathological `git@host:bar.git`, with no slash at all, is
    // rejected — see `ScpUrlWithoutAPathSegmentIsRejectedRatherThanGuessed`.)
    EXPECT_EQ(ok("git@example.invalid:org/bar.git"), "bar");
    EXPECT_EQ(ok("file:///tmp/fixtures/bar.git"), "bar");
}

// ★ ONE strip, not repeated. `…/x.git.git` is `x.git`, and a loop would give
// `x` — a DIFFERENT directory, silently, for a repository whose name happens
// to end in `.git`.
TEST(DependencyCacheName, ExactlyOneTrailingGitSuffixIsStripped) {
    auto const d = deriveDependencyCacheName("https://example.invalid/x.git.git");
    EXPECT_EQ(d.status, DerivedNameStatus::Ok);
    EXPECT_EQ(d.value, "x.git");
}

// CASE-SENSITIVE, so `Bar` and `bar` are two names and NOT a collision. A
// case-folding derivation would merge two distinct repositories into one
// checkout on Windows and macOS while keeping them apart on Linux.
TEST(DependencyCacheName, DerivationIsCaseSensitive) {
    EXPECT_EQ(deriveDependencyCacheName("https://example.invalid/Bar.git").value,
              "Bar");
    EXPECT_NE(deriveDependencyCacheName("https://example.invalid/Bar.git").value,
              deriveDependencyCacheName("https://example.invalid/bar.git").value);
}

TEST(DependencyCacheName, IllegalCharacterIsReportedWithTheOffendingCharacter) {
    auto const d = deriveDependencyCacheName("https://example.invalid/a b.git");
    EXPECT_EQ(d.status, DerivedNameStatus::IllegalCharacter);
    // The derived value is filled in EVEN ON THE REJECT: 0xD024's allocation
    // requires the message to show it, because the name appears nowhere in the
    // manifest and quoting only the URL leaves the reader to run the
    // derivation in their head.
    EXPECT_EQ(d.value, "a b");
    EXPECT_EQ(d.offendingChar, ' ');
}

TEST(DependencyCacheName, ScpUrlWithoutAPathSegmentIsRejectedRatherThanGuessed) {
    auto const d = deriveDependencyCacheName("git@example.invalid:bar.git");
    EXPECT_EQ(d.status, DerivedNameStatus::IllegalCharacter)
        << "the rule splits on `/` only; teaching it about `:` would be "
           "inventing a rule the spec does not state, in the one function a "
           "user-visible diagnostic quotes";
    EXPECT_EQ(d.value, "git@example.invalid:bar");
}

TEST(DependencyCacheName, UrlWithNoUsableSegmentIsRejected) {
    // Each of these leaves NOTHING after the two rules run: the first two have
    // no non-empty segment at all, and the last two have one that is EXACTLY
    // `.git` and therefore strips to nothing.
    for (auto const* url : {"", "///", ".git",
                            "https://example.invalid/.git"}) {
        EXPECT_EQ(deriveDependencyCacheName(url).status,
                  DerivedNameStatus::NoSegment)
            << url;
    }
}

// ★ A PATH-LESS URL DERIVES ITS AUTHORITY, AND THAT IS THE RULE WORKING RATHER
// THAN A HOLE IN IT. `https://example.invalid/` has a last non-empty
// `/`-separated segment — `example.invalid` — so U-5 derives it and the status
// is `Ok`. This assertion used to sit in the loop above expecting `NoSegment`,
// which the implementation has never satisfied; the expectation confused "the
// URL names no REPOSITORY" (true) with "the URL has no SEGMENT" (false).
//
// ⚠ AND THE FIX IS THE TEST, NOT THE DERIVATION — the alternative was measured
// and refused. Rejecting this URL means distinguishing an AUTHORITY from a PATH,
// i.e. parsing `scheme://authority/path`, inside the one function whose exact
// behaviour a user-visible diagnostic quotes (0xD020 and 0xD024 both print the
// derived name). `deriveDependencyCacheName`'s own docblock already refused
// exactly this class of invention one case earlier, for the scp form: "Teaching
// the splitter about `:` would be inventing a rule the spec does not state." A
// URL-structure rule is the same invention wearing a different separator, and it
// would not even be well defined for the scp spelling, which has no `//` at all.
//
// It is also harmless in practice, which is why the cheap rule is the right one:
// `git clone https://example.invalid/` names no repository, so acquisition fails
// and `D_DependencyGitAcquireFailed` (0xD01E) reports it LOUDLY against the URL
// the user actually wrote — a better diagnostic than a derived-name complaint
// that would send the reader looking at their cache directory instead.
TEST(DependencyCacheName, PathLessUrlDerivesItsAuthorityRatherThanRejecting) {
    auto const d = deriveDependencyCacheName("https://example.invalid/");
    EXPECT_EQ(d.status, DerivedNameStatus::Ok);
    EXPECT_EQ(d.value, "example.invalid");
    // The trailing-separator skip is what makes this identical to the
    // slash-less spelling; pin both so a change to either is visible.
    EXPECT_EQ(deriveDependencyCacheName("https://example.invalid").value,
              "example.invalid");
}

// ★ THE PATH-ESCAPE GUARD, AND IT IS NOT COVERED BY THE CHARACTER CHECK. `.`
// and `..` are spelled entirely from `[A-Za-z0-9._-]`, are derivable from a
// real URL, and `depsDir / ".."` is the CONSUMER'S OWN PROJECT DIRECTORY — a
// place this cache clones into and calls `remove_all` on. `dss-lock.json` is
// the third: a checkout directory would sit exactly on the lockfile's path.
TEST(DependencyCacheName, DotDotDotAndTheLockfileNameAreReserved) {
    for (auto const* url : {"https://example.invalid/.",
                            "https://example.invalid/..",
                            "https://example.invalid/dss-lock.json"}) {
        auto const d = deriveDependencyCacheName(url);
        EXPECT_EQ(d.status, DerivedNameStatus::ReservedName) << url;
        EXPECT_FALSE(d.value.empty())
            << "the reject still has to show the name it derived";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// THE ARGV. The seam above makes the state machine testable without git; the
// price is that the four vectors which actually talk to git are reachable by
// no other test. Each encodes a decision that is SILENT when wrong.
// ═══════════════════════════════════════════════════════════════════════════

TEST(GitArgv, CloneSeparatesOptionsFromTheUserSuppliedUrl) {
    // The `--` is the pin. `url` comes from a manifest, so a value beginning
    // with `-` would otherwise be parsed by git as an option.
    EXPECT_EQ(dss::gitCloneArgv("/usr/bin/git", "--upload-pack=evil",
                                fs::path{"/tmp/dest"}),
              (std::vector<std::string>{"/usr/bin/git", "clone", "--",
                                        "--upload-pack=evil",
                                        fs::path{"/tmp/dest"}.string()}));
}

// The two options every command on a CHECKOUT carries, BEFORE the subcommand
// (git's global options must precede it) — so git is told the repository and
// never discovers one by walking up from the working directory.
// [[D-DEPS-GIT-RUNNER-DISCOVERS-AN-ENCLOSING-REPOSITORY-AND-FORCE-CHECKS-IT-OUT]]
namespace {

[[nodiscard]] std::vector<std::string>
namedRepository(std::string const& gitExe, fs::path const& checkout) {
    return {gitExe, "--git-dir=" + (checkout / ".git").string(),
            "--work-tree=" + checkout.string()};
}

[[nodiscard]] std::vector<std::string>
concat(std::vector<std::string> head, std::vector<std::string> const& tail) {
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

} // namespace

TEST(GitArgv, FetchNamesTheRefExplicitlyAndFallsBackToRemoteHead) {
    fs::path const dir{"/work/.dss-deps/bar"};
    EXPECT_EQ(dss::gitFetchArgv("git", dir, "v1.2.0"),
              concat(namedRepository("git", dir),
                     {"fetch", "--force", "--tags", "origin", "v1.2.0"}));
    // An absent ref asks the remote for its own default branch, which is what
    // a `{git}`-with-no-`ref` manifest entry means.
    EXPECT_EQ(dss::gitFetchArgv("git", dir, ""),
              concat(namedRepository("git", dir),
                     {"fetch", "--force", "--tags", "origin", "HEAD"}));
}

// ★ `--detach` IS THE PIN. Checking out a BRANCH name leaves HEAD attached to
// a local branch, and the next fetch+checkout of that same branch moves
// nothing — `--force-git-cache` would cost a round trip and change nothing.
TEST(GitArgv, CheckoutAlwaysDetaches) {
    fs::path const dir{"/work/.dss-deps/bar"};
    EXPECT_EQ(dss::gitCheckoutArgv("git", dir, "FETCH_HEAD"),
              concat(namedRepository("git", dir),
                     {"checkout", "--detach", "--force", "FETCH_HEAD"}));
}

TEST(GitArgv, RevParseAsksForOneRevision) {
    fs::path const dir{"/work/.dss-deps/bar"};
    EXPECT_EQ(dss::gitRevParseArgv("git", dir, "HEAD"),
              concat(namedRepository("git", dir), {"rev-parse", "HEAD"}));
}

// ═══════════════════════════════════════════════════════════════════════════
// ⛔ THE RUNNER NEVER OPERATES ON A REPOSITORY IT WAS NOT HANDED.
// [[D-DEPS-GIT-RUNNER-DISCOVERS-AN-ENCLOSING-REPOSITORY-AND-FORCE-CHECKS-IT-OUT]]
//
// ✔MEASURED 2026-09-18 (lane `rw`), on the author's own worktree: a checkout
// whose `.git` had lost its `HEAD` made git DISCOVER the enclosing repository;
// the cache read that repository's HEAD as the dependency's, fetched its remote
// and ran `git checkout --detach --force FETCH_HEAD` over it — the reflog reads
// `checkout: moving from 7df54cc1… to FETCH_HEAD`, and the uncommitted work was
// gone. `.dss-deps/` lives INSIDE the consuming project, which is almost always
// a repository, so this is the user's case.
//
// REAL git, fully offline — plan 06 §B.7 layer 2 ("real `git`, … still no
// network"): an enclosing repository with a commit and an UNCOMMITTED edit, and
// a dependency checkout under it whose `.git` is not a repository. Skipped, and
// saying why, when `git` is not on PATH — never green without having run.
// RED ON DISABLE: drop the `--git-dir` / `--work-tree` pair from the argv
// builders — `rev-parse` then answers with the ENCLOSING commit and the forced
// checkout wipes the uncommitted edit, so all three assertions go red.
// ═══════════════════════════════════════════════════════════════════════════

TEST(SystemGitRunnerIsolation, AnInvalidCheckoutNeverResolvesToTheEnclosingRepository) {
    auto const gitExe = dss::substrate::resolveExecutableOnPath("git");
    if (!gitExe) {
        GTEST_SKIP() << "`git` is not on PATH, so the offline real-git witness "
                        "(plan 06 §B.7 layer 2) cannot run on this host";
    }
    ScratchDir     scratch{Location::Temp, "dep-git-isolation"};
    fs::path const project = scratch.path() / "consumer";
    fs::create_directories(project);
    auto const run = [&](std::vector<std::string> args) {
        std::vector<std::string> argv{gitExe->string(), "-c", "user.name=dss-test",
                                      "-c", "user.email=dss-test@example.invalid"};
        argv.insert(argv.end(), args.begin(), args.end());
        return dss::substrate::spawnAndWaitInherit(argv, project);
    };
    ASSERT_EQ(run({"init", "--quiet"}).exitCode, 0);
    writeFile(project / "tracked.txt", "committed\n");
    ASSERT_EQ(run({"add", "tracked.txt"}).exitCode, 0);
    ASSERT_EQ(run({"commit", "--quiet", "-m", "the user's project"}).exitCode, 0);
    writeFile(project / "tracked.txt", "UNCOMMITTED WORK\n");

    // The dependency: a directory whose `.git` is NOT a repository — the
    // measured shape (its `HEAD` gone, objects left).
    fs::path const dep = project / ".dss-deps" / "bar";
    fs::create_directories(dep / ".git" / "objects");
    writeFile(dep / "scale.c", "int dss_scale_by_seven(int v) { return v * 7; }\n");

    dss::SystemGitRunner git;
    auto const head = git.revParse(dep, "HEAD");
    EXPECT_FALSE(head.ok)
        << "rev-parse answered '" << head.output << "' for a checkout that is not "
        << "a repository — that is the ENCLOSING project's commit, and the cache "
        << "would treat the user's own repository as the dependency";
    auto const co = git.checkout(dep, "HEAD");
    EXPECT_FALSE(co.ok) << "a forced checkout aimed at a dependency ran somewhere";
    EXPECT_EQ(readFile(project / "tracked.txt"), "UNCOMMITTED WORK\n")
        << "the user's uncommitted edit was DISCARDED by a forced checkout aimed "
        << "at a dependency";
}

// ═══════════════════════════════════════════════════════════════════════════
// ⛔ AND NO `git` THE RUNNER SPAWNS INHERITS GIT'S OWN REPOSITORY VARIABLES.
// [[D-DEPS-GIT-RUNNER-INHERITS-GITS-REPOSITORY-VARIABLES-AND-WRITES-THE-USERS-INDEX]]
//
// ✔MEASURED 2026-09-18 (lane `rw`, Git for Windows 2.55.0): a `pre-commit` hook
// in a LINKED WORKTREE exports `GIT_DIR` and an ABSOLUTE `GIT_INDEX_FILE`, both
// naming the worktree's own repository. A build run from that hook — its
// dependency checkout already naming its repository with `--git-dir` — still had
// its `checkout` (refresh) and its `clone` (first acquisition) write the
// DEPENDENCY's index into the USER's worktree index. The staged work was gone,
// the commit failed `invalid object … for '.dss-project.json'`, and `git status`
// refused the repository. `--git-dir` outranks `GIT_DIR`; nothing on a command
// line outranks `GIT_INDEX_FILE`.
//
// REAL git, offline (§B.7 layer 2), skipped with the reason when git is absent.
// The pin builds that exact shape: a user repository, a LINKED WORKTREE of it
// with a STAGED file, and the two variables git exports to a hook there, set in
// this process for the duration of the runner's three calls (restored after).
// The worktree's index must be byte-identical afterwards, and every call must
// have worked on the DEPENDENCY.
// RED ON DISABLE: stop withholding `gitRepositoryLocalVariables()` in the
// runner (or make the substrate's environment arm a no-op). The clone's and the
// checkout's index then land in the worktree's index file.
// ═══════════════════════════════════════════════════════════════════════════

TEST(SystemGitRunnerIsolation, GitsOwnRepositoryVariablesNeverReachTheDependencysGit) {
    auto const gitExe = dss::substrate::resolveExecutableOnPath("git");
    if (!gitExe) {
        GTEST_SKIP() << "`git` is not on PATH, so the offline real-git witness "
                        "(plan 06 §B.7 layer 2) cannot run on this host";
    }
    ScratchDir scratch{Location::Temp, "dep-git-environment"};
    auto const gitIn = [&](fs::path const& dir, std::vector<std::string> args) {
        std::vector<std::string> argv{gitExe->string(), "-c", "user.name=dss-test",
                                      "-c", "user.email=dss-test@example.invalid"};
        argv.insert(argv.end(), args.begin(), args.end());
        return dss::substrate::spawnAndWaitInherit(argv, dir);
    };

    // The dependency's upstream: one commit.
    fs::path const origin = scratch.path() / "origin";
    fs::create_directories(origin);
    ASSERT_EQ(gitIn(origin, {"init", "--quiet"}).exitCode, 0);
    writeFile(origin / "scale.c", "int dss_scale_by_seven(int v) { return v * 7; }\n");
    ASSERT_EQ(gitIn(origin, {"add", "scale.c"}).exitCode, 0);
    ASSERT_EQ(gitIn(origin, {"commit", "--quiet", "-m", "the dependency"}).exitCode, 0);

    // The user's repository, and a LINKED WORKTREE of it holding a STAGED file —
    // the index a `pre-commit` hook's commit is about to record.
    fs::path const user = scratch.path() / "user";
    fs::create_directories(user);
    ASSERT_EQ(gitIn(user, {"init", "--quiet"}).exitCode, 0);
    writeFile(user / "a.txt", "committed\n");
    ASSERT_EQ(gitIn(user, {"add", "a.txt"}).exitCode, 0);
    ASSERT_EQ(gitIn(user, {"commit", "--quiet", "-m", "the user's project"}).exitCode, 0);
    fs::path const worktree = scratch.path() / "wt";
    ASSERT_EQ(gitIn(user, {"worktree", "add", "--quiet", "--detach", worktree.string()})
                  .exitCode,
              0);
    writeFile(worktree / "staged.txt", "STAGED WORK\n");
    ASSERT_EQ(gitIn(worktree, {"add", "staged.txt"}).exitCode, 0);
    fs::path const worktreeGitDir = user / ".git" / "worktrees" / "wt";
    fs::path const worktreeIndex  = worktreeGitDir / "index";
    std::string const indexBefore = readFile(worktreeIndex);
    ASSERT_FALSE(indexBefore.empty()) << worktreeIndex.string();

    // Where the cache puts a dependency: inside the user's own tree.
    fs::path const dep = worktree / ".dss-deps" / "scale";
    GitCommandResult cloned;
    GitCommandResult checkedOut;
    GitCommandResult head;
    {
        // Exactly the two variables git exported to the measured hook.
        dss::test_support::ScopedEnv const gitDir{"GIT_DIR", worktreeGitDir.string()};
        dss::test_support::ScopedEnv const index{"GIT_INDEX_FILE", worktreeIndex.string()};
        dss::SystemGitRunner git;
        cloned     = git.clone(origin.string(), dep);
        checkedOut = git.checkout(dep, "HEAD");
        head       = git.revParse(dep, "HEAD");
    }
    EXPECT_TRUE(cloned.ok) << cloned.detail;
    EXPECT_TRUE(checkedOut.ok) << checkedOut.detail;
    EXPECT_TRUE(head.ok) << head.detail;
    EXPECT_EQ(readFile(worktreeIndex), indexBefore)
        << "the user's worktree index was REWRITTEN by a git aimed at a dependency "
        << "— the inherited GIT_INDEX_FILE reached it, and the staged work is gone";
    EXPECT_TRUE(fs::exists(dep / ".git" / "index"))
        << "the dependency's own index was never written, so its git wrote it "
        << "somewhere else";
}

// The list the runner withholds must cover everything the INSTALLED git calls
// repository-local (`git rev-parse --local-env-vars`), less the two config
// carriers git's own `sanitize_repo_env` keeps. A git that adds a variable fails
// here, on a developer's machine, instead of reaching a user's repository.
TEST(SystemGitRunnerIsolation, TheWithheldListCoversEverythingTheInstalledGitCallsRepositoryLocal) {
    auto const gitExe = dss::substrate::resolveExecutableOnPath("git");
    if (!gitExe) {
        GTEST_SKIP() << "`git` is not on PATH, so the installed git cannot be asked "
                        "which variables it calls repository-local";
    }
    ScratchDir     scratch{Location::Temp, "dep-git-local-env"};
    fs::path const out = scratch.path() / "local-env-vars.txt";
    auto const     r   = dss::substrate::spawnAndWaitRedirectStdout(
        {gitExe->string(), "rev-parse", "--local-env-vars"}, scratch.path(), out);
    ASSERT_TRUE(r.spawned && r.exitCode == 0) << r.diagnostic;

    auto const&              ours = dss::gitRepositoryLocalVariables();
    std::istringstream       lines{readFile(out)};
    std::size_t              reported = 0;
    std::vector<std::string> handedOn;
    for (std::string name; std::getline(lines, name);) {
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty()) continue;
        ++reported;
        if (name == "GIT_CONFIG_PARAMETERS" || name == "GIT_CONFIG_COUNT") continue;
        if (std::find(ours.begin(), ours.end(), name) == ours.end()) {
            handedOn.push_back(name);
        }
    }
    EXPECT_GT(reported, 0u) << "git named no variable at all, so nothing was compared";
    std::string missing;
    for (auto const& name : handedOn) missing += " " + name;
    EXPECT_TRUE(handedOn.empty())
        << "the installed git calls these repository-local, and the runner would "
        << "hand them to a dependency's git:" << missing;
}

// ═══════════════════════════════════════════════════════════════════════════
// THE LOCKFILE. ABSENT and UNPARSEABLE are DIFFERENT FACTS (U-4).
// ═══════════════════════════════════════════════════════════════════════════

TEST(DependencyLockfileTest, AbsentIsAnEmptyLockfileAndSaysNothing) {
    ScratchDir         scratch{Location::Temp, "dep-lock"};
    DiagnosticReporter rep;
    auto const lock = DependencyLockfile::load(scratch.path() / "dss-lock.json", rep);
    ASSERT_TRUE(lock.has_value())
        << "a first build has no lockfile and a miss is definitional";
    EXPECT_EQ(lock->size(), 0u);
    EXPECT_EQ(rep.all().size(), 0u)
        << "a diagnostic here would fire on every clean checkout of every "
           "project";
}

TEST(DependencyLockfileTest, RoundTripsEntriesWithAndWithoutARef) {
    ScratchDir         scratch{Location::Temp, "dep-lock"};
    DiagnosticReporter rep;
    auto const         path = scratch.path() / ".dss-deps" / "dss-lock.json";

    DependencyLockfile written;
    written.record("bar", LockedDependency{kUrl, std::string{"v1"}, "aaa111"});
    written.record("baz",
                   LockedDependency{"https://example.invalid/baz", std::nullopt,
                                    "bbb222"});
    ASSERT_TRUE(written.save(path, rep));
    ASSERT_EQ(rep.errorCount(), 0u);

    auto const read = DependencyLockfile::load(path, rep);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->size(), 2u);
    auto const bar = read->find("bar");
    ASSERT_TRUE(bar.has_value());
    EXPECT_EQ(*bar, (LockedDependency{kUrl, std::string{"v1"}, "aaa111"}));
    auto const baz = read->find("baz");
    ASSERT_TRUE(baz.has_value());
    // ★ ABSENT ref survives as ABSENT, not as "". The two are different states
    // — the hit test compares the whole `(url, ref)` pair — and storing a
    // sentinel would make them indistinguishable after one round trip.
    EXPECT_FALSE(baz->ref.has_value());
    EXPECT_EQ(rep.all().size(), 0u);
}

TEST(DependencyLockfileTest, SaveReplacesTheDocumentAndLeavesNoScratchFile) {
    ScratchDir         scratch{Location::Temp, "dep-lock"};
    DiagnosticReporter rep;
    auto const         path = scratch.path() / "dss-lock.json";

    DependencyLockfile first;
    first.record("bar", LockedDependency{kUrl, std::nullopt, "aaa111"});
    ASSERT_TRUE(first.save(path, rep));

    DependencyLockfile second;
    second.record("bar", LockedDependency{kUrl, std::nullopt, "bbb222"});
    ASSERT_TRUE(second.save(path, rep));

    auto const read = DependencyLockfile::load(path, rep);
    ASSERT_TRUE(read.has_value());
    ASSERT_TRUE(read->find("bar").has_value());
    EXPECT_EQ(read->find("bar")->resolvedCommit, "bbb222")
        << "the file must be OVERWRITTEN — the artifact writer's "
           "exclusive-create discipline does not transfer to a file that is "
           "rewritten on every build";
    // Every scratch name the save could have used — the unique
    // `dss-lock.json.tmp-<pid>-<n>` and the old fixed `dss-lock.json.tmp` —
    // must be gone. Scanned, not probed by one spelling, so a renamed scheme
    // cannot make this vacuous.
    EXPECT_TRUE(scratchFilesBeside(path).empty())
        << "a temp-then-rename scratch file survived a successful save";
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── [[D-DEPS-LOCKFILE-STAGES-THROUGH-ONE-FIXED-TEMP-NAME]] ─────────────────
//
// The save used to stage through ONE fixed name, `dss-lock.json.tmp`, opened
// truncating, so two builds saving at once wrote the SAME file. ✔MEASURED
// (lane `rw`, the pre-fix sequence replicated, two writers, 30 s): 3500 times a
// TORN document was committed — and an unparseable lockfile abandons the next
// build (U-4). Each save now CLAIMS a unique `dss-lock.json.tmp-<pid>-<n>` with
// an exclusive create. A file another writer holds is simulated here by a
// sentinel sitting at the name — the only deterministic way to be "another
// save, mid-write" without a timer.
//
// RED ON DISABLE, per mechanism — each test reds under exactly one mutant:
//   * back to the fixed `.tmp` name      -> `…NeverWritesThroughTheOldFixed…`;
//   * unique names, but opened TRUNCATING -> `…StepsOverAnOccupiedScratchName…`.

TEST(DependencyLockfileTest, ASaveNeverWritesThroughTheOldFixedScratchName) {
    ScratchDir         scratch{Location::Temp, "dep-lock"};
    DiagnosticReporter rep;
    auto const         path     = scratch.path() / "dss-lock.json";
    auto const         fixed    = scratch.path() / "dss-lock.json.tmp";
    std::string const  sentinel = "another save's document, mid-write\n";
    writeFile(fixed, sentinel);

    DependencyLockfile lock;
    lock.record("bar", LockedDependency{kUrl, std::nullopt, "aaa111"});
    ASSERT_TRUE(lock.save(path, rep)) << "the save itself must succeed";
    EXPECT_EQ(readFile(fixed), sentinel)
        << "the save wrote through the old FIXED scratch name — two builds "
           "saving at once share it, and a torn document gets committed";
    auto const read = DependencyLockfile::load(path, rep);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->find("bar")->resolvedCommit, "aaa111");
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(DependencyLockfileTest, ASaveStepsOverAnOccupiedScratchNameAndNeverTruncatesIt) {
    ScratchDir         scratch{Location::Temp, "dep-lock"};
    DiagnosticReporter rep;
    auto const         path = scratch.path() / "dss-lock.json";
    // The FIRST name this process's save would claim — held by "another save".
    auto const occupied =
        scratch.path() / ("dss-lock.json.tmp-" + std::to_string(thisProcessId()) + "-0");
    std::string const sentinel = "another save's document, mid-write\n";
    writeFile(occupied, sentinel);

    DependencyLockfile lock;
    lock.record("bar", LockedDependency{kUrl, std::nullopt, "bbb222"});
    ASSERT_TRUE(lock.save(path, rep)) << "an occupied name must be stepped over, "
                                         "not reported";
    EXPECT_EQ(readFile(occupied), sentinel)
        << "the save opened a scratch name it did not create — a claim must be "
           "EXCLUSIVE, or two saves share one file again";
    auto const read = DependencyLockfile::load(path, rep);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->find("bar")->resolvedCommit, "bbb222");
    auto const left = scratchFilesBeside(path);
    EXPECT_EQ(left, (std::vector<std::string>{occupied.filename().string()}))
        << "only the occupant may remain: the save's own scratch file must be "
           "gone, and the occupant must not";
    EXPECT_EQ(rep.errorCount(), 0u);
}

// Each shape gets ONE `C_MalformedJson` and a FAILED load. Treating any of them
// as an empty cache is the tolerant fallback that hides a failure.
TEST(DependencyLockfileTest, EveryMalformedShapeFailsLoudWithExactlyOneCode) {
    struct Case {
        char const* label;
        char const* text;
    };
    Case const cases[] = {
        {"not json", "{ this is not json"},
        {"root not an object", "[]"},
        {"unknown root key", R"({"dependencies":{},"extra":1})"},
        {"no dependencies member", R"({})"},
        {"dependencies not an object", R"({"dependencies":[]})"},
        {"entry not an object", R"({"dependencies":{"bar":"x"}})"},
        {"entry unknown key",
         R"({"dependencies":{"bar":{"url":"u","resolvedCommit":"c","x":1}}})"},
        {"missing url", R"({"dependencies":{"bar":{"resolvedCommit":"c"}}})"},
        {"missing commit", R"({"dependencies":{"bar":{"url":"u"}}})"},
        {"empty url", R"({"dependencies":{"bar":{"url":"","resolvedCommit":"c"}}})"},
        {"ref present but not a string",
         R"({"dependencies":{"bar":{"url":"u","ref":7,"resolvedCommit":"c"}}})"},
    };
    for (auto const& c : cases) {
        ScratchDir         scratch{Location::Temp, "dep-lock"};
        DiagnosticReporter rep;
        auto const         path = scratch.path() / "dss-lock.json";
        writeFile(path, c.text);

        auto const lock = DependencyLockfile::load(path, rep);
        EXPECT_FALSE(lock.has_value()) << c.label;
        EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u) << c.label;
        EXPECT_EQ(rep.errorCount(), 1u) << c.label;
    }
}

// The document this build WRITES carries a `$comment` telling the reader not to
// edit it, and it round-trips because the reader applies the codebase-wide `$`
// documentation-key carve-out. A reader that rejected its own writer's output
// would make the second build of every project fail.
TEST(DependencyLockfileTest, ItsOwnCommentKeyRoundTrips) {
    ScratchDir         scratch{Location::Temp, "dep-lock"};
    DiagnosticReporter rep;
    auto const         path = scratch.path() / "dss-lock.json";

    DependencyLockfile lock;
    lock.record("bar", LockedDependency{kUrl, std::nullopt, "aaa111"});
    ASSERT_TRUE(lock.save(path, rep));
    EXPECT_NE(readFile(path).find("$comment"), std::string::npos)
        << "the file lands in a user's project and gets opened; it has to say "
           "it is machine-managed";
    EXPECT_TRUE(DependencyLockfile::load(path, rep).has_value());
    EXPECT_EQ(rep.all().size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// U-3 — `git` IS A HARD REQUIREMENT, ONCE PER BUILD.
// ═══════════════════════════════════════════════════════════════════════════

TEST(DependencyCacheGate, MissingGitFailsLoudExactlyOncePerBuild) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    git.available = false;
    DiagnosticReporter rep;

    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());

    EXPECT_FALSE(cache->requireGit(rep));
    EXPECT_FALSE(cache->requireGit(rep));
    EXPECT_FALSE(cache->requireGit(rep));
    // ONE diagnostic for three asks. 0xD01D's allocation requires it: N copies
    // of "git is not installed" is noise, "and the reporter's per-code cap must
    // not be the thing that hides it".
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitNotFound), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
    // ...and the probe itself is latched, not merely the message.
    EXPECT_EQ(git.isAvailableCalls, 1);
}

TEST(DependencyCacheGate, PresentGitSaysNothing) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    EXPECT_TRUE(cache->requireGit(rep));
    EXPECT_EQ(rep.all().size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// M7 / 0xD020 / 0xD024 — THE NAME-REGISTRATION PRE-PASS, WHICH RUNS BEFORE A
// SINGLE BYTE IS FETCHED.
// ═══════════════════════════════════════════════════════════════════════════

TEST(DependencyCacheNames, IllegalDerivedNameFailsBeforeAnyNetworkAccess) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());

    EXPECT_FALSE(
        cache->registerGitDependency("https://example.invalid/a b.git",
                                     std::nullopt, rep)
            .has_value());
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyDerivedNameInvalid), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(git.cloneCalls, 0);

    // 0xD024's allocation fixes what the message must contain: the URL, the
    // derived NAME (which appears nowhere in the manifest) and the offending
    // character.
    std::string const text = rep.all()[0].actual;
    EXPECT_NE(text.find("https://example.invalid/a b.git"), std::string::npos);
    EXPECT_NE(text.find("'a b'"), std::string::npos);
    EXPECT_NE(text.find("[A-Za-z0-9._-]"), std::string::npos);
    // ...and it must NOT imply an escape hatch that does not exist: the
    // `dependsOn` entry is a closed three-key set.
    EXPECT_EQ(text.find("directory name explicitly"), std::string::npos);
}

TEST(DependencyCacheNames, TwoDistinctUrlsDerivingOneNameCollideBeforeAcquisition) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());

    ASSERT_TRUE(cache->registerGitDependency("https://host-a.invalid/bar.git",
                                             std::nullopt, rep)
                    .has_value());
    EXPECT_FALSE(cache->registerGitDependency("https://host-b.invalid/bar.git",
                                              std::nullopt, rep)
                     .has_value());

    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitNameCollision), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
    // ★ NOTHING WAS FETCHED. The collision must be found on the DERIVED NAMES
    // before acquisition, "not discovered when a clone lands on a non-empty
    // directory, or the first repo's working tree is already damaged by the
    // time we complain".
    EXPECT_EQ(git.cloneCalls, 0);
    EXPECT_EQ(git.fetchCalls, 0);
    // Both entries are named, so the reader can tell which two collided.
    EXPECT_NE(rep.all()[0].actual.find("host-a.invalid"), std::string::npos);
    EXPECT_NE(rep.all()[0].actual.find("host-b.invalid"), std::string::npos);
}

// Shape (b) of 0xD020, and the one post-acquisition detection could never see:
// the derived name is trivially identical, so the second entry's checkout
// target already exists and looks like a cache hit.
TEST(DependencyCacheNames, SameUrlWithTwoDifferentRefsCollides) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());

    ASSERT_TRUE(cache->registerGitDependency(kUrl, std::string{"v1"}, rep)
                    .has_value());
    EXPECT_FALSE(cache->registerGitDependency(kUrl, std::string{"v2"}, rep)
                     .has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitNameCollision), 1u);
    EXPECT_EQ(git.cloneCalls, 0);
}

// THE DIAMOND. Same url, same ref, named twice: one node reached by two edges,
// not a conflict. It dedups SILENTLY, exactly as a repeated `path` dependency
// does — a diagnostic here would fire on every legitimate shared dependency.
TEST(DependencyCacheNames, SameUrlAndSameRefIsADiamondAndSaysNothing) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());

    auto const first  = cache->registerGitDependency(kUrl, std::string{"v1"}, rep);
    auto const second = cache->registerGitDependency(kUrl, std::string{"v1"}, rep);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(rep.all().size(), 0u);

    // Absent-ref-twice is the same case and must behave identically — the
    // discriminator is the full `(url, ref)` pair, and `nullopt == nullopt`.
    DiagnosticReporter rep2;
    auto cache2 = DependencyCache::open(scratch.path(), git, false, rep2);
    ASSERT_TRUE(cache2.has_value());
    ASSERT_TRUE(cache2->registerGitDependency(kUrl, std::nullopt, rep2).has_value());
    ASSERT_TRUE(cache2->registerGitDependency(kUrl, std::nullopt, rep2).has_value());
    EXPECT_EQ(rep2.all().size(), 0u);
}

// ═══════════════════════════════════════════════════════════════════════════
// B.4 — THE FOUR OUTCOMES.
// ═══════════════════════════════════════════════════════════════════════════

TEST(DependencyCacheMachine, MissClonesChecksOutAndRecordsTheCommit) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    git.commitByRev["v1"] = "commit-v1-aaa";

    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::string{"v1"}, rep);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(*name, "bar");

    auto const got = cache->acquire(*name, rep);
    EXPECT_EQ(got.outcome, CacheOutcome::Miss);
    EXPECT_EQ(got.checkout, cache->depsDir() / "bar");
    EXPECT_EQ(got.resolvedCommit, "commit-v1-aaa");
    EXPECT_EQ(rep.all().size(), 0u) << "a successful acquisition says nothing";

    EXPECT_EQ(git.cloneCalls, 1);
    EXPECT_EQ(git.fetchCalls, 0);
    ASSERT_EQ(git.checkedOutRevs.size(), 1u);
    EXPECT_EQ(git.checkedOutRevs[0], "v1")
        << "the declared ref is applied on the clone path";
    EXPECT_TRUE(fs::is_directory(cache->depsDir() / "bar"));

    ASSERT_TRUE(cache->save(rep));
    auto const locked = cache->lockfile().find("bar");
    ASSERT_TRUE(locked.has_value());
    EXPECT_EQ(*locked,
              (LockedDependency{kUrl, std::string{"v1"}, "commit-v1-aaa"}));
}

// ★ THE CENTRAL PIN. Two resolves, two separate cache objects, ONE clone — so
// this reds if the lockfile write breaks, if the load breaks, or if the hit
// short-circuit is not consulted. And the hit's git budget is asserted
// EXACTLY: `clone == 0 && fetch == 0 && revParse == 1`. `clone == 0` alone
// lets a stray fetch through, and B.4's guarantee is "not a conditional
// request, not an `ls-remote`, nothing".
TEST(DependencyCacheMachine, SecondResolveIsAHitWithExactlyOneGitInvocation) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;

    {
        auto cache = DependencyCache::open(scratch.path(), git, false, rep);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
        ASSERT_TRUE(name.has_value());
        ASSERT_EQ(cache->acquire(*name, rep).outcome, CacheOutcome::Miss);
        ASSERT_TRUE(cache->save(rep));
    }
    ASSERT_EQ(git.cloneCalls, 1);
    int const cloneAfterMiss    = git.cloneCalls;
    int const fetchAfterMiss    = git.fetchCalls;
    int const revParseAfterMiss = git.revParseCalls;

    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    EXPECT_EQ(got.outcome, CacheOutcome::Hit);
    EXPECT_EQ(got.resolvedCommit, "commit-default");
    EXPECT_EQ(git.cloneCalls - cloneAfterMiss, 0);
    EXPECT_EQ(git.fetchCalls - fetchAfterMiss, 0);
    EXPECT_EQ(git.revParseCalls - revParseAfterMiss, 1)
        << "a hit costs exactly the one `rev-parse HEAD` that validates it";
    EXPECT_EQ(git.cloneCalls, 1)
        << "clone must have happened exactly once across BOTH resolves — this "
           "reds if the lockfile write or read broke";
    EXPECT_EQ(rep.all().size(), 0u);
}

// U-3's reason for verifying HEAD at all: a checkout somebody moved is the
// stale-but-unnoticed state, and it must NOT be served as a hit.
TEST(DependencyCacheMachine, CheckoutMovedBehindOurBackIsNotAHit) {
    PopulatedCache p;
    ASSERT_NO_FATAL_FAILURE(p.seed());
    DiagnosticReporter rep;

    FakeGitRunner::writeHead(p.scratch.path() / ".dss-deps" / "bar",
                             "somebody-elses-commit");
    int const fetchBefore = p.git.fetchCalls;

    auto cache = DependencyCache::open(p.scratch.path(), p.git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    EXPECT_NE(got.outcome, CacheOutcome::Hit)
        << "the recorded commit and HEAD disagree, so the cache cannot claim "
           "this checkout is what the lockfile describes";
    EXPECT_GT(p.git.fetchCalls, fetchBefore)
        << "and the miss path must actually run";
}

// ═══════════════════════════════════════════════════════════════════════════
// `--force-git-cache`.
// ═══════════════════════════════════════════════════════════════════════════

// ★ THE FLAG MUST MOVE HEAD. A fetch alone leaves HEAD where it was, so
// `rev-parse` returns the OLD commit and the flag is a network round trip that
// changes nothing — and a test asserting `fetchCallCount == 1` would go green
// over an entirely absent mechanism. So: the NEW COMMIT, and the REWRITTEN
// LOCKFILE ON DISK.
TEST(DependencyCacheForce, ForceRefreshMovesHeadAndRewritesTheLockfile) {
    PopulatedCache p;
    ASSERT_NO_FATAL_FAILURE(p.seed());
    auto const lockPath = p.scratch.path() / ".dss-deps" / "dss-lock.json";
    ASSERT_NE(readFile(lockPath).find("commit-default"), std::string::npos);

    DiagnosticReporter rep;
    // The remote has moved on: FETCH_HEAD now names a new tip.
    p.git.commitByRev["FETCH_HEAD"] = "commit-brand-new";

    auto cache = DependencyCache::open(p.scratch.path(), p.git, /*force=*/true, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);
    ASSERT_TRUE(cache->save(rep));

    EXPECT_EQ(got.outcome, CacheOutcome::Miss)
        << "the flag bypasses the short-circuit and nothing else, so what runs "
           "after it IS the miss path";
    EXPECT_EQ(got.resolvedCommit, "commit-brand-new");
    EXPECT_EQ(p.git.fetchCalls, 1);
    ASSERT_FALSE(p.git.checkedOutRevs.empty());
    EXPECT_EQ(p.git.checkedOutRevs.back(), "FETCH_HEAD")
        << "checking out the ref by NAME would move nothing when it is a "
           "branch that is already checked out";

    std::string const onDisk = readFile(lockPath);
    EXPECT_NE(onDisk.find("commit-brand-new"), std::string::npos)
        << "the lockfile must record the revision this build actually used";
    EXPECT_EQ(onDisk.find("commit-default"), std::string::npos)
        << "and must no longer claim the old one";
    EXPECT_EQ(rep.errorCount(), 0u);
}

// The flag changes ONLY the short-circuit. Every other rule is unchanged by
// it, so `--force-git-cache` on an offline machine with an existing checkout
// still emits 0xD01F and still builds.
TEST(DependencyCacheForce, ForceWithAFailingFetchStillFallsBackAndBuilds) {
    PopulatedCache p;
    ASSERT_NO_FATAL_FAILURE(p.seed());
    DiagnosticReporter rep;
    p.git.fetchSucceeds = false;

    auto cache = DependencyCache::open(p.scratch.path(), p.git, /*force=*/true, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    EXPECT_EQ(got.outcome, CacheOutcome::FetchFallback);
    EXPECT_EQ(got.resolvedCommit, "commit-default");
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitFetchFallback), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitAcquireFailed), 0u)
        << "the discriminator is `is there a usable checkout` and NOTHING "
           "else — not the flag, not the operation name, not the exit status";
    EXPECT_EQ(rep.errorCount(), 0u) << "the build proceeds";
}

// ═══════════════════════════════════════════════════════════════════════════
// THE TWO NETWORK-FAILURE ARMS.
// ═══════════════════════════════════════════════════════════════════════════

TEST(DependencyCacheFailure, FetchFallbackProceedsAtInfoWithGuaranteedDelivery) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter arrange;
    {
        // Acquire, but deliberately do NOT save: the next open sees a real
        // checkout with no lock entry, which is not a hit, so the fetch path
        // runs.
        auto cache = DependencyCache::open(scratch.path(), git, false, arrange);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, std::nullopt, arrange);
        ASSERT_TRUE(name.has_value());
        ASSERT_EQ(cache->acquire(*name, arrange).outcome, CacheOutcome::Miss);
    }

    DiagnosticReporter rep;
    git.fetchSucceeds = false;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    EXPECT_EQ(got.outcome, CacheOutcome::FetchFallback);
    EXPECT_FALSE(got.checkout.empty()) << "the build proceeds on what it has";
    EXPECT_EQ(got.resolvedCommit, "commit-default");

    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitFetchFallback), 1u);
    ASSERT_EQ(rep.all().size(), 1u);
    EXPECT_EQ(rep.all()[0].severity, DiagnosticSeverity::Info);
    // ★ THE DISCRIMINATING ASSERTION FOR THE DELIVERY PROPERTY. Membership in
    // `kUnsuppressableCodes` ALSO bypasses the cap, so a "did it survive a
    // saturated cap" arm alone would pass whether or not the field was set —
    // vacuously green. The field itself is the thing the split requires: a code
    // must obtain delivery on delivery's own merits, never as a side effect of
    // a suppression verdict.
    EXPECT_EQ(rep.all()[0].delivery, DiagnosticDelivery::Guaranteed);
    EXPECT_EQ(rep.errorCount(), 0u);

    // ★ AND IT IS DELIBERATELY NOT RECORDED. Recording this commit would make
    // the next build a hit, which would stop re-trying and stop saying
    // anything — the staleness would quietly become the recorded truth.
    EXPECT_FALSE(cache->lockfile().find("bar").has_value());
}

// The behavioural half: it really does survive a stream that has already
// blown the global cap.
TEST(DependencyCacheFailure, FetchFallbackSurvivesASaturatedDiagnosticCap) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter arrange;
    {
        auto cache = DependencyCache::open(scratch.path(), git, false, arrange);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, std::nullopt, arrange);
        ASSERT_TRUE(name.has_value());
        ASSERT_EQ(cache->acquire(*name, arrange).outcome, CacheOutcome::Miss);
    }

    DiagnosticReporter::Config cfg;
    cfg.maxDiagnostics = 1;
    cfg.dedupWindow    = 0;
    DiagnosticReporter rep{cfg};
    dss::report(rep, DiagnosticCode::P_UnexpectedToken, DiagnosticSeverity::Error,
                "filler one");
    dss::report(rep, DiagnosticCode::P_UnknownToken, DiagnosticSeverity::Error,
                "filler two");
    ASSERT_TRUE(rep.hitCap()) << "the cap must actually be saturated, or this "
                                 "test proves nothing";

    git.fetchSucceeds = false;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    ASSERT_EQ(cache->acquire(*name, rep).outcome, CacheOutcome::FetchFallback);

    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitFetchFallback), 1u)
        << "the one line saying the build used sources it could not refresh "
           "must not be what the cap eats";
}

// THREE-SIDED, because zero Infos also makes a two-sided version green.
TEST(DependencyCacheFailure, FetchFallbackDoesNotFailAWarningsAsErrorsBuild) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter arrange;
    {
        auto cache = DependencyCache::open(scratch.path(), git, false, arrange);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, std::nullopt, arrange);
        ASSERT_TRUE(name.has_value());
        ASSERT_EQ(cache->acquire(*name, arrange).outcome, CacheOutcome::Miss);
    }

    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter rep{cfg};

    git.fetchSucceeds = false;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    ASSERT_EQ(cache->acquire(*name, rep).outcome, CacheOutcome::FetchFallback);

    // (1) present exactly once — a version that emitted nothing would satisfy
    //     (2) and (3) vacuously;
    ASSERT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitFetchFallback), 1u);
    // (2) still Info after `applyPolicy` — the elevation arm is code-agnostic
    //     and would promote a Warning here;
    ASSERT_EQ(rep.all().size(), 1u);
    EXPECT_EQ(rep.all()[0].severity, DiagnosticSeverity::Info);
    // (3) and the strict-mode build still passes, which is the whole point.
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(DependencyCacheFailure, NoCheckoutAndAFailedCloneIsAHardError) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    git.cloneSucceeds = false;
    DiagnosticReporter rep;

    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    EXPECT_EQ(got.outcome, CacheOutcome::AcquireFailed);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitAcquireFailed), 1u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitFetchFallback), 0u);
    EXPECT_EQ(rep.errorCount(), 1u);
    // The path is EMPTY, not "the place it would have gone": a caller handed a
    // plausible-looking path would glob an absent tree and report a successful
    // build of nothing.
    EXPECT_TRUE(got.checkout.empty());
    EXPECT_TRUE(got.resolvedCommit.empty());
    // ★ AND THE FAILED CLONE LEFT NOTHING BEHIND. Without staging, a partial
    // clone at `.dss-deps/<name>` would wedge the cache: git refuses to clone
    // into a non-empty directory, so the project would be unbuildable until
    // somebody deleted a directory by hand inside a git-ignored tree.
    EXPECT_FALSE(fs::exists(cache->depsDir() / "bar"));
}

// A clone that succeeds and whose REF checkout fails must also leave nothing:
// a checkout landed at the wrong revision is worse than none, because the next
// build sees a usable checkout and can fall back onto it under 0xD01F.
TEST(DependencyCacheFailure, AFailedRefCheckoutDuringCloneLeavesNothingBehind) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    git.checkoutSucceeds = false;
    DiagnosticReporter rep;

    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::string{"v1"}, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    EXPECT_EQ(got.outcome, CacheOutcome::AcquireFailed);
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitAcquireFailed), 1u);
    EXPECT_FALSE(fs::exists(cache->depsDir() / "bar"));
}

// ═══════════════════════════════════════════════════════════════════════════
// U-4 AT THE CACHE BOUNDARY.
// ═══════════════════════════════════════════════════════════════════════════

// ★ THREE-SIDED: `cloneCallCount == 0` AND the failure AND exactly one
// `C_MalformedJson`. "It failed" is also true of a version that failed AFTER
// cloning, which would have already written into the user's tree on the way to
// saying no.
TEST(DependencyCacheLockfile, CorruptLockfileRefusesToOpenTheCacheAndFetchesNothing) {
    ScratchDir    scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner git;
    writeFile(scratch.path() / ".dss-deps" / "dss-lock.json", "{ truncated");

    DiagnosticReporter rep;
    auto const cache = DependencyCache::open(scratch.path(), git, false, rep);

    EXPECT_FALSE(cache.has_value())
        << "a state file the build cannot read is a different fact from a "
           "state file that is not there yet; treating it as a miss is the "
           "tolerant fallback that hides a failure";
    EXPECT_EQ(countCode(rep, DiagnosticCode::C_MalformedJson), 1u);
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(git.cloneCalls, 0);
    EXPECT_EQ(git.fetchCalls, 0);
    EXPECT_EQ(git.revParseCalls, 0);
}

// The cache directory itself is `<projectDir>/.dss-deps`, never a shared or
// user-home location and never derived from the process cwd — two consuming
// projects naming the same URL get their own checkouts.
TEST(DependencyCacheLockfile, CacheLivesBesideTheConsumingProjectsManifest) {
    ScratchDir         scratch{Location::Temp, "dep-git-cache"};
    FakeGitRunner      git;
    DiagnosticReporter rep;
    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    EXPECT_EQ(cache->depsDir(), scratch.path() / ".dss-deps");
    EXPECT_EQ(cache->lockfilePath(), scratch.path() / ".dss-deps" / "dss-lock.json");

    auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
    ASSERT_TRUE(name.has_value());
    ASSERT_EQ(cache->acquire(*name, rep).outcome, CacheOutcome::Miss);
    ASSERT_TRUE(cache->save(rep));
    EXPECT_TRUE(fs::exists(scratch.path() / ".dss-deps" / "dss-lock.json"));
    EXPECT_TRUE(fs::is_directory(scratch.path() / ".dss-deps" / "bar"));
}

// ═══════════════════════════════════════════════════════════════════════════
// LANDING A CHECKOUT — IN PLACE, MARKED, AND ONE ACQUISITION AT A TIME.
//
// The cache used to clone into a fixed `+staging/<name>` and RENAME the
// populated directory into `.dss-deps/<name>`. Three measured defects came
// with it, and each mechanism of the fix has its own pin here:
//
//   * IN PLACE — [[D-DEPS-CACHE-LANDS-A-CHECKOUT-BY-RENAMING-A-DIRECTORY-A-SCANNER-CAN-HOLD]]:
//     Windows refuses to rename a directory while ANY file beneath it is open,
//     and a real-time scan of the freshly written checkout is such a holder
//     (✔MEASURED, 6 of 500 landings refused under load). ⇒ the checkout is
//     cloned straight into `.dss-deps/<name>` (Go since 1.16, cargo).
//   * THE MARKER — an in-place tree an interrupted build left can answer
//     `rev-parse` perfectly well; `+partial/<name>` is the only thing that knows
//     it is incomplete (Go's `.partial`).
//   * THE LOCK — [[D-DEPS-CONCURRENT-ACQUISITIONS-OF-ONE-DEPENDENCY-SHARE-A-PATH-AND-CAN-HANG]]:
//     two builds acquiring one dependency shared the fixed staging path,
//     `remove_all`-ed it under each other, and on the MinGW build one HUNG
//     (✔MEASURED, 688 s of CPU in libstdc++'s `remove_all`). ⇒ a per-dependency
//     cross-process lock held for the whole acquisition.
//
// RED ON DISABLE, per mechanism:
//   * back to stage-then-rename -> `AFileHeldOpenInsideTheFreshCheckout…` (on
//     WINDOWS only: POSIX `rename(2)` of a directory does not care about open
//     files, so there the pin states the property and cannot red);
//   * the usability probe ignores the marker -> `ACheckoutMarkedIncomplete…`;
//   * no lock -> `ASecondAcquisitionWaitsAndNeverDisturbsTheFirst`.
// Every wait below is a HANDSHAKE on a condition the code under test signals —
// no sleep and no clock anywhere.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

[[nodiscard]] std::string allMessages(DiagnosticReporter const& rep) {
    std::string joined;
    for (auto const& d : rep.all()) {
        joined += d.actual;
        joined += '\n';
    }
    return joined;
}

} // namespace

TEST(DependencyCacheLanding, AFileHeldOpenInsideTheFreshCheckoutNeverFailsTheLanding) {
    ScratchDir    scratch{Location::Temp, "dep-git-landing"};
    FakeGitRunner git;
    // The real-time scan of the fresh tree, as measured: a handle on a file the
    // clone just wrote, still open when the checkout is landed.
    std::optional<std::ifstream> holder;
    git.onClone = [&holder](fs::path const& dest) {
        holder.emplace(dest / "HEAD", std::ios::binary);
    };
    DiagnosticReporter rep;

    auto cache = DependencyCache::open(scratch.path(), git, false, rep);
    ASSERT_TRUE(cache.has_value());
    auto const name = cache->registerGitDependency(kUrl, std::string{"v1"}, rep);
    ASSERT_TRUE(name.has_value());
    auto const got = cache->acquire(*name, rep);

    ASSERT_TRUE(holder.has_value() && holder->is_open())
        << "premise: a file inside the fresh checkout must be held during the "
           "landing, or this pin proves nothing";
    EXPECT_EQ(got.outcome, CacheOutcome::Miss)
        << "a file held open inside the fresh checkout failed the acquisition:\n"
        << allMessages(rep);
    EXPECT_EQ(got.checkout, cache->depsDir() / "bar")
        << "the checkout lives at `.dss-deps/<name>/` — the documented path";
    EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitAcquireFailed), 0u);
    EXPECT_FALSE(fs::exists(cache->depsDir() / "+partial" / "bar"))
        << "a completed landing must take its completeness marker down";
    holder.reset();
}

TEST(DependencyCacheLanding, ACheckoutMarkedIncompleteIsNeverTrusted) {
    ScratchDir     scratch{Location::Temp, "dep-git-partial"};
    FakeGitRunner  git;
    fs::path const deps = scratch.path() / ".dss-deps";
    // The residue of an interrupted acquisition: a tree that ANSWERS rev-parse
    // (git writes HEAD before the working tree is complete) — and the marker.
    auto const leaveResidue = [&deps] {
        fs::create_directories(deps / "bar");
        FakeGitRunner::writeHead(deps / "bar", "commit-half-written");
        writeFile(deps / "+partial" / "bar", "");
    };

    // (1) OFFLINE: there is nothing to fall back ON. 0xD01E, never 0xD01F.
    leaveResidue();
    git.cloneSucceeds = false;
    git.fetchSucceeds = false;
    {
        DiagnosticReporter rep;
        auto cache = DependencyCache::open(scratch.path(), git, false, rep);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
        ASSERT_TRUE(name.has_value());
        auto const got = cache->acquire(*name, rep);
        EXPECT_EQ(got.outcome, CacheOutcome::AcquireFailed)
            << "a tree an interrupted acquisition left was treated as a usable "
               "checkout:\n"
            << allMessages(rep);
        EXPECT_EQ(countCode(rep, DiagnosticCode::D_DependencyGitFetchFallback), 0u)
            << "the build must not PROCEED on a half-written tree";
        EXPECT_EQ(git.fetchCalls, 0)
            << "an untrusted tree is not refreshed, it is replaced";
    }

    // (2) ONLINE: the residue is removed and the dependency acquired afresh.
    leaveResidue();
    git.cloneSucceeds = true;
    {
        DiagnosticReporter rep;
        auto cache = DependencyCache::open(scratch.path(), git, false, rep);
        ASSERT_TRUE(cache.has_value());
        auto const name = cache->registerGitDependency(kUrl, std::nullopt, rep);
        ASSERT_TRUE(name.has_value());
        auto const got = cache->acquire(*name, rep);
        EXPECT_EQ(got.outcome, CacheOutcome::Miss) << allMessages(rep);
        EXPECT_EQ(got.resolvedCommit, "commit-default")
            << "the residue's commit must never be what the build records";
        EXPECT_FALSE(fs::exists(deps / "+partial" / "bar"));
    }
}

TEST(DependencyCacheConcurrency, ASecondAcquisitionWaitsAndNeverDisturbsTheFirst) {
    ScratchDir    scratch{Location::Temp, "dep-git-concurrent"};
    FakeGitRunner gitA;
    FakeGitRunner gitB;

    std::mutex              m;
    std::condition_variable cv;
    bool                    aInClone   = false;
    bool                    releaseA   = false;
    bool                    bContended = false;
    bool                    bCloning   = false;
    bool                    bDone      = false;
    fs::path                aTree;

    // A pauses INSIDE its clone — its tree half-written, whatever it holds
    // held — until the test has seen what B does.
    gitA.onClone = [&](fs::path const& dest) {
        writeFile(dest / "owner-A", "A's clone, in progress\n");
        std::unique_lock<std::mutex> lk(m);
        aTree    = dest;
        aInClone = true;
        cv.notify_all();
        cv.wait(lk, [&] { return releaseA; });
    };
    // Reached only if B clones while A is still in progress (no lock).
    gitB.onClone = [&](fs::path const&) {
        std::lock_guard<std::mutex> lk(m);
        bCloning = true;
        cv.notify_all();
    };

    DiagnosticReporter repA;
    DiagnosticReporter repB;
    auto cacheA = DependencyCache::open(scratch.path(), gitA, false, repA);
    auto cacheB = DependencyCache::open(scratch.path(), gitB, false, repB);
    ASSERT_TRUE(cacheA.has_value());
    ASSERT_TRUE(cacheB.has_value());
    ASSERT_TRUE(cacheA->registerGitDependency(kUrl, std::nullopt, repA).has_value());
    ASSERT_TRUE(cacheB->registerGitDependency(kUrl, std::nullopt, repB).has_value());
    // The handshake the lock offers: B reports that it found A's acquisition
    // in progress, just BEFORE it blocks on it.
    cacheB->setLockContentionObserver([&](std::string const&) {
        std::lock_guard<std::mutex> lk(m);
        bContended = true;
        cv.notify_all();
    });

    dss::ResolvedGitDependency gotA;
    dss::ResolvedGitDependency gotB;
    std::thread                ta([&] { gotA = cacheA->acquire("bar", repA); });
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return aInClone; });
    }
    std::thread tb([&] {
        gotB = cacheB->acquire("bar", repB);
        std::lock_guard<std::mutex> lk(m);
        bDone = true;
        cv.notify_all();
    });

    bool bWaitedForA       = false;
    bool aIntactWhenBMoved = false;
    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return bContended || bCloning || bDone; });
        bWaitedForA       = bContended && !bCloning;
        aIntactWhenBMoved = fs::exists(aTree / "owner-A");
        releaseA          = true;
        cv.notify_all();
    }
    ta.join();
    tb.join();

    EXPECT_TRUE(bWaitedForA)
        << "the second acquisition did not wait for the first one — it "
        << (bCloning ? "CLONED while the first was mid-clone"
                     : "returned without contending")
        << ":\n"
        << allMessages(repB);
    EXPECT_TRUE(aIntactWhenBMoved)
        << "the second acquisition REMOVED the first one's in-progress clone";
    EXPECT_EQ(gotA.outcome, CacheOutcome::Miss) << allMessages(repA);
    EXPECT_NE(gotB.outcome, CacheOutcome::AcquireFailed) << allMessages(repB);
    EXPECT_TRUE(fs::exists(scratch.path() / ".dss-deps" / "bar" / "HEAD"));
    EXPECT_FALSE(fs::exists(scratch.path() / ".dss-deps" / "+partial" / "bar"));
}
