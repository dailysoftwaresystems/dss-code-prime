// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE CACHE'S **WIRING**, DRIVEN
// THROUGH THE REAL DRIVER.
//
// ═══ WHAT THIS FILE IS FOR, AND WHY `test_runtime_object_cache` IS NOT IT ════
//
// `program/test_runtime_object_cache` is a UNIT suite: it hands
// `computeRuntimeObjectKey` a hand-built request and proves that every term
// moves the key. Every one of its cases stayed green for the entire period in
// which the cache had ZERO production call sites — it can prove the mechanism is
// correct and cannot notice that nothing uses it.
//
// This file asserts the other half, and only the other half: that the DRIVER
// feeds the real terms in, that a HIT actually skips the compile, and that a
// change to any input the driver supplies produces a MISS AND A RECOMPILE.
//
// ═══ THE INSTRUMENT: `PhaseTimers::read(Optimize).runs`, NOT A CLOCK ═════════
//
// A wall-clock comparison would make this file a benchmark with a threshold,
// and thresholds on a host that drifts ~25% over tens of minutes are how a test
// starts passing for the wrong reason. `CompilePhase::Optimize`'s scope is
// opened inside `optimizeModule`, so its `runs` is the number of INVOCATIONS —
// exact, deterministic, and independent of how fast anything ran.
//
//   HIT  — the user's sole CU only: the unit stage inside `buildCuMir` plus the
//          driver's program stage. 2.
//   MISS — the same 2, plus a whole nested `Program` build per runtime unit the
//          format realizes (each 1 CU ⇒ 2 more). Strictly greater than a hit.
//
// ⚠ THE HIT COUNT IS MEASURED, NEVER SPELLED. It is read from a run PROVEN warm
// by an immediately preceding identical run, so this file carries no `2u` that
// would have to be maintained when a schedule changes. What each case asserts is
// the RELATION — `miss > hit` — which is the property, and `hit == hit` for the
// control, which is what makes the relation mean something.
//
// ⚠ AND THE BASELINE IS ESTABLISHED BY TWO RUNS, NOT ONE. The first run against
// an empty cache is a MISS by definition; taking it as the hit baseline would
// compare a miss to a miss and pass no matter what.
//
// ═══ NOTHING BELOW NAMES A TARGET OR A FORMAT ════════════════════════════════
//
// The (target, format) pair under test is DISCOVERED: the closed
// `ObjectFormatKind` vocabulary is offered to `allShippedSourcesForFormat`, the
// kinds that answer with a source are the realizing ones, and the pairs come
// from the shipped trees joined by `crossValidateTargetFormat`. A hard-coded
// `pe64-…` here would silently stop testing anything the day a second format
// kind ships a runtime — and it would assert on this host what it could not
// assert on another. The shipped runtime unit, the descriptor that declares it,
// and — for `AHitAndAMissEmitTheSameImage` — the header and symbol its probe
// source references are discovered the same way.
//
// ⚠ THE **LANGUAGE** IS THE ONE EXCEPTION, AND IT IS SPELLED: every case calls
// `compileFiles(…, "c", …)`. It has to. The harness authors a `.c` translation
// unit, so the language is a property of the source this file WRITES rather
// than of the corpus it reads — and this docblock claimed the clean sweep of
// all three until cycle P36, which is the kind of overstatement that makes a
// reader trust the other two claims less. The other two hold.
//
// ⚠ EVERY CASE RUNS AGAINST A **STAGED** CONFIG TREE AND A **PRIVATE** CACHE
// ROOT. The staging is what makes "edit a config document" and "edit the shipped
// source" expressible at all; the private root (`DSS_RUNTIME_CACHE_DIR`) is what
// keeps these cases from writing into — or reading out of — the developer's real
// cache, where a leftover entry from an earlier run would turn a MISS case green
// while measuring nothing.

#include "core/substrate/phase_timers.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "core/types/config_path_walk.hpp"  // findShippedConfigDir — the staged-tree self-check
#include "program/cli_args.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/program.hpp"
#include "program/runtime_object_cache.hpp"

#include <nlohmann/json.hpp>

#include "repo_root.hpp"
#include "scoped_env.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using namespace dss;
using dss::substrate::CompilePhase;
using dss::substrate::PhaseTimers;
using dss::test_support::Location;
using dss::test_support::ScopedEnv;
using dss::test_support::ScratchDir;

