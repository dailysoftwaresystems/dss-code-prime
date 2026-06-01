#pragma once

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// Per-test scratch directory — RAII helper hoisted at D-LK10-6 closure
// (LK10 cycle 3 sibling-cycle, 2026-06-01) from two prior copies in
// `tests/link/test_link_writer.cpp` and `tests/program/test_compile_pipeline.cpp`.
//
// The two prior copies diverged MEANINGFULLY on *where* the scratch
// dir lives (the architect anchor noted this is the policy axis that
// triggers the hoist):
//
//   * `Location::Temp` — `temp_directory_path()` base; correct for tests
//     that don't care about cwd-rooted file resolution.
//   * `Location::InsideRepo` — `<cwd>/test-scratch/<group>/...` inside
//     the repo tree; REQUIRED for tests that exercise schema-loader
//     code paths (`findShippedConfig` walks UP from cwd looking for
//     `src/dss-config/`), because a temp-rooted scratch breaks the walk.
//
// PID-seed + atomic counter guarantee unique paths across parallel
// `ctest -j` runs (different processes against the same base dir) AND
// sequential runs (stack-address reuse from `reinterpret_cast<uintptr_t>(this)`
// would have collided with reclaimed addresses).
//
// `useAsCwd()` is the second policy axis: tests that drive
// `compileFiles` (which computes output paths cwd-rooted) need cwd
// to be inside the scratch dir for the entire test body. The dtor
// restores cwd to `originalCwd_` before `remove_all` — some platforms
// refuse to remove a dir that's the current process's cwd.

namespace dss::test_support {

enum class Location : std::uint8_t {
    Temp        = 0,  // `temp_directory_path()` — fast, isolated from repo
    InsideRepo  = 1,  // `<cwd>/test-scratch/<group>/...` — for cwd-walk tests
};

class ScratchDir {
public:
    // `group` becomes a subdir under the base — e.g. "link-writer",
    // "program", "asm". Lets parallel test binaries share the base
    // without colliding on the unique path slot.
    explicit ScratchDir(Location loc, std::string_view group) {
        static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
        auto const pid = static_cast<std::uint64_t>(_getpid());
#else
        auto const pid = static_cast<std::uint64_t>(getpid());
#endif
        std::error_code ec;
        originalCwd_ = std::filesystem::current_path(ec);

        std::filesystem::path base;
        switch (loc) {
            case Location::Temp:
                base = std::filesystem::temp_directory_path()
                     / "dss-test-scratch"
                     / std::string{group};
                break;
            case Location::InsideRepo:
                base = originalCwd_ / "test-scratch"
                     / std::string{group};
                break;
        }
        std::filesystem::create_directories(base, ec);
        path_ = base / (std::to_string(pid) + "-"
                        + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path_, ec);
    }

    ~ScratchDir() {
        std::error_code ec;
        // Restore cwd before removing the scratch dir; some platforms
        // refuse to remove a dir that's the current process cwd.
        std::filesystem::current_path(originalCwd_, ec);
        std::filesystem::remove_all(path_, ec);
    }

    ScratchDir(ScratchDir const&)            = delete;
    ScratchDir& operator=(ScratchDir const&) = delete;
    ScratchDir(ScratchDir&&)                 = delete;
    ScratchDir& operator=(ScratchDir&&)      = delete;

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return path_; }
    [[nodiscard]] std::filesystem::path const& originalCwd() const noexcept { return originalCwd_; }

    // Pin cwd to this scratch dir for the test body. Output paths
    // computed by `compileFiles` (cwd-rooted) land under here.
    // Safe ONLY with `Location::InsideRepo` — `Location::Temp` puts
    // the scratch outside the repo, and the schema-loader's cwd-walk
    // (`findShippedConfig`) would no longer reach `src/dss-config/`.
    //
    // Throws `std::runtime_error` on `ec` rather than swallow — a
    // failed cwd-change would silently misroute artifacts to
    // `<originalCwd>/target/...` (the build directory), then a
    // downstream "output dir exists" check would fail with a confusing
    // diagnostic instead of naming the actual root cause. (silent-
    // failure F17 fold, LK10 cycle 2 — preserved here.) gtest converts
    // the uncaught throw into a test failure with the message intact.
    void useAsCwd() {
        std::error_code ec;
        std::filesystem::current_path(path_, ec);
        if (ec) {
            throw std::runtime_error(
                "ScratchDir::useAsCwd: cwd change to '"
                + path_.generic_string() + "' failed: " + ec.message());
        }
    }

private:
    std::filesystem::path path_;
    std::filesystem::path originalCwd_;
};

} // namespace dss::test_support
