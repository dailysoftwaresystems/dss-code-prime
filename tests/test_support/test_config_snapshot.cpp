// THE GUARD ON THE RUN'S SHIPPED-CONFIG SNAPSHOT.
//   D-TEST-SHIPPED-CONFIG-EXPOSURE-UNFIXED-OUTSIDE-THE-SUITE-THAT-FLAKED
//
// `cmake/DssConfigSnapshot.cmake` gives every config-reading ctest entry a
// `$DSS_CONFIG_ROOT` pointed at a copy of the shipped config tree taken ONCE per
// ctest run, so the ~840 entries sample the mutable source tree once instead of
// once per `loadShipped()` call. That mechanism has two ways to be silently
// worthless, and this file exercises both rather than describing them.
//
// ★★★ (1) A `$DSS_CONFIG_ROOT` THAT MISSES IS INDISTINGUISHABLE FROM A GREEN RUN.
// `findShippedConfig`'s set-but-miss arm falls THROUGH to the cwd ancestor walk
// — deliberately, and `tests/core/test_config_path_walk.cpp` pins it — so if the
// snapshot directory were absent, wrongly named, or simply never wired to this
// entry, every test in the repository would go on reading the LIVE tree and
// every one of them would still pass. The isolation would be a comment. So this
// suite asserts the override was READ, not merely SET, and it asserts it against
// the CMake-baked `DSS_TEST_REPO_ROOT` — a value that comes from the compiler's
// command line rather than from the environment under test, so the check cannot
// be satisfied by the thing it is checking.
//
// ★★★ (2) A SNAPSHOT THAT IS STALE GREENS EVERY CONFIG-LEVEL RED-ON-DISABLE IN
// THIS REPOSITORY, WHICH IS WORSE THAN NO SNAPSHOT AT ALL. The convention this
// project treats as proof is: mutate a shipped `.json`, re-run `ctest` WITHOUT
// rebuilding, observe RED. Serve that re-run a copy taken at BUILD time — or a
// copy kept from a previous run by an innocent-looking "the destination already
// exists, skip the work" fast path — and the mutant never reaches the suite.
// Hundreds of pins keep their names and stop asserting anything. So
// `TheSnapshotIsCurrentWithTheLiveTree` compares the two trees file for file and
// reds on the first divergence: it is the standing detector for a snapshot that
// stopped being taken at run time, whatever route it took to get there.
//
// ⓘ IT IS A SIZE + WHOLE-SECOND MTIME COMPARISON, and both halves of that are
// stated rather than implied. `file(COPY)` preserves input timestamps, so a
// stale copy carries the OLD write time while the live document carries a NEW
// one; a mutation that changed a file's content but neither its byte count nor
// its write second would pass. Every mutant harness in this tree rewrites a
// document wholesale, so that residue is not a shape this repository produces —
// but it is a residue, not a proof of byte equality, and it is not offered as
// one.
//
// ⚠ WHOLE SECONDS BECAUSE THE INSTRUMENT HAS THAT RESOLUTION, NOT AS A WIDENED
// ASSERTION — and the first draft of this file got it wrong, which is why the
// measurement is recorded here. `file(COPY)` preserves the sub-second part on
// NTFS but TRUNCATES it on Linux: ✔MEASURED on WSL (Ubuntu, cmake 4.x) a source
// at `...45.091654400` copied to `...45.000000000`, so an exact `==` on
// `fs::file_time_type` would have reddened the WSL leg of every gate for a
// reason that has nothing to do with a stale snapshot. ✔ALSO MEASURED, and it
// is what keeps the whole-second form EXACT rather than a tolerance: the
// truncation FLOORS — sub-second parts .100, .500, .900 and .999 all copied to
// .000, max whole-second delta 0 — so `floor(src) == floor(dst)` holds on both
// hosts and no +/-1 slack is needed or given.
//
// ⚠ AND THE WORD `floor` IN THAT SENTENCE IS LOAD-BEARING, WHICH THE FIRST
// VERSION OF THIS FILE ASSERTED AND THEN DID NOT DO. It reached for
// `duration_cast`, which truncates toward zero rather than flooring — harmless
// for a positive duration and wrong for a negative one, and libstdc++ makes
// EVERY file timestamp negative (its `file_clock` epoch is 2174). The WSL leg of
// the P33 gate caught it; the Windows leg could not, because NTFS keeps the
// sub-second part on both sides so the two roundings agreed. See the note on
// `wholeSeconds` below. ★ The lesson is not about clocks: a claim proved for one
// host and an implementation that only MATCHES it on that host look identical in
// review and differ only under execution elsewhere.
//
// ⓘ THIS SUITE IS A STATEMENT ABOUT A ctest RUN, not about the binary. Run the
// executable directly from a shell and the first case fails by design, naming
// the missing wiring — the entry's environment is the subject.

