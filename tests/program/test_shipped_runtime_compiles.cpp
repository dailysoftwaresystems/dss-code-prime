// D-RUNTIME-DSS-SHIPS-NO-IMPLEMENTATION-HALF — THE SHIPPED-RUNTIME COMPILE GATE.
//
// WHY THIS TEST EXISTS, AND WHY IT IS NOT AN EXTRA
//
// A shipped-header descriptor may declare, per object format, that a symbol's
// body is PROVIDED by a shipped C source unit rather than IMPORTED from a
// platform image (`"realization": {"pe": {"source": "runtime/platform/src/
// dirent.c"}}`). The driver compiles those units as ordinary extra translation
// units — but only for the (target, format) pairs a build actually REACHES.
//
// ★★★ AUTOMATIC COMPILATION ONLY EVER COMPILES WHAT A BUILD REACHES. A declared
// realization that no example exercises stays uncompiled, and its breakage stays
// invisible until a user hits it. A `--compile-runtime` CLI verb was designed to
// close that and was CANCELLED by the operator on the explicit understanding
// that the coverage it bought moves HERE. This file is that replacement in full:
// it is what makes EVERY declared realization exercised, on every host, on every
// run of the suite.
//
// WHAT IT ASSERTS
//
// Every source named by ANY descriptor's `realization` map actually COMPILES for
// the object format that declares it — cross, from whatever host runs the test,
// against the ARCHIVE-writing sibling of that format for each machine the format
// kind reaches, at BOTH build configurations.
//
// ★★ NOTHING HERE IS HAND-MAINTAINED, AND THAT IS THE PROPERTY THAT KEEPS IT
// FROM ROTTING. A list of units would go stale the day a second `.c` lands; a
// list of formats would go stale the day a realization names `elf`. So:
//
//   * the FORMAT KEYS come from the descriptors — every name in the CLOSED
//     `ObjectFormatKind` vocabulary is offered to `allShippedSourcesForFormat`,
//     and a key is "declared" exactly when the corpus answers with a source.
//     No format literal appears anywhere below.
//   * the UNITS come from `allShippedSourcesForFormat` — the SAME corpus reader
//     the driver uses, so this gate and the build can never disagree about what
//     the runtime is.
//   * the MACHINES come from the shipped `object-formats/` + `targets/` trees,
//     paired by `crossValidateTargetFormat` and resolved to their archive
//     sibling by `resolveArchiveSiblingFormat` — the production lookup, not a
//     second spelling of it.
//   * the LANGUAGE comes from the file EXTENSION through the shipped
//     `*.lang.json` `fileExtensions` sets — the same rule the driver's own
//     `resolveShippedSourceGrammar` applies. Zero claimants and two claimants
//     are both refusals, never a guess.
//   * the ARTIFACT NAME comes from `TargetSpec::outputExtension` on the loaded
//     sibling schema. A literal `.lib`/`.a` here would be a second owner of the
//     driver's own naming rule and would silently pass the day it changed.
//
// ★★★ THE DENOMINATOR IS ASSERTED, NOT ASSUMED. "no failures" over ZERO
// discovered units is a vacuous pass, and it is the single most likely way a
// gate like this rots — a broken harvester reports perfect success. So the unit
// count is asserted NON-ZERO, every discovered unit is asserted to have been
// ATTEMPTED, and the attempt/success counts are compared EXACTLY rather than
// merely checked for emptiness. `DiscoveryIsNotAConstant` is the negative
// control that gives the non-zero assertion its meaning: pointed at a descriptor
// tree with no realization the SAME discovery answers zero, so a green above is
// a measurement rather than a hard-coded yes.
//
// ★ EACH ATTEMPT COMPILES INTO ITS OWN FRESH DIRECTORY. That is a fail-closed
// property, not tidiness: the artifact assertions then prove THIS compile wrote
// THIS file, and a stale artifact from a previous attempt (or a previous run)
// cannot stand in for one that was never produced.
//
// WHAT IT DELIBERATELY DOES NOT DO. It does not restate
// `tests/ffi/test_shipped_source_realization.cpp`, which is the DECLARATIVE half
// (R1 every realization names an existing file; R2 every file is named; R3 no
// format declares both an image and a source). Neither half implies the other: a
// perfectly-declared corpus can name a unit that does not compile, and a unit
// that compiles can be declared in a shape that never reaches a build. It also
// pins no unit COUNT and no format NAME — those are the config, and a test that
// restates its subject fails for the wrong reason and gets edited reflexively.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "link/object_format_schema.hpp"
#include "program/cli_args.hpp"
#include "program/compile_pipeline.hpp"
#include "program/cross_validate_target_format.hpp"
#include "program/program.hpp"
#include "program/runtime_object_cache.hpp"
#include "program/target_spec.hpp"

