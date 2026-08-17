// The `.dss-deps` git dependency cache — `src/program/{git_acquire,
// dependency_lockfile, dependency_cache}.{hpp,cpp}`.
//
// ★ EVERY TEST IN THIS FILE DRIVES A FAKE `IGitRunner`. ZERO NETWORK, ZERO
// DEPENDENCE ON `git` BEING INSTALLED. That is B.7 layer 1 and it is not
// optional: B.4's machine has FOUR outcomes and TWO of them are network
// FAILURES, which a real-git test could not reach deterministically — you
// cannot ask a working network to fail on cue, and a CI leg that tried would
// be flaky in the direction that reads as green. The seam exists so the state
// machine can be driven, and this file is the reason it exists.
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

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "program/dependency_cache.hpp"
#include "program/dependency_lockfile.hpp"
#include "program/git_acquire.hpp"

#include "diagnostic_count.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

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
// rather than in a path-keyed map, and that is deliberate rather than cute.
// The cache clones into a STAGING directory and renames it into place, so a
// map keyed by path would go stale at exactly the moment the mechanism under
// test does its most interesting work, and the fake would then be testing its
// own bookkeeping. A file inside the tree survives the rename for the same
// reason git's own `.git` directory does.
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

TEST(GitArgv, FetchNamesTheRefExplicitlyAndFallsBackToRemoteHead) {
    EXPECT_EQ(dss::gitFetchArgv("git", "v1.2.0"),
              (std::vector<std::string>{"git", "fetch", "--force", "--tags",
                                        "origin", "v1.2.0"}));
    // An absent ref asks the remote for its own default branch, which is what
    // a `{git}`-with-no-`ref` manifest entry means.
    EXPECT_EQ(dss::gitFetchArgv("git", ""),
              (std::vector<std::string>{"git", "fetch", "--force", "--tags",
                                        "origin", "HEAD"}));
}

// ★ `--detach` IS THE PIN. Checking out a BRANCH name leaves HEAD attached to
// a local branch, and the next fetch+checkout of that same branch moves
// nothing — `--force-git-cache` would cost a round trip and change nothing.
TEST(GitArgv, CheckoutAlwaysDetaches) {
    EXPECT_EQ(dss::gitCheckoutArgv("git", "FETCH_HEAD"),
              (std::vector<std::string>{"git", "checkout", "--detach",
                                        "--force", "FETCH_HEAD"}));
}

TEST(GitArgv, RevParseAsksForOneRevision) {
    EXPECT_EQ(dss::gitRevParseArgv("git", "HEAD"),
              (std::vector<std::string>{"git", "rev-parse", "HEAD"}));
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
    EXPECT_FALSE(fs::exists(fs::path{path}.replace_extension(".json.tmp")))
        << "the temp-then-rename scratch file must not survive a successful "
           "save";
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