#include "core/types/config_path_walk.hpp"   // findShippedConfig — the COMPILER-side resolver
#include "core/types/parse_diagnostic.hpp"   // DiagnosticCode
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// The value `dss_use_config_snapshot` put on this entry, or empty.
[[nodiscard]] std::string envConfigRoot() {
    char const* const v = std::getenv("DSS_CONFIG_ROOT");
    return (v == nullptr) ? std::string{} : std::string{v};
}

// The checkout this binary was COMPILED against. Independent of the environment
// under test, which is the whole reason the comparisons below use it.
[[nodiscard]] fs::path bakedRoot() {
    char const* const v = dss::test::detail::bakedRepoRoot();
    return (v == nullptr) ? fs::path{} : fs::path{v};
}

// Every regular file under `root`, keyed by its path relative to `root`, with
// its size and last-write time. `recursive_directory_iterator` reports names
// beginning with `.` — the `file(GLOB_RECURSE)` in the take script does not, so
// this is the half that covers them.
struct FileFacts {
    std::uintmax_t size{};
    std::int64_t   mtimeSeconds{};   // see the resolution note in the header block
};

// Whole seconds since the file clock's epoch. The unit the comparison is made
// in, because that is the unit `file(COPY)` reproduces on every host — see the
// measurement in the header block.
//
// ⚠⚠ `std::chrono::floor`, NEVER `duration_cast` — AND THE DIFFERENCE IS ONLY
// VISIBLE ON ONE HOST, WHICH IS WHY IT SHIPPED. `duration_cast` truncates TOWARD
// ZERO; libstdc++'s `file_clock` epoch is 2174-01-01, so every present-day
// timestamp is NEGATIVE and truncating toward zero rounds it UP — the opposite
// of the floor the header block measured `file(COPY)` to perform.
// ✔MEASURED at the P33 fold: the WSL leg red on four aarch64 documents with
// snapshot `-4650078848` against live `-4650078847`, an exact off-by-one. The
// live file keeps rsync's sub-second part, `file(COPY)` FLOORS it away on Linux,
// and `duration_cast` then rounded the two sides in opposite directions. Windows
// passed because `file(COPY)` PRESERVES the sub-second part on NTFS, so both
// sides carried the same fraction and any rounding agreed with itself.
// ★ The header block's reasoning was right and its arithmetic was not: it proved
// the COPY floors, then compared with a conversion that does not.
[[nodiscard]] std::int64_t wholeSeconds(fs::file_time_type t) {
    return std::chrono::floor<std::chrono::seconds>(t.time_since_epoch()).count();
}

