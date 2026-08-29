// Tests for the shipped-config locator (`findShippedConfig`, config_path_walk).
// Focus: the `DSS_CONFIG_ROOT` env override that makes discovery independent of
// the launch cwd — the out-of-tree/CI build fix. The historical cwd-walk itself
// is exercised transitively by every loadShipped-using test in the suite.
//
// The second half covers `findShippedConfigDir`, the DIRECTORY form of the same
// precedence. It exists because three call sites had hand-rolled that walk and
// one of them (`program.cpp::applySystemDirs`) had dropped the env branch, so
// the shipped CLI could not resolve `#include <stdio.h>` from any cwd outside
// its own source tree. These are HERMETIC: every tree is planted under a temp
// scratch dir, so they answer the same in-tree and out-of-tree. The user-facing
// symptom itself is pinned separately by
// `tests/program/test_system_dirs_cwd_independent.cpp` — a unit test of this
// primitive alone would not have caught it, because the primitive was fine and
// the caller never called it.

#include "core/types/config_path_walk.hpp"
#include "repo_root.hpp"     // the repo-root VERSION — the compiler's own identity
#include "unc_spelling.hpp"  // the ONE multi-separator-root fixture
#include "scoped_env.hpp"    // the ONE env override (this file used to carry a copy)
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

using dss::DiagnosticCode;
using dss::findShippedConfig;
using dss::findShippedConfigDir;
using dss::ShippedConfigLocator;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

ShippedConfigLocator targetLocator(std::string_view name) {
    return ShippedConfigLocator{name, "targets", ".target.json", "target",
                                DiagnosticCode::C_InvalidTargetName};
}

// Plant `<root>/src/dss-config/targets/<stem>.target.json`. Content is
// irrelevant — findShippedConfig only checks existence — so an empty object
// suffices. Returns the planted file path.
fs::path plantTarget(fs::path const& root, std::string const& stem) {
    fs::path const dir = root / "src" / "dss-config" / "targets";
    std::error_code ec;
    fs::create_directories(dir, ec);
    EXPECT_FALSE(ec) << "plantTarget: create_directories failed: " << ec.message();
    fs::path const file = dir / (stem + ".target.json");
    std::ofstream(file) << "{}\n";
    return file;
}

// Plant `<root>/src/dss-config/<subdir>` as a DIRECTORY — what the directory
// form resolves. `subdir` may be nested (`applySystemDirs` feeds it whole
// `semantics.shippedLibDirs` strings). Returns the planted directory path.
fs::path plantConfigDir(fs::path const& root, std::string const& subdir) {
    fs::path const dir = root / "src" / "dss-config" / subdir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    EXPECT_FALSE(ec) << "plantConfigDir: create_directories failed: " << ec.message();
    return dir;
}

// Create `<root>/a/b/c` and hand back the leaf — a start point several hops
// BELOW a planted config tree, so a test can prove the walk actually climbs.
fs::path makeNestedStart(fs::path const& root) {
    fs::path const leaf = root / "a" / "b" / "c";
    std::error_code ec;
    fs::create_directories(leaf, ec);
    EXPECT_FALSE(ec) << "makeNestedStart: create_directories failed: " << ec.message();
    return leaf;
}

// RAII cwd pin. `ScratchDir::useAsCwd` deliberately REFUSES `Location::Temp`
// (its docblock: a temp scratch is outside the repo, so the schema loader's
// cwd-walk could no longer reach `src/dss-config/`). That refusal is right for
// its callers and wrong for these tests, whose entire subject is what happens
// when cwd is somewhere the walk cannot use — so they pin cwd themselves.
class ScopedCwd {
public:
    explicit ScopedCwd(fs::path const& to) : prev_(fs::current_path()) {
        fs::current_path(to);
    }
    ~ScopedCwd() {
        std::error_code ec;
        fs::current_path(prev_, ec);   // a dtor must not throw
    }
    ScopedCwd(ScopedCwd const&)            = delete;
    ScopedCwd& operator=(ScopedCwd const&) = delete;

private:
    fs::path prev_;
};

// Windows hands back mixed case / short-name forms depending on how a path was
// produced; canonicalise both sides before comparing, as the file-form tests do.
void expectSameDir(fs::path const& got, fs::path const& want) {
    EXPECT_EQ(fs::weakly_canonical(got), fs::weakly_canonical(want));
}

