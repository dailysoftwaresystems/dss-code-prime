// D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED — invariant
// (C), CROSS-DESCRIPTOR REALIZATION AGREEMENT.
//
// WHAT WAS MEASURED, AND WHY THIS FILE EXISTS AS A SEPARATE GUARD.
//
// Two shipped descriptors may declare ONE symbol name with DIFFERENT `library`
// values, and injection is FIRST-WINS BY NAME (`semantic_analyzer.cpp`,
// `injectedNames`). ✔MEASURED on pe64 with `objdump -p`, over a `DSS_CONFIG_ROOT`
// COPY whose `memory.json` said `msvcrt.dll` while `string.json` said
// `ucrtbase.dll` — one three-line program, three answers, rc=0 and ZERO
// diagnostics on every one of them:
//
//   #include <string.h> then <memory.h>  ->  memcpy imported from ucrtbase.dll
//   #include <memory.h> then <string.h>  ->  memcpy imported from msvcrt.dll
//   no #include, hand-written prototype  ->  memcpy imported from msvcrt.dll
//
// The third is the sharp one. It reaches the platform through a DIFFERENT ORDER
// (`realizeShippedExternSymbols` walks the corpus index's relPath sort, not the
// TU's include closure), so one program can bind two different C runtimes for one
// name depending only on how it spelled the declaration — which is precisely the
// law THE PLATFORM REALIZATION ORACLE states it upholds: "a hand-written
// declaration and an `#include`d one produce a BYTE-IDENTICAL import".
//
// ★★ THE RULE IS AGREEMENT, NOT UNIQUENESS. ✔MEASURED over the shipped corpus (49
// descriptors, 31 with symbol rows, 556 rows, 441 distinct names): 53 names are
// declared by more than one descriptor, and TWELVE of them never co-exist on any
// format — `io.json` is pe-gated and `unistd.json` is elf/macho-gated, and they
// declare `close`/`read`/`write`/… with different libraries AND different
// `linkName`s, entirely correctly. A detector that refused duplicates would refuse
// the shipped configuration on day one. The 41 that DO co-live form 118 (name,
// format) live pairs across four descriptor sets — math+tgmath, memory+string,
// sys/sysctl+unistd, and `windows.json`'s own repeated `CloseHandle` row — and all
// 118 agree on every axis. That is why the discriminator is "rows LIVE ON THE SAME
// FORMAT must agree", and why the check is green on the corpus the day it lands.
//
// THE THREE THINGS THIS FILE PINS, in the order they can rot:
//
//   1. THE REAL CORPUS AGREES, on every (arch x format x dataModel) axis the repo
//      declares — including formats no current target selects, so an inactive arm
//      cannot rot. AND — the half a "no errors" assertion cannot give you — that
//      the sweep actually COMPARED duplicates: `duplicateRealizationsCompared()`
//      carries a floor, because a sweep that quietly stopped seeing co-live rows
//      is byte-for-byte indistinguishable from a clean corpus.
//   2. THE CHECKER CATCHES A REAL DIVERGENCE IN THE REAL CORPUS. A COPY of the
//      shipped tree with ONE key REMOVED from `memory.json` must go red. This is
//      the REMOVE-direction mutant: it models the corpus LOSING agreement, which
//      is the way this defect actually arrives, rather than an author adding an
//      exotic new field.
//   3. THE RULE IS THE RIGHT RULE — synthetic pairs pin each axis, in BOTH
//      ORDERS, and the two ACCEPT cases (byte-identical duplicates; different
//      libraries behind disjoint availability gates) pin that it never refuses
//      what the corpus legitimately ships.

#include "core/types/data_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/named_type_binding.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_registry.hpp"
#include "diagnostic_count.hpp"
#include "ffi/shipped_lib_descriptor.hpp"
#include "ffi/shipped_type_consistency.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;
using dss::test_support::Location;
using dss::test_support::ScratchDir;
namespace fs = std::filesystem;

