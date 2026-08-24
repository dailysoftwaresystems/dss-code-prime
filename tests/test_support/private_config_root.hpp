#pragma once

#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// D-TEST-SHIPPED-CONFIG-READ-FROM-A-TREE-ANOTHER-PROCESS-IS-WRITING
//
// ★★★ WHAT THIS EXISTS TO STOP. A test that reads shipped config reads the LIVE
// WORKING TREE — `dss_add_test` points `$DSS_CONFIG_ROOT` at `${CMAKE_SOURCE_DIR}`
// so discovery works out of tree, and every `loadShipped()` in the process then
// re-opens `<repo>/src/dss-config/...` from disk. In a tree that only ever gets
// read that is fine. In THIS tree it is not: several workstreams share one
// checkout, and this project's own red-on-disable harnesses REWRITE a shipped
// document IN PLACE (`open(path, "wb")` — truncate, then write) while a gate is
// running in the next window. A reader that opens the file inside that window
// gets a SHORT file, the load fails, and the test reds on whatever assertion
// happened to be executing. Re-run it and it passes, because the window is gone.
//
// ★★ THE FAILURE IS UNATTRIBUTABLE, WHICH IS THE EXPENSIVE PART. Nothing in the
// red says "the config changed under me". ✔MEASURED 2026-08-24 (P31):
// `ffi/test_c_header_parser` reds with `GrammarLoadFailed` spread over an
// arbitrary SUBSET of its cases, and the same binary re-run immediately is green
// — which is exactly how it presented in the gate that found it, and it cost a
// full lane to re-derive because ctest had already overwritten the only copy of
// the output.
//
// ★ SO THE CONTENDED RESOURCE IS MADE PER-TEST, which is the fix rather than a
// tolerance: no retry, no serialization, no widened assertion. This copies the
// shipped tree ONCE per process into a scratch root nothing else can name, and
// repoints `$DSS_CONFIG_ROOT` at the copy for the rest of the run. A suite that
// installs it samples the mutable tree ONCE (the copy) instead of once per
// `loadShipped()` call — ✔MEASURED for `ffi/test_c_header_parser`: 25 opens of
// `c.lang.json` per run before, 1 after, and a writer-denial probe put the
// handle open on that file for ~67% of the suite's wall time before the change.
//
// ★★ AND A CONFIG-LEVEL RED-ON-DISABLE STILL WORKS, which is the property that
// vetoes the tempting alternative. Copying at BUILD time would isolate the gate
// perfectly and would silently GREEN every config mutant, because this project's
// convention mutates a `.json` and re-runs ctest WITHOUT rebuilding. The copy is
// therefore taken at RUN time, from the tree as it stands when the process
// starts: a mutant planted before the run is copied along with everything else
// and still reds exactly as it did.
//
// ⓘ WHAT IT DOES NOT CLAIM. The copy is itself a read, so a writer that tears
// the tree DURING the copy is still observable — the residual is one window
// instead of N. `verifySizes` below catches the SHORT-FILE shape that the
// in-place rewrite produces and refuses by name; it is a size check, not a proof
// of byte-stability, and it is documented as the former. The only construction
// that closes the window entirely is writers that replace atomically
// (`os.replace`/`rename`), and ✔MEASURED on this host that shape cannot tear a
// reader at all: Windows refuses the RENAME with ERROR_ACCESS_DENIED while a
// reader holds the file, because `ifstream` opens without `FILE_SHARE_DELETE`.

