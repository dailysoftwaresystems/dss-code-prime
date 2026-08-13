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
#include "scoped_env.hpp"    // the ONE env override (this file used to carry a copy)
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