namespace {

[[nodiscard]] fs::path configRoot() {
    auto const cfg = dss::test::findConfigRoot();
    if (!cfg) {
        ADD_FAILURE() << dss::test::configRootDiagnostic();
        return {};
    }
    return *cfg;
}

// EVERY descriptor under a shippedLibs tree, recursively — never a hand list.
// Sorted, so the FIRST-DECLARATION identity the checker records is the same on
// every host (a `recursive_directory_iterator` order is filesystem-dependent).
[[nodiscard]] std::vector<fs::path> allDescriptors(fs::path const& shippedLibs) {
    std::vector<fs::path> out;
    for (auto const& e : fs::recursive_directory_iterator(shippedLibs)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// One target axis the checker is sensitive to, derived from the shipped
// object-format documents rather than listed here — the enumeration is the point.
struct TargetAxis {
    ObjectFormatKind format = ObjectFormatKind::Unknown;
    DataModel        dm     = DataModel::Lp64;
    LongDoubleFormat ldf    = LongDoubleFormat::None;
    std::string      exampleConfig;
    bool operator<(TargetAxis const& o) const {
        return std::tie(format, dm, ldf) < std::tie(o.format, o.dm, o.ldf);
    }
};

[[nodiscard]] std::vector<TargetAxis> allTargetAxes(fs::path const& root) {
    std::set<TargetAxis> uniq;
    for (auto const& e : fs::directory_iterator(root / "object-formats")) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        std::ifstream in{e.path(), std::ios::binary};
        if (!in) continue;
        nlohmann::json doc = nlohmann::json::parse(in, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) continue;
        TargetAxis ax;
        ax.exampleConfig = e.path().filename().string();
        if (doc.contains("format") && doc.at("format").is_object()
            && doc.at("format").contains("kind")) {
            auto const k = objectFormatKindFromName(
                doc.at("format").at("kind").get<std::string>());
            if (!k) continue;
            ax.format = *k;
        }
        if (doc.contains("dataModel")) {
            auto const dm = dataModelFromName(doc.at("dataModel").get<std::string>());
            if (dm) ax.dm = *dm;
        }
        if (doc.contains("longDoubleFormat")) {
            auto const l = longDoubleFormatFromName(
                doc.at("longDoubleFormat").get<std::string>());
            if (l) ax.ldf = *l;
        }
        uniq.insert(std::move(ax));
    }
    return {uniq.begin(), uniq.end()};
}

[[nodiscard]] std::vector<std::string> allArches(fs::path const& root) {
    std::vector<std::string> out;
    for (auto const& e : fs::directory_iterator(root / "targets")) {
        if (!e.is_regular_file()) continue;
        std::string const stem = e.path().filename().string();
        auto const dot = stem.find(".target.json");
        if (dot == std::string::npos) continue;
        out.push_back(stem.substr(0, dot));
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] std::string firstError(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.severity == DiagnosticSeverity::Error) return d.actual;
    }
    return "<none>";
}

// What ONE sweep of ONE (arch x format x dataModel) produced. `compared` is the
// vacuity witness — see `duplicateRealizationsCompared()`.
struct SweepResult {
    std::size_t checked  = 0;   // descriptors that passed the document gate
    std::size_t compared = 0;   // co-live duplicate rows actually compared
};

// Read every descriptor for ONE target through the SAME reader a compile uses and
// feed them all to ONE checker — the analyzer's own arrangement, with the analyzer's
// own document-availability gate applied first.
//
// NOTE the deliberate asymmetry with `test_shipped_type_consistency.cpp`: that
// sweep EXPECTs every descriptor to read, because a read failure is a different
// invariant it wants surfaced. Here a read failure is tolerated silently and shows
// up instead as a SHORTFALL in `checked`/`compared`, which the caller's floors
// catch — because this file is also run against DELIBERATELY MUTATED corpus copies,
// where insisting every descriptor read cleanly would fire on the mutation itself.
[[nodiscard]] SweepResult sweepOneTarget(fs::path const& shippedLibs,
                                         std::vector<fs::path> const& descriptors,
                                         std::string_view arch, TargetAxis const& ax,
                                         DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    // stdio.json's `vfprintf` spells the ABI alias `va_list`; any consistent
    // stand-in works — the checker compares TypeIds and every descriptor is
    // handed the same binding.
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{"va_list",
                         interner.pointer(interner.primitive(TypeKind::Void))}};
    // (C) needs no language vocabulary: it compares realizations, not type
    // spellings. An empty vocabulary disables (B) only, which is pinned by the
    // sibling sweep.
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{},
                                   ax.format};
    SweepResult out;
    for (auto const& path : descriptors) {
        DiagnosticReporter readRep;   // read health is a different invariant
        auto desc = readShippedLibDescriptor(path, interner, typeReg, readRep,
                                             ax.dm, arch, ax.format, named);
        if (!desc.has_value()) continue;
        if (!objectFormatInAvailabilitySet(desc->availableObjectFormats, ax.format))
            continue;   // the header does not exist here — it declares nothing here
        (void)checker.add(fs::relative(path, shippedLibs).generic_string(), *desc,
                          rep);
        ++out.checked;
    }
    out.compared = checker.duplicateRealizationsCompared();
    return out;
}

} // namespace