namespace {

[[nodiscard]] fs::path realConfigRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

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

[[nodiscard]] std::string readWhole(fs::path const& p) {
    std::ifstream in{p, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool writeWhole(fs::path const& p, std::string_view text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    if (!out.good()) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    return out.good();
}

// ── The subject: one realizing (target, format) pair, plus its unit list ─────
struct Subject {
    std::string              formatKey;   // "pe" / "elf" / … — from the corpus
    std::string              target;
    std::string              format;
    std::vector<std::string> units;       // config-root-relative sources
    [[nodiscard]] std::string spec() const { return target + ":" + format; }
};

// The FIRST realizing pair, over the real trees. First in a SORTED walk, so the
// subject is the same on NTFS and on ext4 rather than whatever the filesystem
// happened to hand back.
[[nodiscard]] std::optional<Subject> discoverSubject() {
    fs::path const root = realConfigRoot();
    if (root.empty()) return std::nullopt;
    for (auto const& row : kObjectFormatKindTable.rows) {
        if (!isSelectableObjectFormatKind(row.first)) continue;
        std::string const key{row.second};
        auto const units =
            dss::ffi::allShippedSourcesForFormat(root / "shippedLibs", key);
        if (units.empty()) continue;
        for (auto const& formatName :
             shippedSchemaNames(root / "object-formats", ".format.json")) {
            auto const format = ObjectFormatSchema::loadShipped(formatName);
            if (!format.has_value()) continue;
            if (objectFormatKindName((*format)->kind()) != key) continue;
            // An archive OUTPUT never links the runtime, so it is the wrong
            // subject for a wiring test about what the link receives.
            if ((*format)->isStaticArchive()) continue;
            for (auto const& targetName :
                 shippedSchemaNames(root / "targets", ".target.json")) {
                auto const target = TargetSchema::loadShipped(targetName);
                if (!target.has_value()) continue;
                DiagnosticReporter pairing;   // ordinary mismatch, not an event
                if (!crossValidateTargetFormat(**target, **format, pairing))
                    continue;
                return Subject{key, targetName, formatName, units};
            }
        }
    }
    return std::nullopt;
}

// ── A staged tree the cases may edit, and a private cache root ──────────────
//
// ⚠⚠ `DSS_CONFIG_ROOT` NAMES THE TREE ROOT — the directory that CONTAINS
// `src/dss-config/` — AND NOT THE CONFIG DIRECTORY ITSELF. Both fields are kept
// here rather than composing one from the other at each use, because pointing
// the variable at `configDir` does not FAIL: the walk simply falls through to
// its next arm and resolves the developer's REAL tree, so every mutation below
// lands on a tree nobody reads and every MISS case reports a hit. ✔MEASURED —
// that is exactly what the first cut of this file did, and the three
// file-content cases went red claiming a product defect that was not there.
struct Harness {
    ScratchDir  scratch{Location::Temp, "runtime-cache-wiring"};
    fs::path    treeRoot;    // what DSS_CONFIG_ROOT must be set to
    fs::path    configDir;   // treeRoot/src/dss-config — what the cases edit
    fs::path    cacheDir;
    fs::path    source;
};

[[nodiscard]] bool layDown(Harness& h) {
    h.treeRoot  = h.scratch.path() / "tree";
    h.configDir = h.treeRoot / "src" / "dss-config";
    h.cacheDir  = h.scratch.path() / "cache";
    h.source    = h.scratch.path() / "wiring.c";
    std::error_code ec;
    fs::create_directories(h.configDir.parent_path(), ec);
    if (ec) return false;
    fs::copy(realConfigRoot(), h.configDir, fs::copy_options::recursive, ec);
    if (ec) return false;
    return writeWhole(h.source, "int main(void) { return 0; }\n");
}

// ★ THE HARNESS PROVES IT IS POINTED AT ITS OWN TREE BEFORE ANY CASE RUNS.
// Without this the whole file could silently measure the real repo: the walk's
// fallback arms are precisely designed not to fail. Asserted by MUTATION rather
// than by comparing paths — a path comparison would only restate the string
// this file just built, while this asks the driver's own resolver where it
// would read the descriptor corpus from.
[[nodiscard]] bool stagedTreeIsTheOneBeingRead(Harness const& h) {
    auto const dir = findShippedConfigDir("shippedLibs");
    if (!dir) return false;
    std::error_code ec;
    return fs::equivalent(dir->parent_path(), h.configDir, ec) && !ec;
}

// What one production compile produced: the Optimize invocation count (the
// hit/miss instrument) and the image it wrote (the byte-identity subject).
struct CompileOutcome {
    std::uint64_t             optimizeRuns = 0;
    std::optional<fs::path>   artifact;
};

// One compile through the production driver. `PhaseTimers::reset()` FIRST, so
// the count is this run's.
//
// ⚠ THE COMPILE MUST SUCCEED. A failed build runs fewer phases than a
// successful one, so a case that silently accepted rc!=0 could read "fewer
// Optimize runs" and call it a cache hit.
[[nodiscard]] CompileOutcome compileOnce(Harness const&     h,
                                         std::string const& spec,
                                         CompileConfig      config,
                                         unsigned           cell) {
    PhaseTimers::reset();
    Program program;
    program.setCompileConfig(config);
    program.setOutputDir(h.scratch.path() / ("out-" + std::to_string(cell)));
    DiagnosticReporter rep;
    int const rc = program.compileFiles(
        std::vector<std::string>{h.source.generic_string()}, "c",
        std::vector<std::string>{spec}, rep);
    EXPECT_EQ(rc, 0) << "the wiring harness's own compile failed (spec " << spec
                     << ", cell " << cell << "), so the phase count below "
                        "describes a broken build rather than a cache outcome";
    CompileOutcome out;
    out.optimizeRuns = PhaseTimers::read(CompilePhase::Optimize).runs;
    // The PRODUCER answers where it wrote — never a second copy of the
    // `<outputDir>/<stem><ext>` formula, which is the drift
    // `Program::artifactPaths()` exists to remove.
    if (program.artifactPaths().size() == 1u) {
        out.artifact = program.artifactPaths().front();
    }
    return out;
}

// The count alone, for the cases that assert only on the relation.
[[nodiscard]] std::uint64_t compileAndCountOptimize(Harness const&     h,
                                                    std::string const& spec,
                                                    CompileConfig      config,
                                                    unsigned           cell) {
    return compileOnce(h, spec, config, cell).optimizeRuns;
}

// Every `.a` under the private cache root, sorted — the second, independent
// witness. The phase count says a compile happened; this says WHICH entry the
// key named, so a case cannot pass on a recompile that stored under the SAME
// key (which would be a cache that never hits).
[[nodiscard]] std::vector<std::string> cacheArtifacts(fs::path const& cacheDir) {
    std::vector<std::string> out;
    std::error_code          ec;
    for (fs::recursive_directory_iterator it{cacheDir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() != ".a") continue;
        std::error_code relEc;
        fs::path const rel = fs::relative(it->path(), cacheDir, relEc);
        out.push_back(relEc ? it->path().generic_string() : rel.generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The shape every mutation case shares: warm the cache, PROVE it is warm, then
// mutate exactly one input and require the next build to compile again AND to
// name a different entry.
//
// `mutate` receives the harness after the baseline and returns the spec + config
// the third build must use, so a case can move an input that lives in the
// INVOCATION (the format, the configuration) as easily as one that lives in a
// FILE.
struct Followup {
    std::string   spec;
    CompileConfig config;
};

void expectMutationMissesAndRecompiles(
    char const* axis,
    Followup (*mutate)(Harness&, Subject const&)) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value())
        << "no (target, object format) pair of a realizing format kind was "
           "discovered, so this file would assert nothing about the cache";
    ASSERT_FALSE(subject->units.empty());

    Harness h;
    ASSERT_TRUE(layDown(h)) << "cannot stage the config tree / source";
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h))
        << "the driver would read a config tree other than this harness's staged "
           "one, so every mutation below would land on a tree nobody reads";

    // (1) COLD: a miss by definition. Its count is NOT the baseline.
    std::uint64_t const cold = compileAndCountOptimize(h, subject->spec(),
                                                       CompileConfig::Release, 0);
    auto const afterCold = cacheArtifacts(h.cacheDir);
    ASSERT_FALSE(afterCold.empty())
        << axis << ": the first build against an EMPTY cache root wrote no "
                   "artifact, so the cache is not wired at all and every "
                   "assertion below would be vacuous";

    // (2) WARM: the baseline, and the proof that a hit is cheaper than a miss.
    std::uint64_t const hit = compileAndCountOptimize(h, subject->spec(),
                                                      CompileConfig::Release, 1);
    ASSERT_LT(hit, cold)
        << axis << ": the second identical build ran as many optimize passes as "
                   "the first (" << hit << " vs " << cold
        << "), so the cache did not HIT and this file cannot tell a miss from a "
           "hit.";
    ASSERT_EQ(cacheArtifacts(h.cacheDir), afterCold)
        << axis << ": a warm build changed the cache contents";

    // (3) MUTATE exactly one input, then build again.
    Followup const next = mutate(h, *subject);
    std::uint64_t const after = compileAndCountOptimize(h, next.spec,
                                                        next.config, 2);
    EXPECT_GT(after, hit)
        << axis << ": changing this input did NOT cause a recompile (" << after
        << " optimize run(s), the same as a hit). The cache served an artifact "
           "built from DIFFERENT inputs — a stale object linked into a "
           "working-looking binary, which is the failure this key exists to "
           "make impossible.";
    auto const afterMutation = cacheArtifacts(h.cacheDir);
    EXPECT_NE(afterMutation, afterCold)
        << axis << ": the build recompiled but stored under the SAME cache "
                   "entry, so the key did not move with the input.";
}

}  // namespace

// ═══ THE CONTROL ════════════════════════════════════════════════════════════
//
// Without this every MISS case below could be satisfied by a cache that never
// hits at all — the degenerate "always recompile" implementation passes every
// invalidation test ever written.
TEST(RuntimeCacheWiring, AnUnchangedRebuildHitsAndDoesNotRecompileTheRuntime) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value());

    Harness h;
    ASSERT_TRUE(layDown(h));
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h))
        << "the driver would read a config tree other than this harness's staged "
           "one, so every mutation below would land on a tree nobody reads";

    std::uint64_t const cold = compileAndCountOptimize(h, subject->spec(),
                                                       CompileConfig::Release, 0);
    std::uint64_t const warm1 = compileAndCountOptimize(h, subject->spec(),
                                                        CompileConfig::Release, 1);
    std::uint64_t const warm2 = compileAndCountOptimize(h, subject->spec(),
                                                        CompileConfig::Release, 2);

    EXPECT_GT(cold, warm1)
        << "the first build against an empty cache ran no more optimize passes "
           "than a warm one (" << cold << " vs " << warm1
        << "), so either nothing was cached or nothing was compiled";
    EXPECT_EQ(warm1, warm2)
        << "two consecutive warm builds disagree (" << warm1 << " vs " << warm2
        << ") — the cache is not stable across invocations";

    // ⓘ The unit COUNT is asserted through the arithmetic rather than restated:
    // each realized unit costs a 1-CU nested build, i.e. exactly two optimize
    // invocations, so a corpus of N units makes the cold build 2N dearer. A
    // wiring that silently compiled only SOME of them would land short here.
    EXPECT_EQ(cold - warm1, 2u * subject->units.size())
        << "a cold build cost " << (cold - warm1)
        << " extra optimize invocation(s) over " << subject->units.size()
        << " realized unit(s); two per unit is what one nested single-CU "
           "archive build costs, so a different number means the driver "
           "materialised a different set of units than the corpus declares";
}