namespace dss::test_support {

// A per-run private copy of `<repo>/src/dss-config` (plus the sibling `VERSION`
// the version-skew check reads), with `$DSS_CONFIG_ROOT` pointed at it for this
// object's lifetime. Construct ONCE per process — see
// `installPrivateConfigRoot` below, which is the intended entry point.
class PrivateConfigRoot {
public:
    explicit PrivateConfigRoot(std::string_view group)
        : scratch_(Location::Temp, group) {
        namespace fs = std::filesystem;
        std::error_code ec;

        fs::path const src = dss::test::configRoot();
        fs::path const dst = scratch_.path() / "src" / "dss-config";
        fs::create_directories(dst.parent_path(), ec);
        if (ec) {
            ADD_FAILURE() << "PrivateConfigRoot: could not create "
                          << dst.parent_path().string() << ": " << ec.message();
            return;
        }
        fs::copy(src, dst, fs::copy_options::recursive, ec);
        if (ec) {
            ADD_FAILURE() << "PrivateConfigRoot: could not copy the shipped "
                             "config tree " << src.string() << " -> "
                          << dst.string() << ": " << ec.message()
                          << " — a tree being rewritten while it is copied is "
                             "the condition this helper exists to name";
            return;
        }

        // `VERSION` sits BESIDE `src/`, and `findShippedConfig` reads it to
        // refuse a binary/config version skew. A root without it would take the
        // not-found arm, so the copy is not optional.
        fs::copy_file(dss::test::repoRoot() / "VERSION",
                      scratch_.path() / "VERSION",
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            ADD_FAILURE() << "PrivateConfigRoot: could not copy VERSION: "
                          << ec.message();
            return;
        }

        if (!verifySizes(src, dst)) return;

        env_ = std::make_unique<ScopedEnv>("DSS_CONFIG_ROOT",
                                           scratch_.path().string());

        // The precedent's clause 4, and it is not ceremony: an override that
        // MISSES falls THROUGH to the cwd walk, i.e. straight back to the live
        // tree, and every claim above would be false while every test still
        // passed. `dss::test::configRoot()` shares the compiler's env-first
        // precedence, so proving it here proves it for `findShippedConfig`.
        auto const seen = dss::test::configRoot();
        if (seen != dst) {
            ADD_FAILURE() << "PrivateConfigRoot: this process still reads "
                          << seen.string() << ", not the private copy at "
                          << dst.string()
                          << " — the isolation is not in effect";
            return;
        }
        installed_ = true;
    }

    [[nodiscard]] bool installed() const noexcept { return installed_; }
    [[nodiscard]] std::filesystem::path const& root() const noexcept {
        return scratch_.path();
    }

    // The env override must be dropped BEFORE the tree it names is removed, or a
    // later reader would follow `$DSS_CONFIG_ROOT` to a directory that no longer
    // exists and take the fall-through arm without saying so.
    ~PrivateConfigRoot() { env_.reset(); }

    PrivateConfigRoot(PrivateConfigRoot const&)            = delete;
    PrivateConfigRoot& operator=(PrivateConfigRoot const&) = delete;

private:
    // Every regular file in the copy must have its source's size. This catches
    // the SHORT-FILE shape an in-place `open(path,"wb")` rewrite produces — the
    // shape the repo's own mutant harnesses use — and it refuses by NAME instead
    // of letting a truncated document surface later as a parse error on an
    // unrelated assertion. It is a size check: it does not prove the bytes were
    // stable, and it is not offered as proof of that.
    static bool verifySizes(std::filesystem::path const& src,
                            std::filesystem::path const& dst) {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (auto const& e : fs::recursive_directory_iterator(src, ec)) {
            if (ec) break;
            if (!e.is_regular_file()) continue;
            fs::path const rel  = fs::relative(e.path(), src, ec);
            if (ec) break;
            auto const srcSize = fs::file_size(e.path(), ec);
            if (ec) break;
            auto const dstSize = fs::file_size(dst / rel, ec);
            if (ec) break;
            if (srcSize != dstSize) {
                ADD_FAILURE()
                    << "PrivateConfigRoot: " << rel.generic_string()
                    << " copied as " << dstSize << " bytes but its source is "
                    << srcSize
                    << " — the shipped config tree was being REWRITTEN while "
                       "this suite copied it. This run's verdict is about that, "
                       "not about the code under test; nothing here is a "
                       "statement about the subject.";
                return false;
            }
        }
        if (ec) {
            ADD_FAILURE() << "PrivateConfigRoot: could not verify the copy: "
                          << ec.message();
            return false;
        }
        return true;
    }

    ScratchDir                 scratch_;
    std::unique_ptr<ScopedEnv> env_;
    bool                       installed_ = false;
};

// Install a private config root for the WHOLE binary, once, before any test
// runs. GoogleTest owns the returned environment; call this at namespace scope
// in a suite that reads shipped config:
//
//     namespace { auto const* kCfg = dss::test_support::installPrivateConfigRoot("ffi-c-header"); }
//
// A gtest Environment (not a static ctor) because the copy can FAIL and a
// failure has to be reported as a failure rather than thrown before `main`.
class PrivateConfigRootEnvironment : public ::testing::Environment {
public:
    explicit PrivateConfigRootEnvironment(std::string group)
        : group_(std::move(group)) {}

    void SetUp() override { root_ = std::make_unique<PrivateConfigRoot>(group_); }
    void TearDown() override { root_.reset(); }

private:
    std::string                        group_;
    std::unique_ptr<PrivateConfigRoot> root_;
};

inline ::testing::Environment* installPrivateConfigRoot(std::string group) {
    return ::testing::AddGlobalTestEnvironment(
        new PrivateConfigRootEnvironment(std::move(group)));
}

} // namespace dss::test_support