// ── 1. THE REAL CORPUS AGREES — EXHAUSTIVELY, AND NOT VACUOUSLY ─────────────

TEST(ShippedRealizationConsistency, EveryDescriptorAgreesOnEveryRealizationPerTarget) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());
    auto const descriptors = allDescriptors(root / "shippedLibs");
    auto const axes        = allTargetAxes(root);
    auto const arches      = allArches(root);
    // Guard the ENUMERATIONS themselves: a glob that silently matched nothing is
    // the single commonest way a sweep like this goes vacuously green.
    ASSERT_GE(descriptors.size(), 30u) << "shippedLibs enumeration collapsed";
    ASSERT_GE(axes.size(), 4u)         << "object-format enumeration collapsed";
    ASSERT_GE(arches.size(), 2u)       << "target enumeration collapsed";

    std::size_t totalCompared = 0;
    for (auto const& ax : axes) {
        for (auto const& arch : arches) {
            SCOPED_TRACE(ax.exampleConfig + " / arch=" + arch);
            DiagnosticReporter rep;
            SweepResult const r = sweepOneTarget(root / "shippedLibs", descriptors,
                                                 arch, ax, rep);
            EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken),
                      0u)
                << "shipped descriptors disagree about a symbol's REALIZATION on "
                   "this target: " << firstError(rep);
            bool const realFormat = ax.format == ObjectFormatKind::Elf
                                 || ax.format == ObjectFormatKind::MachO
                                 || ax.format == ObjectFormatKind::Pe;
            EXPECT_GE(r.checked, realFormat ? 20u : 8u)
                << "only " << r.checked << " descriptors reached the checker — "
                   "the sweep is no longer exhaustive";
            // ★ THE VACUITY WITNESS. On a REAL format the corpus has co-live
            // duplicates by design. ✔MEASURED today: pe compares 38 (math+tgmath
            // 32, memory+string 5, windows.json's own repeated CloseHandle 1) and
            // elf compares 39 (34 + 5) — both read straight off this checker;
            // macho is 41 (34 + 5 + sys/sysctl+unistd 2) by the corpus census. The
            // floor is well under all three so it never churns, but it is far
            // above zero — which is the number a sweep that stopped comparing
            // reports while still saying "no conflicts". The reserved formats
            // (spirv/wasm) are excluded from almost every descriptor, so they
            // legitimately compare nothing and carry no floor.
            if (realFormat) {
                EXPECT_GE(r.compared, 30u)
                    << "invariant (C) compared only " << r.compared
                    << " duplicate rows on a real format — it is passing "
                       "VACUOUSLY, not passing";
            }
            totalCompared += r.compared;
        }
    }
    EXPECT_GE(totalCompared, 100u)
        << "the whole sweep compared only " << totalCompared << " duplicate rows";
}

// ── 2. THE REMOVE-DIRECTION MUTANT, ON THE REAL CORPUS ──────────────────────
//
// The way this defect ARRIVES is a corpus that LOSES agreement, so the mutant
// REMOVES rather than adds: one key deleted from `memory.json`'s library map on a
// COPY of the shipped tree. `string.json` still names an image for pe and
// `memory.json` no longer does — which is a genuine ambiguity, because
// `realizeShippedExternSymbols` treats "available here, no image here" as a usable
// answer and stops on it, so path order alone would decide between importing
// `memcpy` from a real DLL and routing it unbound to the link tier.
//
// ⚠ It copies the tree; it never touches `src/dss-config`.