// Write the repo-root `VERSION` file that declares a config tree's version —
// the file the top-level CMakeLists already reads as the single source of truth.
// It sits BESIDE `src/`, so `<root>/VERSION` describes `<root>/src/dss-config`.
void plantVersion(fs::path const& root, std::string const& version) {
    std::error_code ec;
    fs::create_directories(root, ec);
    std::ofstream(root / "VERSION") << version << "\n";
}

// The version THIS BINARY was built with, taken from the same file CMake reads
// to define `DSS_PROJECT_VERSION`. Deliberately read from disk rather than
// hard-coded: a literal here would go stale on the next version bump and turn a
// real skew pin into a test that asserts a historical number.
std::string compilerVersion() {
    std::ifstream in(dss::test::repoRoot() / "VERSION", std::ios::binary);
    std::string   text{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
    auto const isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t b = 0, e = text.size();
    while (b < e && isSpace(text[b])) ++b;
    while (e > b && isSpace(text[e - 1])) --e;
    return text.substr(b, e - b);
}

// Plant `<exeDir>/<installedConfigRelDir()>/<subdir>` — a SYNTHETIC installed
// layout, composed through the resolver's own published relative path so the
// test cannot drift from the layout CMake installs. Returns the config root.
fs::path plantInstalledLayout(fs::path const& exeDir, std::string const& subdir) {
    fs::path const root =
        (exeDir / fs::path{std::string{dss::installedConfigRelDir()}}).lexically_normal();
    std::error_code ec;
    fs::create_directories(subdir.empty() ? root : root / subdir, ec);
    EXPECT_FALSE(ec) << "plantInstalledLayout: " << ec.message();
    return root;
}

bool contains(std::string const& haystack, std::string const& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

// RED-ON-DISABLE (the core of the fix): DSS_CONFIG_ROOT resolves a config the
// cwd-walk can NEVER reach — a synthetic target planted under a TEMP dir outside
// any repo/build ancestry. Remove the env override in findShippedConfig and this
// fails: the walk climbs from cwd and never sees `synth_target`. This is exactly
// what lets an OUT-OF-TREE build's ctest (cwd = a build subdir) find config.
TEST(ConfigPathWalk, EnvRootResolvesConfigOutsideCwdAncestry) {
    ScratchDir scratch(Location::Temp, "config-walk");  // temp: NOT in the repo tree
    fs::path const file = plantTarget(scratch.path(), "synth_target");
    ASSERT_TRUE(fs::exists(file));

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const res = findShippedConfig(targetLocator("synth_target"));
    ASSERT_TRUE(res.has_value())
        << "DSS_CONFIG_ROOT must resolve a config the cwd-walk cannot reach";
    EXPECT_EQ(fs::weakly_canonical(*res), fs::weakly_canonical(file));
}

// The override is NOT a `../` traversal vector: a path-like name is rejected by
// the up-front name validation BEFORE DSS_CONFIG_ROOT is ever consulted, so a
// hostile name cannot escape the config tree via the env root.
TEST(ConfigPathWalk, EnvRootDoesNotBypassPathTraversalRejection) {
    ScratchDir scratch(Location::Temp, "config-walk");
    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const res = findShippedConfig(targetLocator("../../etc/passwd"));
    ASSERT_FALSE(res.has_value());
    ASSERT_FALSE(res.error().empty());
    EXPECT_EQ(res.error().front().code, DiagnosticCode::C_InvalidTargetName)
        << "a path-like name is an invalid-name rejection, not an env resolution";
}

// A set-but-miss DSS_CONFIG_ROOT (dir exists, requested config absent) falls
// THROUGH to the cwd-walk rather than short-circuiting to a different error — a
// stale override never worsens discovery. For a bogus valid name the walk also
// misses, so the result is the normal not-found (never a crash).
TEST(ConfigPathWalk, EnvRootMissFallsThroughToWalk) {
    ScratchDir scratch(Location::Temp, "config-walk");  // empty: no src/dss-config
    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const res =
        findShippedConfig(targetLocator("definitely_not_a_real_target_xyz"));
    EXPECT_FALSE(res.has_value());
}

// ── findShippedConfigDir — the DIRECTORY form of the same precedence ────────
//
// Four behaviours, one per tier of the contract: env wins when valid, a stale
// env falls THROUGH to the walk, the walk alone works, and nothing resolves to
// nullopt. Then the two properties the call sites depend on: an explicit
// `startPath` outranks the env (the LSP's fixtures), and a nested subdir is
// accepted (`applySystemDirs` forwards whole `semantics.shippedLibDirs`
// strings, which the config permits to contain a `/`).

// (1) ENV WINS. The planted tree sits under a temp dir with no repo anywhere in
// its ancestry, so the walk can NEVER reach it — only the override can. Delete
// the env branch from `findShippedConfigDir` and this fails.
TEST(ConfigPathWalk, DirEnvRootResolvesDirOutsideCwdAncestry) {
    ScratchDir scratch(Location::Temp, "config-walk-dir");
    fs::path const planted = plantConfigDir(scratch.path(), "sources");

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const got = findShippedConfigDir("sources");
    ASSERT_TRUE(got.has_value())
        << "DSS_CONFIG_ROOT must resolve a config DIR the cwd-walk cannot reach";
    expectSameDir(*got, planted);
}

// (2) A STALE env (points at a directory with no `src/dss-config` under it)
// falls THROUGH to the walk rather than short-circuiting — a wrong override
// never worsens discovery relative to not setting one. Proven by giving the
// walk something to find: cwd is pinned inside a SECOND, planted tree.
TEST(ConfigPathWalk, DirStaleEnvRootFallsThroughToWalk) {
    ScratchDir stale(Location::Temp, "config-walk-dir");   // empty: no src/dss-config
    ScratchDir real(Location::Temp, "config-walk-dir");
    fs::path const planted = plantConfigDir(real.path(), "sources");

    ScopedEnv env("DSS_CONFIG_ROOT", stale.path().string());
    ScopedCwd cwd(makeNestedStart(real.path()));           // 3 hops below the plant

    auto const got = findShippedConfigDir("sources");
    ASSERT_TRUE(got.has_value())
        << "a set-but-miss override must fall through to the cwd walk";
    expectSameDir(*got, planted);
}

// (3) WALK ONLY — no override at all. The historical behaviour, unchanged: the
// production default is an unset variable, and it must still resolve.
TEST(ConfigPathWalk, DirWalkResolvesWithNoEnvOverride) {
    ScratchDir scratch(Location::Temp, "config-walk-dir");
    fs::path const planted = plantConfigDir(scratch.path(), "sources");

    ScopedEnv env("DSS_CONFIG_ROOT");                      // construct-to-clear
    ScopedCwd cwd(makeNestedStart(scratch.path()));

    auto const got = findShippedConfigDir("sources");
    ASSERT_TRUE(got.has_value()) << "the cwd walk must work with no override set";
    expectSameDir(*got, planted);
}

// (4) NEITHER RESOLVES → nullopt. Not an exception, not an empty path: every
// caller owns its own not-found behaviour (fall through / report "not located" /
// skip the dir so the miss fails loud downstream) and needs to see the miss.
TEST(ConfigPathWalk, DirNotFoundWhenNeitherEnvNorWalkResolves) {
    ScratchDir stale(Location::Temp, "config-walk-dir");
    ScratchDir bare(Location::Temp, "config-walk-dir");    // nothing planted

    ScopedEnv env("DSS_CONFIG_ROOT", stale.path().string());
    EXPECT_FALSE(findShippedConfigDir("sources", makeNestedStart(bare.path()))
                     .has_value());
}

// The env candidate is validated as a DIRECTORY, not merely as existing — the
// deliberate difference from the file form's `exists`. A plain FILE sitting
// where the directory should be must NOT be handed to a caller that will try to
// iterate it; discovery falls through instead.
TEST(ConfigPathWalk, DirEnvRootRejectsNonDirectoryCandidate) {
    ScratchDir scratch(Location::Temp, "config-walk-dir");
    fs::path const cfg = scratch.path() / "src" / "dss-config";
    std::error_code ec;
    fs::create_directories(cfg, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::ofstream(cfg / "sources") << "not a directory\n";   // a FILE, not a dir

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    ScratchDir bare(Location::Temp, "config-walk-dir");
    EXPECT_FALSE(findShippedConfigDir("sources", makeNestedStart(bare.path()))
                     .has_value())
        << "a FILE named `sources` is not a shipped-config directory";
}

// An explicit `startPath` means "discover from exactly here" and OUTRANKS a
// perfectly valid env override. The LSP's discovery fixtures point at a scratch
// dir; honouring the ambient environment over the caller's argument would make
// them untestable (and would have hidden the LSP segfault this cycle fixed).
TEST(ConfigPathWalk, DirExplicitStartPathOutranksEnvRoot) {
    ScratchDir viaEnv(Location::Temp, "config-walk-dir");
    ScratchDir viaStart(Location::Temp, "config-walk-dir");
    plantConfigDir(viaEnv.path(), "sources");
    fs::path const wanted = plantConfigDir(viaStart.path(), "sources");

    ScopedEnv env("DSS_CONFIG_ROOT", viaEnv.path().string());
    auto const got = findShippedConfigDir("sources", makeNestedStart(viaStart.path()));
    ASSERT_TRUE(got.has_value());
    expectSameDir(*got, wanted);
}

// A NESTED subdir resolves. `applySystemDirs` forwards `semantics.shippedLibDirs`
// verbatim, and that config permits a nested value (semantic_config.hpp cites
// "shippedLibs/windows-x86_64"), so the directory form must NOT inherit the file
// form's path-like-name rejection. This pins that: add the rejection and the
// driver's system-include dirs silently stop resolving for any nested config.
TEST(ConfigPathWalk, DirAcceptsNestedSubdir) {
    ScratchDir scratch(Location::Temp, "config-walk-dir");
    fs::path const planted = plantConfigDir(scratch.path(), "shippedLibs/windows-x86_64");

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const got = findShippedConfigDir("shippedLibs/windows-x86_64");
    ASSERT_TRUE(got.has_value()) << "a nested shippedLibDirs value must resolve";
    expectSameDir(*got, planted);
}

// ── THE INSTALLED-LAYOUT ARM ────────────────────────────────────────────────
//
// [[D-PKG-NO-PACKAGING-PATH-SHIPS-THE-CONFIG-TREE]]. Before this arm the
// resolver knew only `$DSS_CONFIG_ROOT` and a cwd walk, so an installed `dsscp`
// at `/usr/bin` invoked from a user's project walked THAT project's ancestors
// and found nothing — a packaged compiler could not resolve `#include <stdio.h>`
// even if the config tree HAD been in the package.
//
// ⚠ THESE PIN THE PURE HALF ONLY, AND THAT IS DELIBERATE. The end-to-end claim
// ("an installed compiler compiles") is proven by the `install_scratch_prefix_smoke`
// ctest entry, which installs to a scratch prefix outside the repository and
// compiles a hello-world with the source tree provably out of reach. A unit test
// CANNOT plant a real installed tree, because the only executable directory it
// could plant beside is the one holding EVERY test binary — under `ctest -j` that
// synthetic tree would silently outrank the cwd walk for every concurrent test in
// the suite. Splitting `runningExecutableDir()` (host I/O) from
// `installedConfigRootFrom()` (layout) is what makes the layout testable at all.

// The relative hop must be RELATIVE, and it must name the config tree. Relative
// is the relocatability property every packaging path here depends on — Homebrew's
// cellar, Nix's store, Scoop's app dir, a user's `tar -xf` into ~/opt all place the
// prefix somewhere unknown at build time. A baked absolute path would resolve on
// the build machine and nowhere else.
TEST(ConfigPathWalk, InstalledRelDirIsRelativeAndNamesTheConfigTree) {
    fs::path const rel{std::string{dss::installedConfigRelDir()}};
    ASSERT_FALSE(rel.empty()) << "cmake/DssInstall.cmake must compute the hop";
    EXPECT_TRUE(rel.is_relative())
        << "an absolute installed-config path makes the package non-relocatable: "
        << rel.generic_string();
    EXPECT_EQ(rel.filename().generic_string(), "dss-config")
        << "the hop must land on the config tree itself: " << rel.generic_string();
}

// The layout ROUND-TRIPS: compose through the published relative hop, and the
// resolver finds exactly that directory. Composing the expected path from
// `installedConfigRelDir()` rather than from a literal is what keeps this pin
// honest — a literal would keep passing after CMake changed the install layout,
// which is the drift the single-owner arrangement exists to prevent.
TEST(ConfigPathWalk, InstalledRootRoundTripsThroughTheComputedRelDir) {
    ScratchDir scratch(Location::Temp, "config-walk-installed");
    fs::path const exeDir = scratch.path() / "bin";
    std::error_code ec;
    fs::create_directories(exeDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::path const planted = plantInstalledLayout(exeDir, "sources");

    auto const got = dss::installedConfigRootFrom(exeDir);
    ASSERT_TRUE(got.has_value())
        << "a planted installed layout must resolve from its executable dir";
    expectSameDir(*got, planted);
}

// ── [[D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS]] ─────
//
// AN INSTALLED COMPILER ON A SHARE MUST STILL FIND ITS OWN CONFIG TREE, and this
// is the arm where the collapse costs BEHAVIOUR rather than wording.
//
// ★★★ THE DEFECT, ✔MEASURED 2026-08-28 with a standalone probe against the
// toolchain that builds DSS (g++ MinGW-W64 UCRT 13.2.0), on a REACHABLE UNC
// directory (`exists()` true), printed with `.string()` so no print-side
// transform can be blamed:
//     base                        '//localhost/C$/Source/DailySoftware'   run 2
//     join + lexically_normal()   '\localhost\C$\Source\dss-config'       run 1
// `installedConfigRootFrom` composed the hop and normalised it exactly that way.
// One separator gone demotes the AUTHORITY to an ordinary directory on the local
// drive root, `is_directory` then answers false for a tree that is right there,
// and this arm — which `findShippedConfig` calls AUTHORITATIVE once found —
// declines without a word and lets the cwd walk answer instead. Not a refusal
// naming a path: a DIFFERENT config tree, silently.
//
// ⚠ WHY THE ASSERTION IS ON THE SEPARATOR RUN AND NOT ONLY ON `has_value()`.
// Both wrong spellings (`C:\localhost\…` and `\localhost\…`) are equally absent
// from disk, so a partial repair reads exactly like a complete one. The run is
// the property that was destroyed, so the run is what gets pinned.
TEST(ConfigPathWalk, InstalledRootSurvivesAMultiSeparatorExecutableDir) {
    ScratchDir scratch(Location::Temp, "config-walk-installed-unc");
    fs::path const exeDir = scratch.path() / "bin";
    std::error_code ec;
    fs::create_directories(exeDir, ec);
    ASSERT_FALSE(ec) << ec.message();
    fs::path const planted = plantInstalledLayout(exeDir, "sources");

    fs::path const uncExeDir = dss::test_support::uncSpellingOf(exeDir);
    if (uncExeDir.empty())
        GTEST_SKIP()
            << "D-PATH-MULTI-SEPARATOR-ROOT-COLLAPSED-BY-STDLIB-PATH-TRANSFORMS"
               ": this host offers no reachable multi-separator spelling of '"
            << exeDir.string()
            << "', so the installed-layout arm WAS NOT MEASURED on this leg. "
               "This is an UNMEASURED property, not a passing one.";
    ASSERT_GE(dss::test_support::leadingSeparatorRun(uncExeDir), 2u);

    auto const got = dss::installedConfigRootFrom(uncExeDir);
    ASSERT_TRUE(got.has_value())
        << "an installed tree reached through an authority-rooted executable "
           "directory was not found — the leading separator run was collapsed, "
           "so the probe asked the LOCAL drive about a path that lives on "
        << uncExeDir.string();
    EXPECT_GE(dss::test_support::leadingSeparatorRun(*got), 2u)
        << "the resolved root no longer names the machine it was asked about: "
        << got->string();

    // ⚠ DELIBERATELY NOT `expectSameDir`, AND THE REASON IS THIS ROW'S OTHER
    // HALF. That helper compares through the THROWING `fs::weakly_canonical`,
    // and ✔MEASURED on this same host that overload does not merely mis-answer
    // for an authority-rooted path — it THROWS `filesystem error: cannot make
    // canonical path: No such file or directory` for a directory whose
    // `exists()` is true. (The non-throwing overload sets `ENOENT` instead;
    // that is the SAFE direction, which is why every call site with a
    // keep-the-original-on-error arm was already correct.) So the equality is
    // stated on what was actually planted: the subdirectory
    // `plantInstalledLayout` created must be reachable THROUGH the resolved
    // root, which no collapsed spelling can satisfy.
    std::error_code sameEc;
    EXPECT_TRUE(fs::is_directory(*got / "sources", sameEc))
        << "the resolved root does not hold the planted tree, so it is not the "
           "tree that was installed: " << got->string();
    EXPECT_EQ(got->filename(), planted.filename())
        << "the hop must still land on the config tree itself";
}

// A DEVELOPMENT build has no installed layout around its binary, so the arm must
// be inert rather than resolving something. This is what guarantees the change
// reorders nothing for the repo's own workflow: `build/bin/dss/` has no sibling
// data directory, so the cwd walk still answers exactly as it always did.
TEST(ConfigPathWalk, InstalledRootIsNulloptWhenNothingIsInstalled) {
    ScratchDir scratch(Location::Temp, "config-walk-installed");
    EXPECT_FALSE(dss::installedConfigRootFrom(scratch.path()).has_value());
}

// Validated as a DIRECTORY, matching the directory form's discipline: a plain
// FILE sitting where the tree should be must not be handed back as a config root
// that callers will then try to iterate.
TEST(ConfigPathWalk, InstalledRootRejectsAFileWhereTheTreeShouldBe) {
    ScratchDir scratch(Location::Temp, "config-walk-installed");
    fs::path const exeDir = scratch.path() / "bin";
    std::error_code ec;
    fs::create_directories(exeDir, ec);
    ASSERT_FALSE(ec) << ec.message();

    fs::path const root =
        (exeDir / fs::path{std::string{dss::installedConfigRelDir()}}).lexically_normal();
    fs::create_directories(root.parent_path(), ec);
    ASSERT_FALSE(ec) << ec.message();
    std::ofstream(root) << "not a directory\n";

    EXPECT_FALSE(dss::installedConfigRootFrom(exeDir).has_value())
        << "a FILE named dss-config is not an installed config tree";
}

// The host primitive answers about THIS process. Asserted by CONTENT, not by
// "some directory came back": the answer must be the directory that actually
// holds this test executable. An implementation that returned the cwd, or the
// directory of argv[0], or an empty path would satisfy a mere has_value() check
// and fail here.
TEST(ConfigPathWalk, RunningExecutableDirHoldsThisTestBinary) {
    auto const dir = dss::runningExecutableDir();
    ASSERT_TRUE(dir.has_value())
        << "this host must report the running executable's own path";
    std::error_code ec;
    ASSERT_TRUE(fs::is_directory(*dir, ec)) << dir->generic_string();

    bool found = false;
    for (fs::directory_iterator it{*dir, ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->path().filename().generic_string().rfind(
                "dss_core_test_config_path_walk", 0) == 0) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "runningExecutableDir() returned '" << dir->generic_string()
        << "', which does not contain this test's own binary — so it is not "
           "reporting this process's image directory";
}

// ── BINARY / CONFIG VERSION SKEW ───────────────────────────────────────────
//
// A compiler paired with a config tree from another version does not fail; it
// compiles something subtly different — the predefined-macro set, a target's
// register file, a shipped header's struct layout and an object format's
// relocation table all live in that tree. Same silent-wrong-answer class as a
// stale cached object, and invisible from inside one image.

// THE REFUSAL, and it must name BOTH halves: a message saying only "version
// mismatch" leaves the user unable to tell which half to fix.
TEST(ConfigPathWalk, VersionSkewedEnvRootIsRefusedNamingBothVersions) {
    ScratchDir scratch(Location::Temp, "config-walk-skew");
    plantTarget(scratch.path(), "synth_target");
    plantVersion(scratch.path(), "0.0.0-a-different-release");

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const res = findShippedConfig(targetLocator("synth_target"));

    ASSERT_FALSE(res.has_value())
        << "a config tree from another version must be REFUSED, not used";
    ASSERT_FALSE(res.error().empty());
    std::string const msg = res.error().front().message;
    EXPECT_TRUE(contains(msg, "0.0.0-a-different-release"))
        << "the diagnostic must name the TREE's version: " << msg;
    EXPECT_TRUE(contains(msg, compilerVersion()))
        << "the diagnostic must name the COMPILER's version: " << msg;
    EXPECT_TRUE(contains(msg, "dss-config"))
        << "the diagnostic must name the tree it refused: " << msg;
}

// SAME TREE, MATCHING VERSION → resolves. The control that proves the pin above
// reddens on the skew rather than on the presence of a VERSION file at all: both
// tests plant the identical tree and differ only in the version string.
TEST(ConfigPathWalk, VersionMatchedEnvRootResolves) {
    ScratchDir scratch(Location::Temp, "config-walk-skew");
    fs::path const file = plantTarget(scratch.path(), "synth_target");
    plantVersion(scratch.path(), compilerVersion());

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const res = findShippedConfig(targetLocator("synth_target"));
    ASSERT_TRUE(res.has_value())
        << "a tree declaring THIS compiler's version must resolve";
    EXPECT_EQ(fs::weakly_canonical(*res), fs::weakly_canonical(file));
}

// A tree that declares NO version is not a mismatch. Stated as a pin because it
// is a deliberate semantic and not an accident: the guard makes a positive claim
// only when it has BOTH facts, so an unversioned tree (a hand-assembled config
// root, the pragma-census scratch tree) keeps working instead of turning one
// missing file into a hard refusal.
TEST(ConfigPathWalk, UnversionedTreeIsNotTreatedAsASkew) {
    ScratchDir scratch(Location::Temp, "config-walk-skew");
    fs::path const file = plantTarget(scratch.path(), "synth_target");   // no VERSION

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    auto const res = findShippedConfig(targetLocator("synth_target"));
    ASSERT_TRUE(res.has_value()) << "an unversioned tree declares no disagreement";
    EXPECT_EQ(fs::weakly_canonical(*res), fs::weakly_canonical(file));
}

// THE CWD-WALK ARM SKEWS TOO. Pinned separately because the two arms reach the
// check by different routes, and a fix applied to only one of them would leave a
// build-tree binary standing in a different checkout compiling silently wrong.
TEST(ConfigPathWalk, VersionSkewedWalkedTreeIsRefused) {
    ScratchDir scratch(Location::Temp, "config-walk-skew");
    plantTarget(scratch.path(), "synth_target");
    plantVersion(scratch.path(), "0.0.0-a-different-release");

    ScopedEnv env("DSS_CONFIG_ROOT");                  // construct-to-clear
    ScopedCwd cwd(makeNestedStart(scratch.path()));    // 3 hops below the plant

    auto const res = findShippedConfig(targetLocator("synth_target"));
    ASSERT_FALSE(res.has_value())
        << "the cwd walk must refuse a tree from another version too";
    ASSERT_FALSE(res.error().empty());
    EXPECT_TRUE(contains(res.error().front().message, "0.0.0-a-different-release"));
}

// THE TWO FORMS AGREE. They share one implementation precisely so a file can
// never resolve out of one tree while a directory resolves out of another —
// mixed trees would be a skew nothing could observe from either side.
TEST(ConfigPathWalk, DirFormRefusesTheSameSkewedTree) {
    ScratchDir scratch(Location::Temp, "config-walk-skew");
    plantConfigDir(scratch.path(), "sources");
    plantVersion(scratch.path(), "0.0.0-a-different-release");

    ScopedEnv  env("DSS_CONFIG_ROOT", scratch.path().string());
    ScratchDir bare(Location::Temp, "config-walk-skew");
    ScopedCwd  cwd(makeNestedStart(bare.path()));      // nothing for the walk to find

    EXPECT_FALSE(findShippedConfigDir("sources").has_value())
        << "the directory form must not hand back a tree the file form refuses";
}

// ── THE NOT-FOUND DIAGNOSTIC NAMES WHERE IT LOOKED ─────────────────────────
//
// An installed binary that cannot find its config must SAY SO. Falling through
// to a bare "not found" and letting a confusing downstream error surface (a
// missing language, an unresolvable `<stdio.h>`) is the defect this arm closes,
// not an acceptable way to report it.
TEST(ConfigPathWalk, NotFoundDiagnosticListsEveryPathTried) {
    ScratchDir scratch(Location::Temp, "config-walk-tried");   // nothing planted
    ScratchDir bare(Location::Temp, "config-walk-tried");

    ScopedEnv env("DSS_CONFIG_ROOT", scratch.path().string());
    ScopedCwd cwd(makeNestedStart(bare.path()));

    auto const res = findShippedConfig(targetLocator("no_such_target_at_all"));
    ASSERT_FALSE(res.has_value());
    ASSERT_FALSE(res.error().empty());
    std::string const msg = res.error().front().message;

    EXPECT_TRUE(contains(msg, "tried:")) << msg;
    EXPECT_TRUE(contains(msg, "$DSS_CONFIG_ROOT"))
        << "the override it consulted must appear among the tried paths: " << msg;
    EXPECT_TRUE(contains(msg, "installed layout"))
        << "the installed-layout probe must appear among the tried paths — that is "
           "the arm a packaged user's report will be about: " << msg;
    EXPECT_TRUE(contains(msg, "cwd walk"))
        << "the walk must appear among the tried paths: " << msg;
    EXPECT_TRUE(contains(msg, "no_such_target_at_all"))
        << "the message must name what was being looked for: " << msg;
}