// ═══ THE STORE'S OWN SHAPE ══════════════════════════════════════════════════

// The build stamp cannot be varied from inside a running binary, so the two
// halves of "a compiler rebuilt from different sources cannot address the
// previous compiler's objects" are pinned in two places: that two DIFFERENT
// stamps render two different segments (`RuntimeObjectCacheRoots.
// DifferentBuildStampsRenderDifferentRootSegments`), and — here — that the
// driver's entries actually live UNDER that segment. Either alone is
// satisfiable by a broken implementation; together they are the property.
TEST(RuntimeCacheWiring, EveryStoredEntryLivesUnderTheBuildStampSegment) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value());

    Harness h;
    ASSERT_TRUE(layDown(h));
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h))
        << "the driver would read a config tree other than this harness's staged "
           "one, so every mutation below would land on a tree nobody reads";
    (void)compileAndCountOptimize(h, subject->spec(), CompileConfig::Release, 0);

    std::string const segment = dss::runtime::runtimeCacheBuildStampSegment();
    ASSERT_FALSE(segment.empty());
    auto const stored = cacheArtifacts(h.cacheDir);
    ASSERT_FALSE(stored.empty()) << "the build stored nothing";
    for (auto const& rel : stored) {
        EXPECT_TRUE(rel.starts_with(segment + "/"))
            << "cache entry '" << rel << "' is not under the build-stamp "
               "segment '" << segment
            << "', so a differently-built compiler would address this "
               "compiler's objects";
    }
}