[[nodiscard]] std::map<std::string, FileFacts> walkTree(fs::path const& root,
                                                        std::string&    err) {
    std::map<std::string, FileFacts> out;
    std::error_code                  ec;
    for (auto const& e : fs::recursive_directory_iterator(root, ec)) {
        if (ec) { err = "iterating " + root.string() + ": " + ec.message(); break; }
        if (!e.is_regular_file(ec)) continue;
        fs::path const rel = fs::relative(e.path(), root, ec);
        if (ec) { err = "relative(" + e.path().string() + "): " + ec.message(); break; }
        FileFacts f;
        f.size  = fs::file_size(e.path(), ec);
        if (ec) { err = "size(" + e.path().string() + "): " + ec.message(); break; }
        auto const raw = fs::last_write_time(e.path(), ec);
        if (ec) { err = "mtime(" + e.path().string() + "): " + ec.message(); break; }
        f.mtimeSeconds = wholeSeconds(raw);
        out.emplace(rel.generic_string(), f);
    }
    if (ec && err.empty()) err = "iterating " + root.string() + ": " + ec.message();
    return out;
}

} // namespace

// The wiring exists at all. A missing value here means `dss_use_config_snapshot`
// stopped reaching this entry — and because a missing override falls through to
// the live tree, that is the failure that would otherwise be invisible.
TEST(ConfigSnapshot, TheChokepointPointsThisRunAtASnapshot) {
    std::string const env = envConfigRoot();
    ASSERT_FALSE(env.empty())
        << "$DSS_CONFIG_ROOT is unset for this ctest entry. Either "
           "dss_use_config_snapshot() no longer reaches dss_add_test's entries, or "
           "this binary was invoked directly rather than through ctest. Without it "
           "every test in this repository silently reads the live "
           "src/dss-config tree again, and every one of them still passes.";

    fs::path const root{env};
    ASSERT_TRUE(fs::is_directory(root / "src" / "dss-config"))
        << root.string()
        << " does not contain src/dss-config, so this override is a set-but-miss: "
           "findShippedConfig falls THROUGH to the cwd walk and back onto the live "
           "tree. The `config/snapshot` fixture is supposed to make that "
           "impossible by failing loud instead.";
}

// THE ASSERTION THE MECHANISM EXISTS FOR: the override was READ, not merely SET.
// Compared against the baked root rather than against anything derived from the
// environment, so a broken override cannot satisfy it.
TEST(ConfigSnapshot, TheRunDoesNotReadTheLiveSourceTree) {
    ASSERT_FALSE(bakedRoot().empty())
        << "DSS_TEST_REPO_ROOT was not baked into this binary, so this case has "
           "no independent reference to compare against and can prove nothing";

    fs::path const live = bakedRoot() / "src" / "dss-config";
    fs::path const seen = dss::test::configRoot();

    EXPECT_NE(seen, live)
        << "this process resolves shipped config out of " << seen.string()
        << ", which IS the live source tree. The per-run snapshot is not in "
           "effect and every suite is exposed to a neighbour rewriting a shipped "
           "document mid-run again.";
    EXPECT_EQ(seen, fs::path{envConfigRoot()} / "src" / "dss-config")
        << "the env candidate did not win: resolution landed on " << seen.string();
}

// The COMPILER side, not only the test-side resolver. `repo_root.hpp` and
// `config_path_walk.cpp` are two different implementations of one precedence,
// and it is the compiler's that every `loadShipped()` in the suite goes through.
// Proving it here is what lets the other 800-odd entries be believed.
TEST(ConfigSnapshot, TheCompilerSideResolverAgrees) {
    auto const resolved = dss::findShippedConfig(
        {"c", "sources", ".lang.json", "language",
         dss::DiagnosticCode::C_InvalidLanguageName});
    ASSERT_TRUE(resolved.has_value())
        << "findShippedConfig could not resolve the C language document out of "
        << envConfigRoot();

    fs::path const snapshotConfig = fs::path{envConfigRoot()} / "src" / "dss-config";
    std::string const got = resolved->generic_string();
    EXPECT_EQ(got.rfind(snapshotConfig.generic_string(), 0), 0u)
        << "the compiler-side resolver answered out of " << got
        << ", not out of the run's snapshot at " << snapshotConfig.generic_string()
        << " — the test-side and compiler-side resolvers are reading different "
           "trees, which is the mixed-tree condition both of them exist to prevent";
}

