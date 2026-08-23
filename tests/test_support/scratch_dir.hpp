#pragma once

#include "core/substrate/path_identity.hpp"

#include <atomic>
#include <cstdio>
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
// UNIQUENESS (`D-TEST-EXAMPLES-RUNNER-PARALLEL-CONTENTION-FLAKE`, corrected
// TF-C58 2026-07-24). A PID seed plus an atomic counter is NOT by itself a
// guarantee, and this docblock used to claim it was. The original code built
// the path from PID+counter and called `create_directories`, which reports
// SUCCESS when the directory ALREADY EXISTS. Windows recycles PIDs freely, and
// the dtor's `remove_all` can fail (a spawned example binary still holding a
// handle, antivirus, a killed run), so stale directories accumulate under the
// base — 55 of them were sitting in `test-scratch/examples/` when this was
// found. A new process that drew a recycled PID and the same counter value
// therefore SILENTLY SHARED a stale directory, and the next `copy_file` failed
// with "File exists" — surfacing as a non-deterministic ctest red on a
// DIFFERENT example each run, in parallel AND serial runs alike.
//
// The claim is now true by CONSTRUCTION: the ctor loops on `create_directory`
// (singular) and accepts only a slot it actually created. That check-and-create
// is atomic in the OS, so it is race-free across concurrent `ctest -j`
// processes and it steps over stale directories rather than reusing them.
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

// THE SAME resolution the product applies to any path it keys on — by CALLING
// it, not by mirroring it.
//
// ⚠ THIS WAS A DELIBERATE COPY UNTIL 2026-08-18, and the reasons it stopped being
// one are worth keeping. The copy existed so a test would not link the RESOLVER
// merely to build a directory path, and so a product-side change could not
// silently retune every fixture in the tree. The first no longer applies: the
// canonicalizer is now a leaf host-substrate function
// (`core/substrate/path_identity`), beside `process_spawn`, not the resolver.
// The second INVERTS on inspection — a fixture that resolves paths DIFFERENTLY
// from the product is exactly the 2026-08-17 CI failure (fixture spelled the
// 8.3 short form, product reported the long one, `find()` returned npos), so
// agreeing BY CONSTRUCTION is the property actually wanted here.
//
// The pin that guarded the copy is replaced, not dropped, by
// `FixtureHasNotGrownItsOwnCanonicalizer` — see `test_scratch_dir.cpp` for why
// "the two agree" became a test that passes both ways.
[[nodiscard]] inline std::filesystem::path
canonicalizeLikeTheProduct(std::filesystem::path const& p) {
    // `make_preferred` for the same reason `PathIdentity::path()` does it:
    // this value becomes a DIRECTORY that tests hand to `cmd.exe`, and a
    // forward-slashed Windows path is fine for every API except the shell.
    return std::filesystem::path{dss::core::canonicalIdentityKey(p)}
        .make_preferred();
}