namespace {

// Copy the shipped corpus into scratch and hand back the copy's shippedLibs dir.
[[nodiscard]] fs::path copyCorpus(ScratchDir const& dir, fs::path const& root) {
    fs::path const dst = dir.path() / "shippedLibs";
    fs::copy(root / "shippedLibs", dst,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    return dst;
}

// Sweep a corpus for ONE format at LP64/x86_64 and answer how many realization
// conflicts it produced, plus how many duplicates were compared (so a "0 errors"
// answer can be told apart from "0 comparisons").
struct MutantOutcome {
    std::size_t conflicts = 0;
    std::size_t compared  = 0;
};

[[nodiscard]] MutantOutcome sweepFormat(fs::path const& shippedLibs,
                                        ObjectFormatKind fmt) {
    TargetAxis ax;
    ax.format = fmt;
    ax.dm     = fmt == ObjectFormatKind::Pe ? DataModel::Llp64 : DataModel::Lp64;
    DiagnosticReporter rep;
    SweepResult const r =
        sweepOneTarget(shippedLibs, allDescriptors(shippedLibs), "x86_64", ax, rep);
    return {countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken),
            r.compared};
}

// REMOVE one key from one descriptor's `library` map, in a corpus copy.
void removeLibraryKey(fs::path const& descriptor, char const* formatKey) {
    nlohmann::json doc;
    {
        std::ifstream in{descriptor, std::ios::binary};
        ASSERT_TRUE(in.good()) << descriptor.generic_string();
        doc = nlohmann::json::parse(in, nullptr, false);
    }
    ASSERT_FALSE(doc.is_discarded());
    ASSERT_TRUE(doc.contains("library") && doc.at("library").contains(formatKey))
        << descriptor.filename().generic_string() << " no longer carries the '"
        << formatKey << "' key this mutant removes — re-point the mutation at a "
           "descriptor that still does, do not delete the test";
    doc.at("library").erase(formatKey);
    std::ofstream out{descriptor, std::ios::binary | std::ios::trunc};
    out << doc.dump(2);
    ASSERT_TRUE(out.good()) << "the mutation did not reach disk";
}

} // namespace

// ⚠⚠ THE CLEAN ARM AND THE MUTANT ARM GET THEIR OWN CORPUS COPIES AT DIFFERENT
// PATHS. ✔MEASURED the expensive way, and it is the reason this file is shaped
// like this: sweeping ONE copy, mutating it in place, and sweeping again returned
// BYTE-IDENTICAL numbers (checked=32 compared=38 conflicts=0 both times) because
// the second sweep was served the PRE-mutation document out of
// `cachedDescriptorJson`, a thread-local parse cache that was keyed on PATH with
// no staleness check at all.
//
// ★ THAT CACHE IS NOW CONTENT-VALIDATED
// (D-FFI-SHIPPED-DESCRIPTOR-PARSE-CACHE-SERVES-A-STALE-DOCUMENT, P42 lane N): it
// re-reads the file on every lookup and serves the parsed document only when the
// bytes are byte-identical, so an in-place mutation is seen. The two copies stay
// because two ARMS deserve two fixtures — but the rule they used to encode is now
// enforced by the reader rather than remembered by each test author, and
// `tests/ffi/test_shipped_descriptor_cache_staleness.cpp` pins it.
// ⚠ ONE SIBLING MEMO IS STILL PATH-KEYED AND NOT CONTENT-VALIDATED: `corpusIndex()`
// memoizes the whole (name -> rows) index per RESOLVED ROOT, process-wide. Any
// test that mutates a corpus and re-asks through `realizeShippedExternSymbols`
// therefore still needs a DISTINCT ROOT per variant — see the ⚠ in
// `test_shipped_realization_oracle.cpp`.
TEST(ShippedRealizationConsistency, RemovingOneLibraryKeyFromTheRealCorpusIsCaught) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());

    // CONTROL FIRST, on its own copy: untouched, clean, AND non-vacuous. Without
    // it the red below could be an artifact of copying rather than of mutating.
    ScratchDir cleanDir{Location::Temp, "shipped-realization-clean"};
    fs::path const cleanCorpus = copyCorpus(cleanDir, root);
    MutantOutcome const clean = sweepFormat(cleanCorpus, ObjectFormatKind::Pe);
    EXPECT_EQ(clean.conflicts, 0u) << "the unmutated corpus copy is not clean";
    ASSERT_GE(clean.compared, 5u)
        << "the copy compared almost nothing — the mutation below would prove "
           "nothing";

    ScratchDir mutantDir{Location::Temp, "shipped-realization-mutant"};
    fs::path const mutantCorpus = copyCorpus(mutantDir, root);
    ASSERT_NE(cleanCorpus, mutantCorpus) << "both arms landed on one path — the "
                                            "parse cache will hide the mutation";
    removeLibraryKey(mutantCorpus / "memory.json", "pe");
    if (::testing::Test::HasFatalFailure()) return;

    MutantOutcome const mutated = sweepFormat(mutantCorpus, ObjectFormatKind::Pe);
    EXPECT_GT(mutated.compared, 0u) << "the mutated sweep compared nothing";
    EXPECT_GT(mutated.conflicts, 0u)
        << "removing memory.json's pe library entry left string.json naming an "
           "image for the same five mem* names and memory.json naming none, and "
           "the checker said nothing — invariant (C) is not wired in";
    // And the elf arm is UNAFFECTED: the rule is per-format, so a pe-only
    // divergence must not red a format it cannot reach. This is the half that
    // keeps the check from degenerating into "duplicates are forbidden".
    EXPECT_EQ(sweepFormat(mutantCorpus, ObjectFormatKind::Elf).conflicts, 0u)
        << "a pe-only divergence was reported on elf — the per-format "
           "discriminator is broken";
}