// ★★★ THE ANTI-STALE GUARD. This is what keeps the snapshot a RUN-time copy:
// a build-time copy, or a "skip the work if the destination exists" fast path,
// reds here the first time anyone mutates a shipped document — which is exactly
// the sequence this repository uses to prove a config-level test is not vacuous.
TEST(ConfigSnapshot, TheSnapshotIsCurrentWithTheLiveTree) {
    ASSERT_FALSE(bakedRoot().empty());
    fs::path const live     = bakedRoot() / "src" / "dss-config";
    fs::path const snapshot = fs::path{envConfigRoot()} / "src" / "dss-config";
    ASSERT_TRUE(fs::is_directory(live))     << live.string();
    ASSERT_TRUE(fs::is_directory(snapshot)) << snapshot.string();

    std::string liveErr;
    std::string snapErr;
    auto const  liveFiles = walkTree(live, liveErr);
    auto const  snapFiles = walkTree(snapshot, snapErr);
    ASSERT_TRUE(liveErr.empty()) << liveErr;
    ASSERT_TRUE(snapErr.empty()) << snapErr;

    ASSERT_FALSE(liveFiles.empty())
        << "the live config tree at " << live.string() << " enumerated NO files, "
           "so this comparison would pass against anything";

    for (auto const& [rel, want] : liveFiles) {
        auto const it = snapFiles.find(rel);
        ASSERT_NE(it, snapFiles.end())
            << rel
            << " exists in the live config tree but NOT in this run's snapshot. "
               "The snapshot is stale — it was not taken from the tree as it "
               "stands now, so a shipped-config mutant would never reach this "
               "run and every config-level red-on-disable in this repository "
               "would be vacuous.";
        EXPECT_EQ(it->second.size, want.size)
            << rel << ": snapshot holds " << it->second.size
            << " bytes, the live tree holds " << want.size
            << ". Either the snapshot is stale (see the note above) or the live "
               "tree moved UNDER this run, which voids the run's verdict rather "
               "than saying anything about the subject.";
        EXPECT_EQ(it->second.mtimeSeconds, want.mtimeSeconds)
            << rel
            << ": snapshot and live tree disagree on last-write SECOND (snapshot "
            << it->second.mtimeSeconds << ", live " << want.mtimeSeconds
            << "). file(COPY) reproduces the write time to whole-second "
               "resolution on every host we run, so a disagreement means this "
               "copy was not taken from the current tree at the start of this "
               "run.";
    }

    for (auto const& [rel, unused] : snapFiles) {
        (void)unused;
        EXPECT_NE(liveFiles.find(rel), liveFiles.end())
            << rel
            << " exists in this run's snapshot but NOT in the live config tree. A "
               "document deleted from the tree must disappear from the snapshot "
               "too, or a test could keep resolving a config that no longer ships.";
    }
}

// The other half of the split this change made: `$DSS_CONFIG_ROOT` names the
// CONFIG tree and nothing else. `repoRoot()` — which reaches tests/corpus,
// tests/hir/lowering_goldens, examples/, scripts/ and src/ — must keep naming the
// checkout these binaries were built from, or pointing the suite at a
// config-only snapshot would take every golden file in the repository with it.
TEST(ConfigSnapshot, RepoRootStillNamesTheCheckoutTheseTestsWereBuiltFrom) {
    ASSERT_FALSE(bakedRoot().empty());
    EXPECT_EQ(dss::test::repoRoot(), bakedRoot())
        << "repoRoot() answered " << dss::test::repoRoot().string()
        << " while this binary was built from " << bakedRoot().string();
    EXPECT_TRUE(fs::is_directory(dss::test::corpusRoot()))
        << dss::test::corpusRoot().string()
        << " — the golden-file corpus must not follow $DSS_CONFIG_ROOT";
}
