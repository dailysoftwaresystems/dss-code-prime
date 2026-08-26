// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE RUNTIME OBJECT CACHE'S
// ctest FIXTURE. It asserts almost nothing and that is the point: its product
// is a SIDE EFFECT on the machine, and it exists so that the suite's EXACT
// pipeline-shape instruments measure one build instead of two.
//
// ═══ WHY A FIXTURE IS THE RIGHT ANSWER AND A RELAXED ASSERTION IS NOT ════════
//
// Several tests read `PhaseTimers::read(<phase>).runs` and compare it for
// EQUALITY — `program/test_driver_argument_supply` is the one that made this
// necessary, with *"a 1-CU exec build must run Optimize TWICE"*. That instrument
// is exact on purpose: delete the driver's program-stage `optimizeModule` call
// and the count drops by exactly one, whatever either schedule contains.
//
// `PhaseTimers` is PROCESS-GLOBAL. So the instrument's real precondition is
// *no OTHER build ran in this process*, and it held for free until the shipped
// runtime stopped being compiled inside the user's build. Now a cache MISS runs
// a nested `Program` build to materialise the runtime archive, and that build
// legitimately opens phase scopes of its own: ✔MEASURED, a cold-cache pe64
// `--config=release` exec build reads `Optimize.runs` = 6 (the sole CU's two
// plus two per runtime unit) and a warm one reads 2.
//
// ⛔ THE TEMPTING FIX — `EXPECT_GE(runs, 2u)` — IS STRICTLY WORSE, and the
// arithmetic says so rather than taste. On a cold cache the honest count is 6;
// with the program-stage call DELETED it would be 5, and `GE(2)` passes both.
// So relaxing the comparison does not make the instrument robust to the extra
// build, it makes it BLIND to the defect it exists for exactly when the extra
// build happens. The precondition is what broke, so the precondition is what is
// repaired here.
//
// ⓘ This is the ctest expression of the packaged-`dist/` story
// `runtime_object_cache.hpp` describes: an INSTALLED compiler ships its runtime
// archives read-only under `<configRoot>/runtime/platform/dist/` and never
// misses at all. A source checkout has no packaged `dist/`, so the first build
// on each machine materialises them — and this fixture is where that first
// build happens, instead of inside whichever test ctest happened to schedule
// first.
//
// ═══ WHAT IT DOES, AND WHY NOTHING HERE IS A LITERAL ═════════════════════════
//
// It compiles ONE trivial translation unit for every (target, object format)
// pair whose FORMAT KIND realizes at least one shipped runtime source, at every
// build configuration — because the cache key carries the target spec, the
// build format, the archive sibling AND the config, so each of those axes is a
// separate entry that a later test could be the first to miss.
//
//   * the realizing format KEYS come from the descriptors: every name in the
//     closed `ObjectFormatKind` vocabulary is offered to
//     `allShippedSourcesForFormat`, and a key is "realizing" exactly when the
//     corpus answers with a source. No format literal appears below.
//   * the PAIRS come from the shipped `object-formats/` + `targets/` trees,
//     joined by `crossValidateTargetFormat` — the production pairing.
//   * the CONFIGS come from the closed `CompileConfig` vocabulary.
//   * STATIC-ARCHIVE formats are skipped, because an archive OUTPUT does not
//     link the runtime at all (see the `container` note at the runtime-archive
//     call site in `program.cpp`) — warming a key nothing reads would be a
//     fixture asserting its own busywork.
//
// ⚠ IT DOES NOT REQUIRE EVERY COMPILE TO SUCCEED, and that restraint is
// deliberate rather than lax: whether a given format can host a bare `main` is
// `program/test_shipped_runtime_compiles`'s and the corpus examples' business,
// not this fixture's — and the runtime archives are resolved and STORED before
// the link is even attempted, so a pair that cannot produce an image still
// warms the cache it was run for. What it DOES refuse is vacuity: zero realizing
// keys, zero pairs, or zero attempts would all make this fixture a no-op that
// reports success, which is precisely the shape a fixture rots into.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/program.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