// ═══ THE FIVE INPUTS, ONE CASE EACH ═════════════════════════════════════════

TEST(RuntimeCacheWiring, EditingTheShippedRuntimeSourceMissesAndRecompiles) {
    expectMutationMissesAndRecompiles(
        "shipped source bytes", [](Harness& h, Subject const& s) {
            fs::path const unit = h.configDir / s.units.front();
            std::string    text = readWhole(unit);
            EXPECT_FALSE(text.empty()) << "the staged unit is empty";
            // A comment: it changes the BYTES and nothing the compiler does, so
            // a recompile can only be attributed to the digest.
            EXPECT_TRUE(writeWhole(unit, text + "/* wiring-probe */\n"));
            return Followup{s.spec(), CompileConfig::Release};
        });
}

TEST(RuntimeCacheWiring, EditingTheDeclaringDescriptorMissesAndRecompiles) {
    expectMutationMissesAndRecompiles(
        "declaring descriptor bytes", [](Harness& h, Subject const& s) {
            // Which descriptor declares the unit is asked of the SAME reader the
            // driver uses, so this case cannot drift from the attribution the
            // key is built from.
            std::error_code ec;
            for (fs::recursive_directory_iterator
                     it{h.configDir / "shippedLibs", ec}, end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                std::error_code typeEc;
                if (!it->is_regular_file(typeEc) || typeEc) continue;
                if (it->path().extension() != ".json") continue;
                auto const declared =
                    dss::ffi::readShippedSourcesForFormat(it->path(), s.formatKey);
                if (std::find(declared.begin(), declared.end(), s.units.front())
                    == declared.end())
                    continue;
                std::string text = readWhole(it->path());
                // Trailing whitespace: every JSON reader ignores it, so the
                // document's MEANING is untouched and only its digest moves.
                EXPECT_TRUE(writeWhole(it->path(), text + "   \n"));
                return Followup{s.spec(), CompileConfig::Release};
            }
            ADD_FAILURE() << "no staged descriptor declares '" << s.units.front()
                          << "' for format key '" << s.formatKey
                          << "', so nothing was mutated";
            return Followup{s.spec(), CompileConfig::Release};
        });
}

TEST(RuntimeCacheWiring, EditingAConfigDocumentTheBuildReadsMissesAndRecompiles) {
    expectMutationMissesAndRecompiles(
        "config document bytes", [](Harness& h, Subject const& s) {
            // The build's own OBJECT FORMAT document — one of the documents the
            // loaders report into `loadedDocuments`. Trailing whitespace again:
            // the digest moves, the meaning does not, so a recompile is
            // attributable to the key and to nothing else.
            fs::path const doc = h.configDir / "object-formats"
                               / (s.format + ".format.json");
            std::string text = readWhole(doc);
            EXPECT_FALSE(text.empty()) << doc.generic_string() << " is empty";
            EXPECT_TRUE(writeWhole(doc, text + "   \n"));
            return Followup{s.spec(), CompileConfig::Release};
        });
}

TEST(RuntimeCacheWiring, ADifferentObjectFormatMissesAndRecompiles) {
    expectMutationMissesAndRecompiles(
        "object format", [](Harness& h, Subject const& s) {
            // A SECOND non-archive format of the same kind that pairs with the
            // same target — discovered, never named. If the corpus has only one,
            // the case says so rather than passing silently.
            auto const target = TargetSchema::loadShipped(s.target);
            EXPECT_TRUE(target.has_value());
            if (target.has_value()) {
                for (auto const& name : shippedSchemaNames(
                         h.configDir / "object-formats", ".format.json")) {
                    if (name == s.format) continue;
                    auto const format = ObjectFormatSchema::loadShipped(name);
                    if (!format.has_value()) continue;
                    if (objectFormatKindName((*format)->kind()) != s.formatKey)
                        continue;
                    if ((*format)->isStaticArchive()) continue;
                    DiagnosticReporter pairing;
                    if (!crossValidateTargetFormat(**target, **format, pairing))
                        continue;
                    return Followup{s.target + ":" + name, CompileConfig::Release};
                }
            }
            ADD_FAILURE()
                << "format kind '" << s.formatKey << "' reaches only one "
                << "non-archive format for target '" << s.target
                << "', so the object-format axis of the key cannot be exercised "
                   "through the driver on this corpus";
            return Followup{s.spec(), CompileConfig::Release};
        });
}

TEST(RuntimeCacheWiring, ADifferentBuildConfigurationMissesAndRecompiles) {
    expectMutationMissesAndRecompiles(
        "build configuration", [](Harness&, Subject const& s) {
            // The baseline above builds Release; Debug is the other member of
            // the closed vocabulary. A release-only miscompile is a shape this
            // repo has shipped before, which is why the config is in the key.
            return Followup{s.spec(), CompileConfig::Debug};
        });
}