#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

using namespace dss;
using dss::test_support::Location;
using dss::test_support::ScratchDir;

namespace {

// ── The real trees, through the ONE test-side resolver ───────────────────────
// ($DSS_CONFIG_ROOT → the CMake-baked repo root → the cwd ancestor walk). A
// private cwd walk here would find nothing in an out-of-tree build, and a gate
// with no tree to read is a hole rather than a pass.
[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

[[nodiscard]] fs::path descriptorDir()    { return configRoot() / "shippedLibs"; }
[[nodiscard]] fs::path objectFormatsDir() { return configRoot() / "object-formats"; }
[[nodiscard]] fs::path targetsDir()       { return configRoot() / "targets"; }
[[nodiscard]] fs::path languagesDir()     { return configRoot() / "sources"; }

// Every shipped schema NAME in `dir`, i.e. the `loadShipped` key of each
// `<name><suffix>` document, sorted. Sorted for a DETERMINISTIC failure
// message: `directory_iterator` is sorted on NTFS and hash-ordered on ext4, and
// a message whose text depends on the host filesystem is a message nobody can
// pin. The ANSWER never depends on order — every scan below is total.
[[nodiscard]] std::vector<std::string> shippedSchemaNames(fs::path const& dir,
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

[[nodiscard]] std::string lowered(std::string_view s) {
    std::string out{s};
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

[[nodiscard]] std::string joined(std::vector<std::string> const& items) {
    std::string out;
    for (auto const& i : items) {
        if (!out.empty()) out += ", ";
        out += '\'';
        out += i;
        out += '\'';
    }
    return out.empty() ? std::string{"(none)"} : out;
}

// Every ERROR the reporter carries, rendered. A compile failure has to name
// WHAT the front end said, or the gate reports only that something broke.
[[nodiscard]] std::string renderErrors(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) {
        if (d.severity != DiagnosticSeverity::Error) continue;
        out += "\n      [";
        out += diagnosticCodeName(d.code);
        out += "] ";
        out += d.actual;
    }
    return out.empty() ? std::string{"\n      (the reporter carries no Error)"}
                       : out;
}

// ── THE UNIT SET, DERIVED FROM THE DESCRIPTORS ───────────────────────────────
//
// One shipped runtime translation unit, as the descriptor corpus declares it:
// which object-format key realizes it, which config-root-relative source it
// names, and which descriptor(s) said so. The descriptors are carried because
// the failure message must name all three — a message that says only "a runtime
// unit failed" leaves the reader to grep the corpus for who claimed it.
struct DeclaredUnit {
    std::string              formatKey;    // "pe" / "elf" / "macho" — from the corpus
    std::string              source;       // config-root-relative, as declared
    std::vector<std::string> descriptors;  // descriptor-dir-relative paths
};

// Which descriptors name `source` for `formatKey`, via the driver's own
// per-descriptor fast reader. Attribution ONLY — the authoritative unit set
// comes from `allShippedSourcesForFormat` below, so a bug here can never
// shrink the denominator, only leave a failure message poorer (and the
// `EveryDiscoveredUnitIsAttributableToADescriptor` pin refuses even that).
[[nodiscard]] std::vector<std::string> descriptorsNaming(std::string const& formatKey,
                                                         std::string const& source) {
    std::vector<std::string> out;
    std::error_code          ec;
    fs::path const           dir = descriptorDir();
    for (fs::recursive_directory_iterator it{dir, ec}, end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code typeEc;
        if (!it->is_regular_file(typeEc) || typeEc) continue;
        if (it->path().extension() != ".json") continue;
        auto const named = dss::ffi::readShippedSourcesForFormat(it->path(), formatKey);
        if (std::find(named.begin(), named.end(), source) == named.end()) continue;
        out.push_back(it->path().lexically_relative(dir).generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ★★★ THE DENOMINATOR. Every (format key, source) pair the descriptor corpus
// declares, discovered by offering EVERY name in the closed `ObjectFormatKind`
// vocabulary to the corpus reader and keeping the ones the corpus answers.
//
// ⓘ Offering the whole vocabulary is what makes this TOTAL rather than a scan
// that could miss a shape: the descriptor loader validates a `realization` key
// against exactly this vocabulary (`objectFormatKindFromName`), so a key the
// corpus can legally carry is a key asked here. The `Unknown` sentinel is
// excluded through `isSelectableObjectFormatKind` — it spells "unknown" and
// would otherwise be offered as if it named a format.
[[nodiscard]] std::vector<DeclaredUnit> discoverUnits(fs::path const& fromDescriptorDir) {
    std::vector<DeclaredUnit> units;
    for (auto const& row : kObjectFormatKindTable.rows) {
        if (!isSelectableObjectFormatKind(row.first)) continue;
        std::string const key{row.second};
        for (auto& source : dss::ffi::allShippedSourcesForFormat(fromDescriptorDir, key))
            units.push_back(DeclaredUnit{key, std::move(source), {}});
    }
    return units;
}

// ── THE MACHINES A FORMAT KIND REACHES ───────────────────────────────────────
//
// One (target, archive-writing format) pair a runtime unit of this format kind
// must compile for. `buildFormat` is retained only for the failure message —
// it names which build shape led to this sibling.
struct Machine {
    std::string target;       // a `TargetSchema::loadShipped` key
    std::string sibling;      // the archive-writing format, from the production lookup
    std::string buildFormat;  // the format whose sibling this is (message only)
};

// Every distinct (target, archive sibling) a format KIND reaches, discovered
// over the shipped trees.
//
// ★ THE SIBLING IS RESOLVED, NEVER GUESSED. `resolveArchiveSiblingFormat` scans
// every candidate and refuses on 0 or >1 — the property that keeps a first-match
// rule from letting `directory_iterator` order decide the answer (sorted on
// NTFS, hash-ordered on ext4). Asking it here means this gate compiles against
// exactly the format the driver's cache would.
//
// ⚠ A REFUSAL IS A FAILURE, NOT A SKIP. A format kind that declares a
// realization but reaches no archive sibling for some target cannot have its
// runtime compiled at all; silently dropping that pair would shrink the
// denominator to whatever happens to work.
[[nodiscard]] std::vector<Machine> machinesForFormatKey(std::string const& formatKey) {
    std::vector<Machine> machines;
    auto const           targetNames = shippedSchemaNames(targetsDir(), ".target.json");
    if (targetNames.empty()) {
        ADD_FAILURE() << "no shipped targets found under "
                      << targetsDir().generic_string()
                      << " — with no target to compile FOR, this gate would "
                         "silently attempt nothing";
        return machines;
    }
    for (auto const& formatName :
         shippedSchemaNames(objectFormatsDir(), ".format.json")) {
        auto const format = ObjectFormatSchema::loadShipped(formatName);
        if (!format.has_value()) {
            // A document that cannot be read is a refusal, never a skip: it
            // could have been the one that reached a machine nothing else does,
            // so skipping it manufactures a smaller denominator out of a broken
            // input.
            ADD_FAILURE() << "shipped object format '" << formatName
                          << "' failed to load, so the set of machines format "
                             "kind '" << formatKey << "' reaches cannot be proven "
                             "complete";
            continue;
        }
        if (objectFormatKindName((*format)->kind()) != formatKey) continue;
        for (auto const& targetName : targetNames) {
            auto const target = TargetSchema::loadShipped(targetName);
            if (!target.has_value()) {
                ADD_FAILURE() << "shipped target '" << targetName
                              << "' failed to load";
                continue;
            }
            // ⚠ THE REPORTER IS LOCAL AND THROWN AWAY. A non-matching (target,
            // format) pair is the ORDINARY case here — this loop is a cross
            // product — so its machine-mismatch diagnostic is not an event.
            DiagnosticReporter pairing;
            if (!crossValidateTargetFormat(**target, **format, pairing)) continue;

            auto const sibling =
                dss::runtime::resolveArchiveSiblingFormat(
                    **format, **target, objectFormatsDir(),
                    dss::runtime::kRuntimeCacheSiblingRequester);
            if (!sibling.has_value()) {
                ADD_FAILURE()
                    << "SHIPPED RUNTIME COMPILE GATE: object format '" << formatName
                    << "' (kind '" << formatKey << "') agrees with target '"
                    << targetName << "', but its archive-writing sibling could "
                       "not be resolved, so a shipped runtime unit declared for "
                       "this format kind cannot be compiled for this machine at "
                       "all: " << sibling.error();
                continue;
            }
            auto const already =
                std::find_if(machines.begin(), machines.end(), [&](Machine const& m) {
                    return m.target == targetName && m.sibling == *sibling;
                });
            if (already == machines.end())
                machines.push_back(Machine{targetName, *sibling, formatName});
        }
    }
    return machines;
}

// Memoized per format key. The scan above loads every shipped object-format
// document and every shipped target; at one unit that is invisible, but the
// whole point of this gate is that the unit corpus GROWS, and an N-unit corpus
// would otherwise re-load the same 24 documents N times.
[[nodiscard]] std::vector<Machine> const& machinesFor(std::string const& formatKey) {
    static std::map<std::string, std::vector<Machine>> cache;
    auto const it = cache.find(formatKey);
    if (it != cache.end()) return it->second;
    return cache.emplace(formatKey, machinesForFormatKey(formatKey)).first->second;
}

// ── THE FRONT END, FROM THE EXTENSION ────────────────────────────────────────
//
// The shipped language whose `fileExtensions` claims `path`'s extension. This
// is the driver's own rule (`resolveShippedSourceGrammar`), re-derived from the
// same config rather than restated as a language literal: `.c` is claimed by
// exactly one shipped language, and naming that language here would be a second
// owner of a fact `sources/*.lang.json` already holds.
//
// ⚠ ZERO and TWO claimants are BOTH refusals. Two is not hypothetical — `.s`/
// `.S` is claimed by both shipped assembly dialects, so a hand-written assembly
// runtime unit is genuinely ambiguous by extension and needs the ARCH to
// disambiguate. Refusing is what the driver does; guessing here would let this
// gate report a green the driver could never produce.
[[nodiscard]] std::optional<std::string> languageClaiming(fs::path const& path) {
    std::string const        ext = lowered(path.extension().generic_string());
    std::vector<std::string> claimants;
    for (auto const& name : shippedSchemaNames(languagesDir(), ".lang.json")) {
        auto const grammar = GrammarSchema::loadShipped(name);
        if (!grammar.has_value()) continue;   // health is the loader's own business
        for (auto const& declared : (*grammar)->fileExtensions()) {
            if (lowered(declared) != ext) continue;
            claimants.push_back(name);
            break;
        }
    }
    if (claimants.size() == 1u) return claimants.front();
    ADD_FAILURE() << (claimants.empty() ? "NO" : std::to_string(claimants.size()))
                  << " shipped language(s) claim the extension '" << ext
                  << "' of shipped runtime unit '" << path.generic_string()
                  << "' — the extension alone cannot name a front end, so the "
                     "driver refuses this unit too. Claimants: "
                  << joined(claimants);
    return std::nullopt;
}

// ── ONE COMPILE ATTEMPT ──────────────────────────────────────────────────────

// Every build configuration a shipped runtime is compiled at. The runtime
// object cache keys on this (`RuntimeObjectRequest::configName`), so a runtime
// unit that compiles at one and not the other is a real, shippable hole — and
// this repo has shipped release-only miscompiles before. The closed enum is the
// vocabulary, not a hand-kept list.
constexpr CompileConfig kConfigs[] = {CompileConfig::Debug, CompileConfig::Release};

// The failure message's SUBJECT LINE. Every message below opens with it, so
// each one names the DESCRIPTOR, the FORMAT and the SOURCE PATH — the three
// facts a reader needs to find the thing that broke.
[[nodiscard]] std::string subject(DeclaredUnit const& unit, Machine const& machine,
                                  CompileConfig config, std::size_t index,
                                  std::size_t total) {
    return "SHIPPED RUNTIME COMPILE GATE [unit " + std::to_string(index + 1) + " of "
         + std::to_string(total) + "]: descriptor " + joined(unit.descriptors)
         + " declares realization." + unit.formatKey + ".source = '" + unit.source
         + "'; compiling it for target '" + machine.target
         + "' against archive format '" + machine.sibling + "' (the archive sibling of '"
         + machine.buildFormat + "') at --config=" + std::string{compileConfigName(config)}
         + ": ";
}

}  // namespace

// ═══ THE GATE ════════════════════════════════════════════════════════════════

TEST(ShippedRuntimeCompiles, EveryDeclaredRealizationSourceCompilesForItsFormat) {
    ASSERT_FALSE(configRoot().empty());
    ASSERT_TRUE(fs::is_directory(descriptorDir()))
        << "the shipped descriptor corpus is the SUBJECT of this gate; without "
           "it every scan below would silently pass over nothing: "
        << descriptorDir().generic_string();

    // ★★ TWO RESOLVERS, ONE TREE — asserted, because the gate reads the corpus
    // through the TEST-side resolver and resolves each unit's file through the
    // ENGINE's own (`resolveShippedSourcePath`). Under `ctest` both consult
    // $DSS_CONFIG_ROOT and agree; run as a BARE `.exe` the engine walks the cwd
    // instead, and a disagreement would have this gate discover units from one
    // tree and compile sources from another — a green that measured a
    // combination nobody ships. This repo has paid for that cwd-walk before,
    // which is why it is a refusal rather than a note.
    auto const engineRoot = dss::ffi::findShippedConfigRootDir();
    ASSERT_TRUE(engineRoot.has_value())
        << "the engine cannot locate a shipped config root, so no shipped "
           "runtime source can be resolved at all";
    ASSERT_EQ(fs::weakly_canonical(*engineRoot), fs::weakly_canonical(configRoot()))
        << "the engine resolves the shipped config root to '"
        << engineRoot->generic_string() << "' while this test reads the corpus at '"
        << configRoot().generic_string()
        << "'. Run through `ctest` (which sets DSS_CONFIG_ROOT); a bare `.exe` "
           "walks the cwd and can pick up a different tree.";

    auto units = discoverUnits(descriptorDir());
    for (auto& unit : units)
        unit.descriptors = descriptorsNaming(unit.formatKey, unit.source);

    // ★★★ THE DENOMINATOR, ASSERTED FIRST. Every assertion after this is a
    // statement about N units; at N == 0 they are all vacuously true and this
    // gate would report perfect success having compiled nothing — which is
    // precisely the coverage-that-does-not-exist the cancelled `--compile-runtime`
    // verb was replaced to avoid.
    ASSERT_FALSE(units.empty())
        << "discovered ZERO shipped runtime units across the whole "
           "ObjectFormatKind vocabulary under "
        << descriptorDir().generic_string()
        << ". Either the corpus declares no 'realization' at all (in which case "
           "this gate covers nothing and must not report success), or the "
           "discovery is broken. `DiscoveryIsNotAConstant` proves the discovery "
           "can answer zero, so this assertion is a measurement.";

    ScratchDir scratch{Location::InsideRepo, "shipped-runtime-compiles"};

    std::size_t attemptedUnits = 0;
    std::size_t attempts       = 0;
    std::size_t succeeded      = 0;
    // Names the per-attempt output directory. A SEPARATE counter from
    // `attempts`, and monotonic across every cell including the ones that bail
    // out: reusing `attempts` would hand two cells the same directory the first
    // time one of them failed before being counted.
    std::size_t cells          = 0;

    for (std::size_t index = 0; index < units.size(); ++index) {
        DeclaredUnit const& unit = units[index];

        auto const& machines = machinesFor(unit.formatKey);
        EXPECT_FALSE(machines.empty())
            << "SHIPPED RUNTIME COMPILE GATE [unit " << (index + 1) << " of "
            << units.size() << "]: descriptor " << joined(unit.descriptors)
            << " declares realization." << unit.formatKey << ".source = '"
            << unit.source
            << "', but format kind '" << unit.formatKey
            << "' reaches NO (target, archive format) pair over the shipped "
               "object-formats and targets trees — so this unit can never be "
               "compiled and would be silently skipped by a weaker gate.";
        if (machines.empty()) continue;

        auto const resolved = dss::ffi::resolveShippedSource(unit.source);
        EXPECT_TRUE(resolved.resolved())
            << "SHIPPED RUNTIME COMPILE GATE [unit " << (index + 1) << " of "
            << units.size() << "]: descriptor " << joined(unit.descriptors)
            << " declares realization." << unit.formatKey << ".source = '"
            << unit.source << "', but "
            << dss::ffi::describeShippedSourceLookup(resolved, unit.source)
            << " — the format would carry a DECLARED symbol with no body.";
        if (!resolved.resolved()) continue;

        auto const language = languageClaiming(resolved.path);
        if (!language) continue;   // languageClaiming already ADD_FAILURE'd

        bool attemptedThisUnit = false;
        for (auto const& machine : machines) {
            for (auto const config : kConfigs) {
                std::string const spec = machine.target + ":" + machine.sibling;
                std::string const head = subject(unit, machine, config, index,
                                                 units.size());

                // ★ A FRESH, EMPTY DIRECTORY PER ATTEMPT. This is what makes the
                // artifact assertions below mean "THIS compile wrote it" rather
                // than "a file with that name exists" — a leftover from an
                // earlier attempt or an earlier run cannot stand in for an
                // artifact that was never produced.
                fs::path const outDir =
                    scratch.path() / ("cell-" + std::to_string(cells++));
                std::error_code mkEc;
                fs::create_directories(outDir, mkEc);
                EXPECT_FALSE(mkEc) << head << "could not create the per-attempt "
                                      "output directory "
                                   << outDir.generic_string() << ": "
                                   << mkEc.message();
                if (mkEc) continue;

                ++attempts;
                attemptedThisUnit = true;

                Program program;
                program.setOutputDir(outDir);
                program.setCompileConfig(config);
                DiagnosticReporter rep;
                int const rc = program.compileFiles(
                    std::vector<std::string>{resolved.path.string()}, *language,
                    std::vector<std::string>{spec}, rep);

                bool const compiled = (rc == 0) && !rep.hasErrors();
                EXPECT_TRUE(compiled)
                    << head << "THE COMPILE FAILED (rc=" << rc << ", "
                    << rep.errorCount()
                    << " error diagnostic(s)). A shipped runtime unit that does "
                       "not compile is a body the linker will never see: with "
                       "[[D-LINK-EXEC-UNDEFINED-SYMBOL-FAIL-LOUD]] still open an "
                       "undefined EXEC symbol links rc=0 and dies at run time, so "
                       "this ships as a successful build of a program that cannot "
                       "run." << renderErrors(rep);
                if (!compiled) continue;

                // The artifact NAME is derived from the loaded sibling schema,
                // never spelled here — the driver owns that rule and a literal
                // would pass silently the day it changed.
                //
                // ⚠ EVERY FAILURE BELOW IS AN `EXPECT` + `continue`, NEVER AN
                // `ASSERT`. A gtest ASSERT returns from the enclosing function,
                // so one bad unit would cancel every remaining unit, machine and
                // configuration — the run would report ONE failure where there
                // are five, and the units after it would look untested rather
                // than broken. Each cell stays independent.
                auto const parsed = TargetSpec::parse(spec);
                EXPECT_TRUE(parsed.has_value())
                    << head << "the target spec this gate composed does not parse";
                if (!parsed) continue;
                auto const siblingSchema =
                    ObjectFormatSchema::loadShipped(machine.sibling);
                EXPECT_TRUE(siblingSchema.has_value())
                    << head << "the archive sibling schema failed to load";
                if (!siblingSchema) continue;
                fs::path const artifact =
                    outDir / (resolved.path.stem().generic_string()
                              + std::string{parsed->outputExtension(**siblingSchema)});

                std::error_code statEc;
                bool const wrote = fs::is_regular_file(artifact, statEc);
                EXPECT_TRUE(wrote)
                    << head << "the compile reported SUCCESS but wrote no "
                       "artifact at " << artifact.generic_string()
                    << " — a rc=0 that produces nothing is the worst shape this "
                       "gate can accept, because it reports coverage that does "
                       "not exist";
                if (!wrote) continue;
                EXPECT_GT(fs::file_size(artifact, statEc), 0u)
                    << head << "the artifact " << artifact.generic_string()
                    << " is EMPTY";
                // STRONGER THAN NON-EMPTY, and provable: the sibling was selected
                // BY `container() == Archive`, so its output must carry the `ar`
                // global magic. The check is by MAGIC BYTES through the driver's
                // own predicate — the same one `--resolve-library` dispatches on
                // — never by extension.
                EXPECT_TRUE(isArArchiveFile(artifact))
                    << head << "the artifact " << artifact.generic_string()
                    << " is not an `ar` archive, although the format it was "
                       "compiled against was selected precisely because it "
                       "declares container 'archive' — so the bytes on disk are "
                       "not the shape the linker's static-archive path will pull "
                       "members from";
                ++succeeded;
            }
        }
        if (attemptedThisUnit) ++attemptedUnits;
    }

    // ★★ EXACT COUNTS, NOT "no failures". Each of these three can fail while the
    // per-attempt expectations above are all green, and each names a different
    // way this gate could quietly stop covering what it claims.
    EXPECT_EQ(attemptedUnits, units.size())
        << "only " << attemptedUnits << " of " << units.size()
        << " discovered shipped runtime unit(s) were ATTEMPTED. A unit that is "
           "discovered and then skipped is coverage this gate reports and does "
           "not have.";
    EXPECT_GT(attempts, 0u)
        << units.size() << " unit(s) were discovered but ZERO compiles ran";
    EXPECT_EQ(succeeded, attempts)
        << succeeded << " of " << attempts
        << " compile attempt(s) succeeded, over " << units.size()
        << " discovered unit(s) x their (target, archive format) pairs x "
        << std::size(kConfigs) << " build configuration(s).";

    // ★ THE DENOMINATOR IS REPORTED ON SUCCESS, NOT ONLY ON FAILURE. A gate
    // whose coverage is visible only when it breaks is a gate whose coverage
    // silently shrinking to one unit — or to none — looks exactly like a healthy
    // green. These land in ctest's XML (`RecordProperty`) and in `ctest -V`.
    RecordProperty("discovered_units", static_cast<int>(units.size()));
    RecordProperty("attempted_units", static_cast<int>(attemptedUnits));
    RecordProperty("compile_attempts", static_cast<int>(attempts));
    RecordProperty("compiles_succeeded", static_cast<int>(succeeded));
    std::cout << "[ SHIPPED RUNTIME COMPILE GATE ] " << units.size()
              << " declared unit(s), " << attemptedUnits << " attempted, "
              << succeeded << '/' << attempts << " compile(s) succeeded:\n";
    for (auto const& unit : units)
        std::cout << "    realization." << unit.formatKey << ".source = '"
                  << unit.source << "' declared by " << joined(unit.descriptors)
                  << '\n';
}

// ── THE NEGATIVE CONTROL ─────────────────────────────────────────────────────
//
// The gate above asserts it discovered MORE THAN ZERO units. That assertion
// carries information only if the discovery is capable of answering zero — a
// harvester that returned a constant non-empty list would satisfy it forever
// while covering nothing. So the SAME discovery is run against a descriptor tree
// that declares no realization, and must answer zero.
//
// ★ The tree is not empty — it holds a well-formed descriptor WITHOUT a
// `realization` map. An empty directory would also answer zero and would prove
// only that the walk finds no files; this proves it is the REALIZATION KEY that
// produces a unit, not the mere presence of a descriptor.
TEST(ShippedRuntimeCompiles, DiscoveryIsNotAConstant) {
    ScratchDir scratch{Location::InsideRepo, "shipped-runtime-compiles"};
    fs::path const emptyCorpus = scratch.path() / "no-realization";
    std::error_code ec;
    fs::create_directories(emptyCorpus, ec);
    ASSERT_FALSE(ec) << emptyCorpus.generic_string() << ": " << ec.message();

    {
        std::ofstream out{emptyCorpus / "declares-no-realization.json",
                          std::ios::binary | std::ios::trunc};
        ASSERT_TRUE(out.good());
        out << R"({"header":"probe.h","library":{"pe":"probe.dll"},)"
               R"("symbols":[{"name":"dss_runtime_probe",)"
               R"("signature":"fn() -> i32"}]})";
    }

    auto const units = discoverUnits(emptyCorpus);
    EXPECT_TRUE(units.empty())
        << "the discovery reported " << units.size()
        << " unit(s) over a descriptor tree that declares NO 'realization' — so "
           "the non-zero denominator the gate asserts over the real corpus would "
           "be satisfied by a constant rather than measured, and the gate could "
           "report coverage it does not have.";

    // And the control's own precondition: the SAME discovery over the REAL
    // corpus must answer non-zero. Without this, a discovery that always
    // answered zero would pass the assertion above and this control would be
    // asserting nothing.
    ASSERT_FALSE(configRoot().empty());
    EXPECT_FALSE(discoverUnits(descriptorDir()).empty())
        << "the discovery answers zero over the REAL shipped corpus too, so its "
           "'zero' above carries no information";
}

// ── THE ATTRIBUTION ──────────────────────────────────────────────────────────
//
// Every failure message the gate can emit names the DESCRIPTOR, the FORMAT and
// the SOURCE PATH. The format and the source come from the unit itself and
// cannot go missing; the descriptor is looked up separately, so it can. A gate
// whose message reads "descriptor (none)" sends the reader to grep the corpus
// for who claimed a file — exactly the work the message exists to save.
TEST(ShippedRuntimeCompiles, EveryDiscoveredUnitIsAttributableToADescriptor) {
    ASSERT_FALSE(configRoot().empty());
    auto const units = discoverUnits(descriptorDir());
    ASSERT_FALSE(units.empty())
        << "no units discovered — see the gate's denominator assertion";
    for (auto const& unit : units) {
        auto const owners = descriptorsNaming(unit.formatKey, unit.source);
        EXPECT_FALSE(owners.empty())
            << "shipped runtime unit '" << unit.source << "' is realized on format '"
            << unit.formatKey
            << "' according to the corpus-wide reader, but NO descriptor names it "
               "according to the per-descriptor reader — so a compile failure "
               "for this unit could not name the descriptor that declared it.";
    }
}