// Every shipped schema NAME in `dir`, sorted for a deterministic failure
// message (`directory_iterator` is sorted on NTFS and hash-ordered on ext4).
[[nodiscard]] std::vector<std::string> shippedSchemaNames(fs::path const&  dir,
                                                          std::string_view suffix) {
    std::vector<std::string> names;
    std::error_code          ec;
    for (fs::directory_iterator it{dir, ec}, end; it != end; it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        std::string const leaf = it->path().filename().generic_string();
        if (!leaf.ends_with(suffix)) continue;
        names.push_back(leaf.substr(0, leaf.size() - suffix.size()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Every build configuration the cache keys on. The closed enum IS the
// vocabulary — a hand-kept list would go stale the day a third lands, and the
// stale entry would be the one a later test misses on.
constexpr CompileConfig kConfigs[] = {CompileConfig::Debug, CompileConfig::Release};

}  // namespace

TEST(RuntimeObjectCacheWarm, MaterializeEveryRealizedRuntimeArchive) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());

    // ── Which object-format KINDS realize a shipped runtime source ──────────
    // ⓘ The CLOSED vocabulary is the table, walked whole; the `unknown`
    // sentinel is excluded through the shared predicate rather than by name,
    // exactly as `program/test_shipped_runtime_compiles` does it — a new
    // enumerator therefore joins this fixture by existing.
    std::vector<std::string> realizingKeys;
    for (auto const& row : kObjectFormatKindTable.rows) {
        if (!isSelectableObjectFormatKind(row.first)) continue;
        std::string const key{row.second};
        if (!dss::ffi::allShippedSourcesForFormat(root / "shippedLibs", key).empty())
            realizingKeys.push_back(key);
    }
    ASSERT_FALSE(realizingKeys.empty())
        << "NO object-format kind realizes a shipped runtime source, so this "
           "fixture would warm nothing while reporting success. Either the "
           "corpus declares no 'realization' at all — in which case the runtime "
           "object cache has no subject — or the discovery is broken.";

    ScratchDir  scratch{Location::Temp, "runtime-cache-warm"};
    fs::path const src = scratch.path() / "warm.c";
    {
        std::ofstream out{src, std::ios::binary};
        ASSERT_TRUE(out.good()) << "cannot write " << src.generic_string();
        out << "int main(void) { return 0; }\n";
    }

    auto const targetNames = shippedSchemaNames(root / "targets", ".target.json");
    ASSERT_FALSE(targetNames.empty())
        << "no shipped targets — nothing to compile FOR";

    std::size_t pairs    = 0;
    std::size_t attempts = 0;
    std::size_t built    = 0;
    std::size_t cell     = 0;

    for (auto const& formatName :
         shippedSchemaNames(root / "object-formats", ".format.json")) {
        auto const format = ObjectFormatSchema::loadShipped(formatName);
        if (!format.has_value()) continue;   // health is the loader's business
        std::string const kindKey{objectFormatKindName((*format)->kind())};
        if (std::find(realizingKeys.begin(), realizingKeys.end(), kindKey)
            == realizingKeys.end())
            continue;
        // An archive OUTPUT never links the runtime, so its key is never read.
        if ((*format)->isStaticArchive()) continue;

        for (auto const& targetName : targetNames) {
            auto const target = TargetSchema::loadShipped(targetName);
            if (!target.has_value()) continue;
            // ⚠ THE REPORTER IS LOCAL AND DISCARDED. A non-matching pair is the
            // ORDINARY case here — this loop is a cross product — so its
            // machine-mismatch diagnostic is not an event.
            DiagnosticReporter pairing;
            if (!crossValidateTargetFormat(**target, **format, pairing)) continue;
            ++pairs;

            for (auto const config : kConfigs) {
                ++attempts;
                Program program;
                program.setCompileConfig(config);
                program.setOutputDir(scratch.path()
                                     / ("cell-" + std::to_string(cell++)));
                DiagnosticReporter rep;
                if (program.compileFiles(
                        std::vector<std::string>{src.generic_string()}, "c",
                        std::vector<std::string>{targetName + ":" + formatName},
                        rep)
                    == 0) {
                    ++built;
                }
            }
        }
    }

    // ★★ VACUITY, REFUSED AT EVERY LEVEL IT COULD CREEP IN. "No failures" over
    // zero pairs is the classic way a fixture stops doing anything while still
    // reporting green, and each of these three names a different level at which
    // that could happen.
    EXPECT_GT(pairs, 0u)
        << "the realizing format kind(s) " << realizingKeys.size()
        << " reach NO (target, format) pair, so no runtime archive can be "
           "materialised and every later build would still be the first to miss";
    EXPECT_GT(attempts, 0u) << "zero compiles were attempted";
    EXPECT_GT(built, 0u)
        << built << " of " << attempts
        << " warm-up compile(s) produced an artifact. NOT every pair has to — "
           "whether a format can host a bare `main` is another test's subject — "
           "but if NONE can, this fixture ran nothing that could have populated "
           "the cache and the exact phase-count instruments it protects are "
           "measuring an unknown number of builds.";
}