// ═══ THE CACHE MAY NEVER BE LOAD-BEARING FOR CORRECTNESS ════════════════════

// An unwritable cache root is an ordinary state of a machine — a read-only
// volume, a quota, a container with no HOME. It must cost a note and a repeated
// compile, NEVER a build. `DSS_RUNTIME_CACHE_DIR` is pointed at a path that
// cannot become a directory (an existing FILE), which is the same failure the
// store reports for every other unwritable root.
TEST(RuntimeCacheWiring, AnUnwritableCacheRootStillProducesAWorkingBuild) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value());

    Harness h;
    ASSERT_TRUE(layDown(h));
    fs::path const blocker = h.scratch.path() / "not-a-directory";
    ASSERT_TRUE(writeWhole(blocker, "this is a file, not a cache root\n"));

    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", blocker.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h));

    Program program;
    program.setCompileConfig(CompileConfig::Release);
    fs::path const outDir = h.scratch.path() / "unwritable-out";
    program.setOutputDir(outDir);
    DiagnosticReporter rep;
    ASSERT_EQ(program.compileFiles(
                  std::vector<std::string>{h.source.generic_string()}, "c",
                  std::vector<std::string>{subject->spec()}, rep), 0)
        << "a cache root that cannot be written FAILED THE BUILD. The cache is "
           "an optimization and may never be load-bearing for correctness.";
    EXPECT_FALSE(rep.hasErrors());
    ASSERT_EQ(program.artifactPaths().size(), 1u);
    ASSERT_TRUE(program.artifactPaths().front().has_value())
        << "the build reported success and produced no artifact";
    EXPECT_TRUE(fs::is_regular_file(*program.artifactPaths().front()));
}

// ⛔ THE OTHER DIRECTION, AND IT IS DELIBERATELY **NOT** SYMMETRIC. An entry
// that cannot be shown to be this key's is a REFUSAL, never a miss — that
// refusal is the whole reason the 16-character path index is an index rather
// than a weakening (see the ★★★ section at the top of `runtime_object_cache.hpp`).
// If a corrupt entry silently fell back to recompiling, deleting a `.key`
// sidecar would restore the un-verified 80-bit behaviour by the back door.
TEST(RuntimeCacheWiring, AnArtifactWithNoKeyDocumentRefusesTheBuild) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value());

    Harness h;
    ASSERT_TRUE(layDown(h));
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h))
        << "the driver would read a config tree other than this harness's staged "
           "one, so every mutation below would land on a tree nobody reads";
    (void)compileAndCountOptimize(h, subject->spec(), CompileConfig::Release, 0);
    ASSERT_FALSE(cacheArtifacts(h.cacheDir).empty());

    // Remove the sidecar beside ONE artifact — the state an interrupted store
    // must never leave, and the state a hand-copied cache entry does leave.
    std::size_t     removed = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it{h.cacheDir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() != ".a") continue;
        fs::path const sidecar =
            dss::runtime::runtimeKeyDocumentPath(it->path());
        std::error_code rmEc;
        if (fs::remove(sidecar, rmEc) && !rmEc) ++removed;
        break;
    }
    ASSERT_EQ(removed, 1u) << "no key document was removed, so the refusal "
                              "below would be measuring nothing";

    Program program;
    program.setCompileConfig(CompileConfig::Release);
    program.setOutputDir(h.scratch.path() / "refuse-out");
    DiagnosticReporter rep;
    int const rc = program.compileFiles(
        std::vector<std::string>{h.source.generic_string()}, "c",
        std::vector<std::string>{subject->spec()}, rep);
    EXPECT_NE(rc, 0)
        << "an artifact whose key document is GONE was accepted. It cannot be "
           "shown to belong to this key, so serving it is exactly the "
           "unverified-truncation behaviour the sidecar exists to prevent.";
    EXPECT_TRUE(rep.hasErrors());
}