// ── 3. THE RULE IS THE RIGHT RULE — synthetic pairs, both orders ────────────

namespace {

[[nodiscard]] fs::path writeDesc(ScratchDir const& dir, std::string const& name,
                                 std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

// Feed two descriptors to one checker in the given order and count (C) errors.
// The ORDER is a parameter because order-dependence is the whole defect: a check
// that only fires one way round would leave the other half of the hazard live.
struct PairOutcome {
    std::size_t conflicts = 0;
    std::size_t compared  = 0;
};

[[nodiscard]] PairOutcome checkPair(fs::path const& a, fs::path const& b,
                                    ObjectFormatKind fmt, DataModel dm) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{}, fmt};
    DiagnosticReporter rep;
    for (fs::path const& p : {a, b}) {
        DiagnosticReporter readRep;
        auto desc = readShippedLibDescriptor(p, interner, typeReg, readRep, dm,
                                             "x86_64", fmt, {});
        EXPECT_TRUE(desc.has_value())
            << p.filename().string() << " failed to read: " << firstError(readRep);
        if (!desc.has_value()) continue;
        if (!objectFormatInAvailabilitySet(desc->availableObjectFormats, fmt))
            continue;
        (void)checker.add(p.filename().generic_string(), *desc, rep);
    }
    return {countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken),
            checker.duplicateRealizationsCompared()};
}

// Assert a synthetic pair is REFUSED in BOTH orders.
void expectRefusedBothOrders(fs::path const& a, fs::path const& b,
                             char const* what,
                             ObjectFormatKind fmt = ObjectFormatKind::Pe,
                             DataModel dm = DataModel::Llp64) {
    for (bool aFirst : {true, false}) {
        SCOPED_TRACE(std::string{what}
                     + (aFirst ? " [a,b]" : " [b,a]"));
        PairOutcome const r = aFirst ? checkPair(a, b, fmt, dm)
                                     : checkPair(b, a, fmt, dm);
        EXPECT_EQ(r.compared, 1u) << "the pair was never compared at all";
        EXPECT_GT(r.conflicts, 0u) << "a divergent " << what << " was accepted";
    }
}

// Assert a synthetic pair is ACCEPTED in both orders, and say whether the two
// rows were even compared — an accept for the wrong reason (never compared) is
// as bad as a wrong refusal.
void expectAcceptedBothOrders(fs::path const& a, fs::path const& b,
                              char const* what, std::size_t expectCompared,
                              ObjectFormatKind fmt = ObjectFormatKind::Pe,
                              DataModel dm = DataModel::Llp64) {
    for (bool aFirst : {true, false}) {
        SCOPED_TRACE(std::string{what} + (aFirst ? " [a,b]" : " [b,a]"));
        PairOutcome const r = aFirst ? checkPair(a, b, fmt, dm)
                                     : checkPair(b, a, fmt, dm);
        EXPECT_EQ(r.conflicts, 0u) << "a legitimate " << what << " was REFUSED";
        EXPECT_EQ(r.compared, expectCompared)
            << "the pair was compared " << r.compared << " times, expected "
            << expectCompared;
    }
}

} // namespace

// The row's own shape: one name, two descriptors, two runtime images, one format.
TEST(ShippedRealizationConsistency, DivergentLibraryForOneFormatIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-lib"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "beta.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    expectRefusedBothOrders(one, two, "library");
}

// The per-SYMBOL override must be MERGED before comparing, or a divergence that
// lives on the override rather than the descriptor map walks straight through.
TEST(ShippedRealizationConsistency, DivergentPerSymbolLibraryOverrideIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-override"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    // Same DESCRIPTOR map, divergence carried ENTIRELY by the symbol override.
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32",
                       "library": { "pe": "beta.dll" } } ]
    })");
    expectRefusedBothOrders(one, two, "per-symbol library override");
}

