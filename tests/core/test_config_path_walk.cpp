// Tests for the shipped-config locator (`findShippedConfig`, config_path_walk).
// Focus: the `DSS_CONFIG_ROOT` env override that makes discovery independent of
// the launch cwd — the out-of-tree/CI build fix. The historical cwd-walk itself
// is exercised transitively by every loadShipped-using test in the suite.

#include "core/types/config_path_walk.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

using dss::DiagnosticCode;
using dss::findShippedConfig;
using dss::ShippedConfigLocator;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// Portable RAII env override — restores the prior value (or clears) on exit so
// the CMake-set DSS_CONFIG_ROOT (dss_add_test) is untouched after each test.
class ScopedEnv {
public:
    ScopedEnv(char const* name, std::string const& value) : name_(name) {
        if (char const* prev = std::getenv(name)) { had_ = true; prev_ = prev; }
        set(value);
    }
    ~ScopedEnv() { had_ ? set(prev_) : clear(); }
    ScopedEnv(ScopedEnv const&) = delete;
    ScopedEnv& operator=(ScopedEnv const&) = delete;

private:
    void set(std::string const& v) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), v.c_str());
#else
        ::setenv(name_.c_str(), v.c_str(), 1);
#endif
    }
    void clear() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }
    std::string name_;
    bool        had_ = false;
    std::string prev_;
};

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