// ═══ THE LANGUAGE IS NAMED BY ITS CONFIG STEM, NOT BY ITS DECLARED NAME ═════
//
// ★★★ THIS PIN EXISTS BECAUSE THE WINDOWS GATE CANNOT SEE THE DEFECT IT PINS.
// A language has two names: the one its document DECLARES (`language.name`,
// which for C is "C") and the one the config tree is INDEXED BY (the file stem,
// "c"). The nested runtime build once passed the DECLARED name as `--language`,
// so it resolved `sources/C.lang.json`.
//
//   * NTFS and APFS are case-insensitive: same file, build green.
//   * ext4 is case-sensitive: NO file. ✔MEASURED 2026-08-25 — Windows ran
//     1656/1656 while the WSL leg reported
//     `error[C_InvalidLanguageName] at C: no shipped language config found for
//     'C'` and took 527 tests down with it.
//
// ⚠⚠ AND C IS THE MILD CASE. Reading this defect as a CASE problem understates
// the class by half: ✔MEASURED over the shipped corpus, three of the five
// nameable language documents declare a name that is not a file stem at all —
// `asm-arm64-gas` → "AsmArm64Gas", `asm-x86_64-att` → "AsmX86_64Att",
// `tsql-subset` → "TsqlSubset". The substitution resolves to NOTHING on NTFS
// and APFS for those, so it is not a Linux-only class; it is a class whose
// host-dependent members are the ones that survive a Windows gate long enough
// to ship. The corpus census is re-measured on every run by
// `GrammarSchemaConfigName.ShippedCorpusStemAndDeclaredNameDivergeByMoreThanCase`
// in `tests/core/test_grammar_schema.cpp`.
//
// ⚠ SO THIS ASSERTION MUST NOT ASK THE FILESYSTEM ANYTHING. `fs::exists` on the
// upper-case spelling is TRUE on two of the three hosts this repository gates
// on, so a gate built from it would reproduce the exact blindness that let the
// defect ship. A `std::string` comparison is case-sensitive everywhere, which is
// why the key document is read as TEXT and compared byte for byte.
//
// ⚠ BOTH DIRECTIONS ARE ASSERTED. "the lower-case spelling is present" would
// still pass if a future change emitted BOTH; "the upper-case spelling is
// absent" is what makes it exact.
TEST(RuntimeCacheWiring, TheKeyNamesTheLanguageByItsConfigStemAndNotItsDeclaredName) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value());

    Harness h;
    ASSERT_TRUE(layDown(h));
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h))
        << "the driver would read a config tree other than this harness's staged "
           "one, so this assertion would be reading somebody else's key";
    (void)compileAndCountOptimize(h, subject->spec(), CompileConfig::Release, 0);

    // The key document beside any stored artifact carries the unit-language
    // document path. One is enough: every unit in this build resolved the same
    // language through the same call.
    std::string     keyText;
    std::error_code ec;
    for (fs::recursive_directory_iterator it{h.cacheDir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() != ".a") continue;
        std::ifstream in{dss::runtime::runtimeKeyDocumentPath(it->path()),
                         std::ios::binary};
        if (!in) continue;
        keyText.assign(std::istreambuf_iterator<char>{in},
                       std::istreambuf_iterator<char>{});
        break;
    }
    ASSERT_FALSE(keyText.empty())
        << "no key document was read, so the two assertions below would both "
           "pass over an empty string and prove nothing";

    EXPECT_NE(keyText.find("sources/c.lang.json"), std::string::npos)
        << "the cache key does not name the C language document by the name the "
           "config tree is INDEXED by (the `c.lang.json` stem). Whatever it "
           "names is what the nested runtime build will pass to `--language`, "
           "and a name that is not a file stem resolves to nothing on a "
           "case-sensitive filesystem.\nKey document:\n"
        << keyText;
    EXPECT_EQ(keyText.find("sources/C.lang.json"), std::string::npos)
        << "the cache key names the C language document by its DECLARED name "
           "(`language.name` == \"C\") rather than by its file stem. That "
           "spelling resolves on NTFS and APFS and resolves to NOTHING on ext4 "
           "— the defect that cost 527 tests on the WSL leg while the Windows "
           "gate stayed green.\nKey document:\n"
        << keyText;
}

// ═══ A RUNTIME THAT CANNOT BE BUILT MUST NOT LEAVE A BINARY BEHIND ══════════
//
// ★★★ THE FILE ON DISK IS THE ASSERTION, NOT THE EXIT CODE. ✔MEASURED during
// this wiring's development: the refusal fired, the message was right and the
// process exited 1 — AND a `.exe` sat in the output directory with a
// `dsscp: artifact …` line announcing it, because the failure was detected
// inside the per-target loop and the link ran anyway. That binary is missing a
// runtime body, and with [[D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD]] still OPEN
// it links clean and dies at run time — the exact failure the shipped-source
// mechanism exists to prevent, reintroduced one tier down. An exit code is read
// by a shell; a file is read by whatever picks it up next.
TEST(RuntimeCacheWiring, ARuntimeUnitThatDoesNotCompileWritesNoArtifact) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value());

    Harness h;
    ASSERT_TRUE(layDown(h));
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h));

    // Break the STAGED unit, never the real one. Appended rather than replaced
    // so the file still exists and still resolves: the refusal under test is the
    // COMPILE failing, not a missing body, and those are different arms.
    fs::path const unit = h.configDir / subject->units.front();
    std::string const before = readWhole(unit);
    ASSERT_FALSE(before.empty());
    ASSERT_TRUE(writeWhole(unit, before + "this is not a translation unit ###\n"));

    Program program;
    program.setCompileConfig(CompileConfig::Release);
    fs::path const outDir = h.scratch.path() / "broken-runtime-out";
    program.setOutputDir(outDir);
    DiagnosticReporter rep;
    int const rc = program.compileFiles(
        std::vector<std::string>{h.source.generic_string()}, "c",
        std::vector<std::string>{subject->spec()}, rep);

    EXPECT_NE(rc, 0) << "a shipped runtime unit that does not compile was "
                        "silently skipped";
    // The driver's own attribution, not merely SOME error: a parse error from
    // the nested build alone would not tell a reader WHICH shipped unit broke
    // or which target it was being compiled for.
    bool attributed = false;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        if (d.actual.find("FAILED TO COMPILE") != std::string::npos
            && d.actual.find(subject->units.front()) != std::string::npos) {
            attributed = true;
        }
    }
    EXPECT_TRUE(attributed)
        << "the build failed without naming the shipped runtime unit that "
           "caused it — a parse error in a file the user never wrote is "
           "otherwise unattributable";

    // ★ THE POINT OF THE CASE.
    std::error_code ec;
    std::vector<std::string> written;
    for (fs::recursive_directory_iterator it{outDir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (it->is_regular_file(typeEc) && !typeEc)
            written.push_back(it->path().filename().generic_string());
    }
    EXPECT_TRUE(written.empty())
        << "the build FAILED and still wrote " << written.size()
        << " file(s) into its output directory (first: "
        << (written.empty() ? std::string{"<none>"} : written.front())
        << "). An image missing a runtime body links clean and dies at run "
           "time, so a failed build must leave nothing that looks linkable.";
    ASSERT_EQ(program.artifactPaths().size(), 1u);
    EXPECT_FALSE(program.artifactPaths().front().has_value())
        << "the failed target reported an artifact path, so a consumer would "
           "link a binary this build refused to stand behind";
}