// "Names an image" vs "names none" — the shape the mutant above builds out of the
// real corpus, in synthetic form so the axis stays pinned even if that descriptor
// changes.
TEST(ShippedRealizationConsistency, ImagePresentVersusAbsentIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-absent"};
    auto const named = writeDesc(dir, "named.json", R"({
        "header": "named.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    // Available on pe (no gate at all), but the map names no pe image.
    auto const bare = writeDesc(dir, "bare.json", R"({
        "header": "bare.h",
        "library": { "elf": "libalpha.so" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    expectRefusedBothOrders(named, bare, "image present vs absent");
}

// The brief's third hazard: same name, same image, DIFFERENT declared signature.
// Dangerous regardless of the library, and invisible to the type-identity
// invariants — nothing in `fn(i32) -> i32` versus `fn(i64) -> i32` is a NAMED
// type, so (A)/(B) walk right past it.
TEST(ShippedRealizationConsistency, DivergentSignatureForOneNameIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-sig"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i64) -> i32" } ]
    })");
    expectRefusedBothOrders(one, two, "signature");
}

// `linkName` decides WHICH EXPORT is asked for. Two descriptors agreeing on the
// image and disagreeing here import two different functions under one C name —
// the `fstat$INODE64` class, which an export check cannot see because BOTH names
// exist in the image.
TEST(ShippedRealizationConsistency, DivergentLinkNameIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-linkname"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32",
                       "linkName": "dup_v2" } ]
    })");
    expectRefusedBothOrders(one, two, "linkName");
}

// One row says "this is data", the other "this is a function": different HIR
// nodes, different lowering. Whoever is injected first decides.
TEST(ShippedRealizationConsistency, DivergentSymbolKindIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-kind"};
    auto const fn = writeDesc(dir, "fn.json", R"({
        "header": "fn.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "i32", "kind": "function" } ]
    })");
    auto const obj = writeDesc(dir, "obj.json", R"({
        "header": "obj.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "i32", "kind": "object" } ]
    })");
    expectRefusedBothOrders(fn, obj, "symbol kind");
}

// ── THE TWO ACCEPTS — what the rule must NEVER refuse ───────────────────────

// The <memory.h>/<string.h> and <math.h>/<tgmath.h> shape: a byte-identical
// duplicate is not merely tolerated, it is RELIED ON. If this ever goes red the
// checker has become "duplicates are forbidden", and the corpus stops building.
TEST(ShippedRealizationConsistency, ByteIdenticalDuplicateIsAccepted) {
    ScratchDir dir{Location::Temp, "shipped-realization-same"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll", "elf": "libalpha.so" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "alpha.dll", "elf": "libalpha.so" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    expectAcceptedBothOrders(one, two, "byte-identical duplicate", 1u);
}

// ★ THE MEASURED SHAPE THE ROW DID NOT NAME, and the reason the rule is
// per-format. `io.json` (pe) and `unistd.json` (elf/macho) declare TWELVE names in
// common with DIFFERENT libraries and DIFFERENT `linkName`s, and they are BOTH
// RIGHT — no compile ever selects both. `compared == 0` is the assertion that
// matters here: not "no conflict was reported" but "these two were never even held
// against each other", which is what makes the accept structural rather than
// lucky.
TEST(ShippedRealizationConsistency, DisjointAvailabilityGatesNeverCollide) {
    ScratchDir dir{Location::Temp, "shipped-realization-disjoint"};
    auto const peSide = writeDesc(dir, "pe_side.json", R"({
        "header": "pe_side.h",
        "availableObjectFormats": ["pe"],
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32",
                       "linkName": "_dup" } ]
    })");
    auto const elfSide = writeDesc(dir, "elf_side.json", R"({
        "header": "elf_side.h",
        "availableObjectFormats": ["elf", "macho"],
        "library": { "elf": "libalpha.so", "macho": "/usr/lib/libalpha.dylib" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    // On pe only one side is live; on elf only the other. Never compared, never
    // refused — on EITHER format, in EITHER order.
    expectAcceptedBothOrders(peSide, elfSide, "disjoint document gates on pe", 0u,
                             ObjectFormatKind::Pe, DataModel::Llp64);
    expectAcceptedBothOrders(peSide, elfSide, "disjoint document gates on elf", 0u,
                             ObjectFormatKind::Elf, DataModel::Lp64);
}

// The SYMBOL-level gate does the same job one tier down — the `threads.json`
// shape, where one name carries three rows whose parameter identity differs BY
// DESIGN because only one of them ever ships on a given format.
TEST(ShippedRealizationConsistency, DisjointPerSymbolGatesNeverCollide) {
    ScratchDir dir{Location::Temp, "shipped-realization-symgate"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll", "elf": "libalpha.so" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32",
                       "availableObjectFormats": ["pe"] } ]
    })");
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "beta.dll", "elf": "libbeta.so" },
        "symbols": [ { "name": "dup", "signature": "fn(i64) -> i64",
                       "availableObjectFormats": ["elf"] } ]
    })");
    expectAcceptedBothOrders(one, two, "disjoint per-symbol gates on pe", 0u,
                             ObjectFormatKind::Pe, DataModel::Llp64);
    expectAcceptedBothOrders(one, two, "disjoint per-symbol gates on elf", 0u,
                             ObjectFormatKind::Elf, DataModel::Lp64);
}

