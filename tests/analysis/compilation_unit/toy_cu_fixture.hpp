#pragma once

// Shared toy-CompilationUnit helpers for the CU3 test files
// (test_unit_attribute.cpp + adjacent CU3-era suites). Kept here
// rather than copied per-file so the suites can't drift.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace dss::cu_test {

[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadShippedSchema(std::string_view name) {
    auto loaded = GrammarSchema::loadShipped(name);
    if (!loaded) {
        ADD_FAILURE() << "loadShipped(\"" << name << "\") failed";
        std::abort();
    }
    return *loaded;
}

[[nodiscard]] inline std::shared_ptr<GrammarSchema const> loadToySchema() {
    return loadShippedSchema("toy");
}

// True if any diagnostic in `reporter` carries `code`.
[[nodiscard]] inline bool hasCode(DiagnosticReporter const& reporter, DiagnosticCode code) {
    auto all = reporter.all();
    return std::any_of(all.begin(), all.end(),
                       [code](ParseDiagnostic const& d) { return d.code == code; });
}

[[nodiscard]] inline std::size_t countCode(DiagnosticReporter const& reporter, DiagnosticCode code) {
    auto all = reporter.all();
    return static_cast<std::size_t>(
        std::count_if(all.begin(), all.end(),
                      [code](ParseDiagnostic const& d) { return d.code == code; }));
}

// ── Scratch source directory (shared by the CU suites) ──────────────────────
//
// RAII scratch directory for the suites whose fixture files must all live in
// ONE directory (same-directory `#include` resolution has to find them). Its
// ONLY addition over the shared `dss::test_support::ScratchDir` is `write()`,
// a convenience that carries no invariant of its own — which is exactly why
// it is here and not copied per suite: two identical copies of a facade can
// silently drift, and this one had nothing suite-specific in it.
//
// `Group` is the ScratchDir group (the subdirectory a suite claims its unique
// slot under). It is a TEMPLATE parameter rather than a ctor argument so each
// suite can alias it once — `using TempDir = ScratchSourceDir<kScratchGroup>;`
// — and leave every construction site a bare `TempDir dir;`. The suites
// deliberately pass DIFFERENT groups so their scratch trees stay separate.
//
// The unique-path scheme is NOT reimplemented here — it is the shared one in
// `tests/test_support/scratch_dir.hpp`. It used to be reimplemented per file,
// and that is the defect D-TEST-FIXED-SCRATCH-PATH-POPULATION names: the path
// came from an atomic counter with NO per-process seed, so the Nth TempDir of
// EVERY process of a binary resolved to the same fixed path (`<temp>/dss_cu4_<n>`
// in the import-resolver suite, `<temp>/dss_fc2_oracle_<n>` in the type-name-
// oracle one) and the dtor's `remove_all` deleted a concurrent sibling
// process's fixture files mid-test. MEASURED before the change, two concurrent
// processes of the SAME binary, `--gtest_repeat=12 --gtest_shuffle`: 6 of 6
// rounds red in BOTH suites — 1..18 failed tests per round (import resolver),
// 96..136 failure lines per round (type-name oracle); the count DIVERGING run
// to run is the contention tell — plus SIGSEGV wherever an `EXPECT`ed-empty
// `trees()` was then indexed (those count checks are `ASSERT_EQ` now, so the
// same contention would report a legible failure instead of crashing). The
// visible symptom is `cu.trees().size() == 0`, NOT a `filesystem_error`:
// libc++ maps the vanished entry's ENOENT to `file_type::not_found`, so the
// file is simply gone and nothing throws.
//
// `ScratchDir` claims its slot with SINGULAR `create_directory` in a loop —
// that check-and-create is atomic at the OS level, so it is race-free and it
// steps over a stale directory instead of sharing it. A PID alone would be a
// seed, not a guarantee: PIDs recycle and killed runs leave directories behind.
template <char const* Group>
class ScratchSourceDir {
public:
    ScratchSourceDir() : dir_{dss::test_support::Location::Temp, Group} {}

    ScratchSourceDir(ScratchSourceDir const&)            = delete;
    ScratchSourceDir& operator=(ScratchSourceDir const&) = delete;

    // Write `content` to `<scratch>/name`; returns the file's path.
    std::filesystem::path write(std::string const& name, std::string const& content) const {
        auto path = dir_.path() / name;
        std::ofstream(path, std::ios::binary) << content;
        return path;
    }

    [[nodiscard]] std::filesystem::path const& path() const noexcept { return dir_.path(); }

private:
    dss::test_support::ScratchDir dir_;
};

// Build a CompilationUnit from one in-memory toy source per entry. Each source
// must be a clean toy program so the resulting Tree has a valid root.
[[nodiscard]] inline CompilationUnit makeToyUnit(std::initializer_list<std::string> sources) {
    UnitBuilder builder{loadToySchema()};
    unsigned index = 0;
    for (auto const& source : sources) {
        builder.addInMemory(source, "<mem" + std::to_string(index++) + ">");
    }
    return std::move(builder).finish();
}

} // namespace dss::cu_test