// ═══ THE PROPERTY THE WHOLE DESIGN WAS CHOSEN FOR ═══════════════════════════
//
// ★★★ A HIT AND A MISS MUST EMIT THE SAME IMAGE, BYTE FOR BYTE.
//
// Every other case in this file asserts that the cache HITS when it should and
// MISSES when it should. None of them asserts the thing those two outcomes are
// only allowed to differ in — namely NOTHING that reaches the user. The runtime
// archives are resolved and handed to the link on the hit path and the miss
// path alike, precisely so that "was this compiler's cache warm?" is not an
// input to the program it produces.
//
// ⚠ THIS IS THE ONE REGRESSION THAT WOULD MAKE A GREEN SUITE GO RED PURELY BY
// BEING RUN TWICE. A hit path that fed the link a different set would produce a
// working binary on a cold machine and a broken one on a warm machine, or the
// reverse, and every symptom would point at whatever the second run happened to
// be testing.
//
// ✔MEASURED BY HAND FIRST (2026-08-25, cycle P36): `examples/c/
// shipped_dirent_readdir/main.c` built `--config=release` for
// `x86_64:pe64-x86_64-windows-exec` against an EMPTY `DSS_RUNTIME_CACHE_DIR`
// (miss, 2 entries written) and again against the warm cache (hit) produced
// byte-identical images — md5 cd0722b9b102785f36a578689107a8d0, 12288 bytes,
// `cmp -l` reporting 0 differing bytes — and both exited 42, their declared
// pass condition.
//
// ═══ ⚠⚠ WHY THE SUBJECT IS NOT `int main(void){return 0;}` ══════════════════
//
// ✔MEASURED 2026-08-25 BY MUTATION, and it is the reason this case builds its
// own source instead of reusing the harness's. `allShippedSourcesForFormat`'s
// own docblock states the rule: *COMPILE-ALWAYS IS NOT LINK-ALWAYS* — what
// reaches the image stays demand-driven, so a program that calls nothing in the
// runtime links NO archive member. With the trivial harness source, a mutant
// that made a cache HIT push NO archive at all left every image byte-identical
// and every case in this file GREEN, this one included. The property held; the
// subject simply could not tell the two apart.
//
// The same mutant against a program that DOES demand the runtime is
// catastrophic and instant: cold built `probe.exe`, warm reported
// `error[K_SymbolUndefined] undefined symbol 'opendir'` and wrote nothing.
//
// ⇒ THE PROBE IS BUILT FROM THE CORPUS, NOT SPELLED HERE. The descriptor that
// declares the discovered unit supplies BOTH halves: its `header` is what the
// probe includes, and one of its `symbols` is what the probe references. No
// header name, symbol name, platform or library appears in this file.
//
// ⓘ IT TAKES AN ADDRESS AND NEVER CALLS. A call would need the symbol's
// SIGNATURE, which is exactly the descriptor detail this case has no business
// decoding (`readShippedLibDescriptor` would want a TypeInterner and a
// TypeRegistry for it). `typeof(sym) *const p = sym;` at file scope needs no
// signature, and an external-linkage `const` object holding the address is a
// relocation no optimizer can fold away — which matters because the subject is
// built `--config=release`.
//
// ⚠ THE COMPARISON IS THE BYTES, NOT THE SIZE. Two images of equal size that
// differ in a relocation or a member order are exactly the failure this guards,
// and a size check would sail past it. The first index at which they differ is
// reported, because "the images differ" alone sends a reader nowhere.

namespace {

// The staged descriptor that declares `unit` for `formatKey` — asked of the
// SAME reader the driver uses, so this cannot drift from the attribution the
// key is built from.
[[nodiscard]] fs::path descriptorDeclaring(fs::path const&    descriptorDir,
                                           std::string const& unit,
                                           std::string const& formatKey) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it{descriptorDir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() != ".json") continue;
        auto const declared =
            dss::ffi::readShippedSourcesForFormat(it->path(), formatKey);
        if (std::find(declared.begin(), declared.end(), unit) != declared.end()) {
            return it->path();
        }
    }
    return {};
}

// A translation unit that DEMANDS the runtime, composed from the descriptor's
// own `header` and one of its own `symbols`. Empty when the descriptor cannot
// supply both — the caller fails loudly rather than falling back to a source
// that would make the case vacuous.
//
// ⚠ THE SYMBOL MUST BE AVAILABLE ON THIS FORMAT. `availableObjectFormats` is
// per-symbol and restricting: errno's accessor is spelled differently per
// platform and a symbol absent here would be undefined at link for a reason
// that has nothing to do with the cache. Absent/empty means every format.
[[nodiscard]] std::string runtimeDemandingProbeSource(fs::path const&    descriptor,
                                                      std::string const& formatKey) {
    std::string const text = readWhole(descriptor);
    if (text.empty()) return {};
    nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return {};

    auto const header = doc.find("header");
    if (header == doc.end() || !header->is_string()) return {};
    auto const symbols = doc.find("symbols");
    if (symbols == doc.end() || !symbols->is_array()) return {};

    std::string chosen;
    for (auto const& sym : *symbols) {
        if (!sym.is_object()) continue;
        auto const name = sym.find("name");
        if (name == sym.end() || !name->is_string()) continue;
        auto const avail = sym.find("availableObjectFormats");
        if (avail != sym.end() && avail->is_array() && !avail->empty()) {
            bool ok = false;
            for (auto const& fmt : *avail) {
                if (fmt.is_string() && fmt.get<std::string>() == formatKey) {
                    ok = true;
                    break;
                }
            }
            if (!ok) continue;
        }
        chosen = name->get<std::string>();
        break;
    }
    if (chosen.empty()) return {};

    // `typeof` is C23 and needs no signature; the object is `const` with
    // external linkage, so the reference survives the release pipeline.
    return "#include <" + header->get<std::string>() + ">\n"
           "typeof(" + chosen + ") *const dss_runtime_probe = " + chosen + ";\n"
           "int main(void) { return dss_runtime_probe != 0 ? 0 : 1; }\n";
}

}  // namespace