// One descriptor declaring the same name twice with divergent rows is the SAME
// silence — the author sees rows 22 and 39 of a 40-row file no better than they
// see two files. `windows.json` ships a byte-identical repeat of `CloseHandle`
// today, so the corpus stays green; a DIVERGENT repeat must not.
TEST(ShippedRealizationConsistency, DivergentRepeatInsideOneDescriptorIsRefused) {
    ScratchDir dir{Location::Temp, "shipped-realization-intra"};
    auto const solo = writeDesc(dir, "solo.json", R"({
        "header": "solo.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [
            { "name": "dup", "signature": "fn(i32) -> i32" },
            { "name": "other", "signature": "fn(i32) -> i32" },
            { "name": "dup", "signature": "fn(i32) -> i32",
              "library": { "pe": "beta.dll" } }
        ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter readRep;
    auto desc = readShippedLibDescriptor(solo, interner, typeReg, readRep,
                                         DataModel::Llp64, "x86_64",
                                         ObjectFormatKind::Pe, {});
    ASSERT_TRUE(desc.has_value()) << firstError(readRep);
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{},
                                   ObjectFormatKind::Pe};
    DiagnosticReporter rep;
    EXPECT_FALSE(checker.add("solo.json", *desc, rep));
    EXPECT_GT(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken), 0u)
        << "a descriptor declaring one name twice with two images was accepted";
    EXPECT_EQ(checker.duplicateRealizationsCompared(), 1u);
}

// Without an active format there is no `library` entry to select, so (C) must
// state nothing rather than guess — and, crucially, must not report AGREEMENT it
// did not check. The sentinel `Unknown` spells "unknown" and would otherwise be a
// perfectly good map key that matches nothing, making every pair agree trivially.
TEST(ShippedRealizationConsistency, NoActiveFormatComparesNothingRatherThanAgreeing) {
    ScratchDir dir{Location::Temp, "shipped-realization-noformat"};
    auto const one = writeDesc(dir, "one.json", R"({
        "header": "one.h",
        "library": { "pe": "alpha.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    auto const two = writeDesc(dir, "two.json", R"({
        "header": "two.h",
        "library": { "pe": "beta.dll" },
        "symbols": [ { "name": "dup", "signature": "fn(i32) -> i32" } ]
    })");
    for (auto fmt : {std::optional<ObjectFormatKind>{},
                     std::optional<ObjectFormatKind>{ObjectFormatKind::Unknown}}) {
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        ShippedTypeConsistency checker{interner,
                                       std::span<VocabularyCore const>{}, fmt};
        DiagnosticReporter rep;
        for (fs::path const& p : {one, two}) {
            DiagnosticReporter readRep;
            auto desc = readShippedLibDescriptor(p, interner, typeReg, readRep,
                                                 DataModel::Llp64, "x86_64", fmt, {});
            ASSERT_TRUE(desc.has_value()) << firstError(readRep);
            (void)checker.add(p.filename().generic_string(), *desc, rep);
        }
        EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedCorpusInvariantBroken), 0u);
        EXPECT_EQ(checker.duplicateRealizationsCompared(), 0u)
            << "(C) claimed to compare realizations with no selectable format — "
               "that answer would be about the 'unknown' key, not about a target";
    }
}