class ScratchDir {
public:
    // `group` becomes a subdir under the base — e.g. "link-writer",
    // "program", "asm". Lets parallel test binaries share the base
    // without colliding on the unique path slot.
    //
    // Throws `std::runtime_error` on ANY filesystem error (parent
    // permission denied, disk full, antivirus lock). Silent swallow
    // would leave `path_` set to a path that doesn't exist on disk;
    // tests writing into it produce confusing ENOENT downstream
    // rather than a clear "scratch dir setup failed" signal. (silent-
    // failure audit post-fold #1 — same anti-pattern that F17 closed
    // for `useAsCwd`; ctor was missed in the original hoist.)
    explicit ScratchDir(Location loc, std::string_view group)
        : loc_(loc) {
        static std::atomic<std::uint64_t> counter{0};
#ifdef _WIN32
        auto const pid = static_cast<std::uint64_t>(_getpid());
#else
        auto const pid = static_cast<std::uint64_t>(getpid());
#endif
        std::error_code ec;
        originalCwd_ = std::filesystem::current_path(ec);
        if (ec) {
            throw std::runtime_error(
                "ScratchDir ctor: current_path() failed: " + ec.message());
        }

        originalCwd_ = canonicalizeLikeTheProduct(originalCwd_);

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
        // ★★ CANONICALIZE THE BASE — the ONE site, deliberately, because the
        // alternative is 53 test files each remembering to do it. ✔MEASURED
        // 2026-08-17: `windows-msvc-release` CI failed 2 tests / 5 cases while
        // ALL FOUR local legs were green, and the cause was here.
        //
        // WHY. `temp_directory_path()` returns whatever `TMP`/`TEMP` holds, and
        // GitHub's Windows runner holds the 8.3 SHORT form —
        // `C:/Users/RUNNER~1/...` for user `runneradmin`. The product resolves a
        // dependency path with `fs::weakly_canonical`
        // (`src/program/dependency_resolver.cpp` `canonicalize`), which rewrites
        // that to the LONG form `C:/Users/runneradmin/...`. So a diagnostic
        // NAMED the manifest correctly while `message.find(fixturePath)` returned
        // npos — two spellings of one directory, compared as strings.
        //
        // ⚠ THE LOCAL GATE STRUCTURALLY CANNOT SEE THIS, which is why it reached
        // CI. 8.3 shortening only kicks in for a directory component longer than
        // 8 characters; the local operator's user directory is `rafae` (5), so
        // short form == long form and every assertion here is trivially
        // satisfied. It is not a flaky test or a Release-vs-Debug difference —
        // it is a test that could only ever fail on a machine whose username is
        // long. ⇒ reproduced locally by pointing `TMP`/`TEMP` at an 8.3 spelling,
        // and that reproduction is now a permanent pin in `test_scratch_dir.cpp`
        // (`ShortPathSpellingIsDefeatedAtTheChokepoint`) rather than a one-off.
        //
        // Same function as the product's, fallback included: a degraded key beats
        // an exception out of a test fixture's constructor.
        base = canonicalizeLikeTheProduct(base);
        std::filesystem::create_directories(base, ec);
        if (ec) {
            throw std::runtime_error(
                "ScratchDir ctor: create_directories('"
                + base.generic_string() + "') failed: " + ec.message());
        }
        // Claim a unique directory ATOMICALLY — see the uniqueness note in the
        // class docblock for why `create_directories` cannot do this job.
        // `create_directory` (SINGULAR) returns true only when THIS call
        // created the directory; that check-and-create is atomic at the OS
        // level, so it is race-free against concurrent `ctest -j` processes
        // AND it detects a stale directory left by an earlier run instead of
        // silently sharing it.
        for (std::uint32_t attempt = 0;; ++attempt) {
            if (attempt > 10000u) {
                throw std::runtime_error(
                    "ScratchDir ctor: could not claim a unique scratch dir "
                    "under '" + base.generic_string() + "' after 10000 "
                    "attempts. Stale directories are accumulating — an "
                    "earlier run's destructor could not remove them (see the "
                    "remove_all warning on stderr). Delete the directory and "
                    "re-run.");
            }
            auto candidate = base / (std::to_string(pid) + "-"
                                     + std::to_string(counter.fetch_add(1)));
            std::error_code cec;
            if (std::filesystem::create_directory(candidate, cec)) {
                path_ = std::move(candidate);
                break;
            }
            if (cec) {
                throw std::runtime_error(
                    "ScratchDir ctor: create_directory('"
                    + candidate.generic_string() + "') failed: "
                    + cec.message());
            }
            // Otherwise the path already exists (a stale dir from a killed
            // run, or a live sibling that won the race) — take the next slot.
        }
    }

    ~ScratchDir() {
        std::error_code ec;
        // Restore cwd before removing the scratch dir; some platforms
        // refuse to remove a dir that's the current process cwd.
        std::filesystem::current_path(originalCwd_, ec);
        ec.clear();
        std::filesystem::remove_all(path_, ec);
        // A dtor must not throw, but it must not hide this either: every
        // silent failure here leaves a directory behind forever, and it was
        // exactly that accumulation which made the (now-fixed) path-collision
        // bug fire. Warn so the leak is visible while it is still small.
        if (ec) {
            std::fprintf(stderr,
                "ScratchDir: WARNING — could not remove scratch dir '%s': %s. "
                "Stale scratch directories accumulate under the base dir; "
                "delete them if the count grows.\n",
                path_.generic_string().c_str(), ec.message().c_str());
        }
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
        // `Location::Temp` puts the scratch dir outside the repo tree;
        // calling `useAsCwd` would silently misroute schema-loader
        // walks (`findShippedConfig` walks UP from cwd looking for
        // `src/dss-config/`). Reject loudly rather than produce a
        // confusing "shipped config not found" downstream.
        // (architect audit Q6 post-fold #1 — was a precondition the
        // type didn't enforce.)
        if (loc_ == Location::Temp) {
            throw std::runtime_error(
                "ScratchDir::useAsCwd: Location::Temp is outside the "
                "repo tree; schema-loader cwd-walk would fail to find "
                "src/dss-config/. Construct with Location::InsideRepo "
                "for tests that drive `compileFiles` or anything else "
                "that loads shipped configs.");
        }
        std::error_code ec;
        std::filesystem::current_path(path_, ec);
        if (ec) {
            throw std::runtime_error(
                "ScratchDir::useAsCwd: cwd change to '"
                + path_.generic_string() + "' failed: " + ec.message());
        }
    }

private:
    Location              loc_;
    std::filesystem::path path_;
    std::filesystem::path originalCwd_;
};

} // namespace dss::test_support