TEST(RuntimeCacheWiring, AHitAndAMissEmitTheSameImage) {
    auto const subject = discoverSubject();
    ASSERT_TRUE(subject.has_value())
        << "no realizing (target, object format) pair was discovered, so this "
           "case would compare two images built with no runtime at all";
    ASSERT_FALSE(subject->units.empty());

    Harness h;
    ASSERT_TRUE(layDown(h)) << "cannot stage the config tree / source";
    ScopedEnv const cfgEnv{"DSS_CONFIG_ROOT", h.treeRoot.string()};
    ScopedEnv const cacheEnv{"DSS_RUNTIME_CACHE_DIR", h.cacheDir.string()};
    ASSERT_TRUE(stagedTreeIsTheOneBeingRead(h))
        << "the driver would read a config tree other than this harness's "
           "staged one";

    // ── The subject must DEMAND the runtime, or the comparison is vacuous ────
    fs::path const descriptor = descriptorDeclaring(
        h.configDir / "shippedLibs", subject->units.front(), subject->formatKey);
    ASSERT_FALSE(descriptor.empty())
        << "no staged descriptor declares '" << subject->units.front()
        << "' for format key '" << subject->formatKey
        << "', so no probe naming that runtime's own surface can be composed";
    std::string const probe =
        runtimeDemandingProbeSource(descriptor, subject->formatKey);
    ASSERT_FALSE(probe.empty())
        << "the descriptor " << descriptor.generic_string()
        << " supplies no (header, symbol) pair available on format key '"
        << subject->formatKey
        << "', so this case could only compile a program that demands nothing "
           "from the runtime — and a program that demands nothing links no "
           "archive member, which makes 'the two images agree' true for a "
           "reason that has nothing to do with the cache.";
    ASSERT_TRUE(writeWhole(h.source, probe))
        << "cannot write the probe source at " << h.source.generic_string();

    // (1) COLD — a MISS by definition: the private cache root is empty.
    CompileOutcome const cold =
        compileOnce(h, subject->spec(), CompileConfig::Release, 0);
    ASSERT_TRUE(cold.artifact.has_value())
        << "the cold build reported no artifact. The probe source is:\n"
        << probe;
    ASSERT_FALSE(cacheArtifacts(h.cacheDir).empty())
        << "the first build against an EMPTY cache root wrote no artifact, so "
           "the second build below would be a second miss and this case would "
           "compare a miss with a miss";
    // Read the bytes NOW. The warm build writes into a different output
    // directory, but reading before the second compile is what makes this a
    // comparison of two INDEPENDENTLY PRODUCED images rather than of one file
    // with itself.
    std::string const coldImage = readWhole(*cold.artifact);
    ASSERT_FALSE(coldImage.empty())
        << "the cold build's artifact is empty or unreadable: "
        << cold.artifact->generic_string();

    // (2) WARM — PROVEN a hit, by the same instrument the control uses. Without
    // this the case would pass over two misses, which asserts only determinism.
    CompileOutcome const warm =
        compileOnce(h, subject->spec(), CompileConfig::Release, 1);
    ASSERT_TRUE(warm.artifact.has_value())
        << "the warm build reported no artifact — the cache HIT produced no "
           "image at all where the MISS produced one. If the reason is an "
           "undefined symbol from the runtime, the hit path did not hand the "
           "link what the miss path did.";
    ASSERT_LT(warm.optimizeRuns, cold.optimizeRuns)
        << "the second identical build ran as many optimize passes as the "
           "first (" << warm.optimizeRuns << " vs " << cold.optimizeRuns
        << "), so the cache did NOT hit and this case compares two misses.";
    std::string const warmImage = readWhole(*warm.artifact);
    ASSERT_FALSE(warmImage.empty())
        << "the warm build's artifact is empty or unreadable: "
        << warm.artifact->generic_string();

    // (3) THE POINT.
    std::size_t       firstDiff = std::string::npos;
    std::size_t const common    = std::min(coldImage.size(), warmImage.size());
    for (std::size_t i = 0; i < common; ++i) {
        if (coldImage[i] != warmImage[i]) { firstDiff = i; break; }
    }
    if (firstDiff == std::string::npos && coldImage.size() != warmImage.size()) {
        firstDiff = common;
    }
    EXPECT_EQ(firstDiff, std::string::npos)
        << "A CACHE HIT AND A CACHE MISS PRODUCED DIFFERENT IMAGES for spec '"
        << subject->spec() << "'.\n"
           "  miss: " << cold.artifact->generic_string() << " ("
        << coldImage.size() << " bytes, " << cold.optimizeRuns
        << " optimize run(s))\n"
           "  hit:  " << warm.artifact->generic_string() << " ("
        << warmImage.size() << " bytes, " << warm.optimizeRuns
        << " optimize run(s))\n"
           "  first differing byte index: " << firstDiff
        << "\nThe runtime archives are resolved and handed to the link on BOTH "
           "paths precisely so this cannot happen. A build whose output depends "
           "on whether this machine's cache happened to be warm is a build that "
           "passes on the developer's second run and fails on CI's first — in "
           "either direction, and with every symptom pointing at the code under "
           "test rather than at the cache.";
}
