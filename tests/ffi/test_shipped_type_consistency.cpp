// D-LANG-TYPE-IDENTITY-VOCABULARY — the CROSS-DESCRIPTOR consistency invariant.
//
// WHY THIS TEST EXISTS (and why it is EXHAUSTIVE rather than a sample):
//
// Three consecutive fix rounds on the type-identity split all shipped green and
// all shipped BROKEN, and every single failure was the same class — two shipped
// descriptors, authored independently, disagreeing about ONE name:
//
//   * `struct timeval` spelled `{i64, i64}` in `sys/resource.json` and
//     `{i64 "long", i64 "long"}` in `sys/time.json`. Tag injection is FIRST-WINS
//     BY NAME and only the winner gets a field scope, so `#include <sys/time.h>`
//     BEFORE `<sys/resource.h>` made `r.ru_utime.tv_sec` fail `S000D` while the
//     reverse order compiled — an INCLUDE-ORDER-DEPENDENT compile failure.
//   * `ssize_t` tagged `i64 "long"` FLAT while `sys/types.json` ships on `pe`
//     too, where the language mints `long` as I32 — a PHANTOM (I64, "long") pair
//     no source spelling can produce, matching no `_Generic` arm.
//
// The per-file reader structurally CANNOT see either: it reads ONE descriptor at
// a time, and it has no access to the source language's vocabulary. A hand-listed
// sample of tags is exactly how the first two rounds missed these, so this test
// ENUMERATES:
//
//   * every `*.json` under `src/dss-config/shippedLibs/**` (recursive), and
//   * every target axis combination declared by `src/dss-config/object-formats/
//     *.format.json` (object-format kind × data model × long-double axis),
//     crossed with every arch in `src/dss-config/targets/*.target.json`.
//
// and asserts, per target, that all of them agree — through the SAME
// `ffi::ShippedTypeConsistency` verb the semantic analyzer runs at compile time
// (`semantic_analyzer.cpp`, right before descriptor injection). The analyzer's
// copy fires only for descriptors a program actually co-includes; this one fires
// for every PAIR in the tree, including headers no corpus program includes
// together yet.
//
// RED-ON-DISABLE: revert either descriptor fix above and this goes red — see
// `RevertedTimevalTagWouldBeCaught` / `PhantomVocabularyTagIsCaught`, which
// reproduce those exact pre-fix shapes on synthetic descriptors so the guard
// itself is pinned even after the real files are correct.

#include "core/types/data_model.hpp"   // DataModel + LongDoubleFormat vocabularies
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
#include "scratch_dir.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
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
    fs::path here = fs::current_path();
    for (int i = 0; i < 12 && !here.empty(); ++i) {
        fs::path const cand = here / "src" / "dss-config";
        if (fs::exists(cand)) return cand;
        fs::path const parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    return {};
}

// EVERY descriptor under shippedLibs, recursively — never a hand-written list.
[[nodiscard]] std::vector<fs::path> allDescriptors(fs::path const& root) {
    std::vector<fs::path> out;
    for (auto const& e : fs::recursive_directory_iterator(root / "shippedLibs")) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".json") continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());   // deterministic first-declaration order
    return out;
}

// One target axis combination the checker is sensitive to. Two distinct format
// CONFIGS that agree on all three axes are one combination here — the reader and
// the vocabulary resolution read nothing else — so this dedup loses no coverage.
struct TargetAxis {
    ObjectFormatKind  format = ObjectFormatKind::Unknown;
    DataModel         dm     = DataModel::Lp64;
    LongDoubleFormat  ldf    = LongDoubleFormat::None;
    std::string       exampleConfig;   // for the failure message
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

// Every arch the repo ships a target schema for (`x86_64.target.json` → x86_64).
[[nodiscard]] std::vector<std::string> allArches(fs::path const& root) {
    std::vector<std::string> out;
    for (auto const& e : fs::directory_iterator(root / "targets")) {
        if (!e.is_regular_file()) continue;
        std::string stem = e.path().filename().string();
        auto const dot = stem.find(".target.json");
        if (dot == std::string::npos) continue;
        out.push_back(stem.substr(0, dot));
    }
    std::sort(out.begin(), out.end());
    return out;
}

[[nodiscard]] std::shared_ptr<GrammarSchema> loadCSubset(fs::path const& root) {
    std::ifstream in{root / "sources" / "c-subset.lang.json", std::ios::binary};
    if (!in) return nullptr;
    std::string const text{std::istreambuf_iterator<char>{in},
                           std::istreambuf_iterator<char>{}};
    auto r = GrammarSchema::loadFromText(text, "<c-subset>");
    return r.has_value() ? *r : nullptr;
}

// The active language's primitive VOCABULARY, resolved for one target axis —
// exactly what `semantic_analyzer.cpp` hands the checker at compile time.
struct Vocabulary {
    std::vector<std::string>   names;   // stable storage the views point into
    std::vector<TypeKind>      cores;
    [[nodiscard]] std::vector<VocabularyCore> rows() const {
        std::vector<VocabularyCore> out;
        out.reserve(names.size());
        for (std::size_t i = 0; i < names.size(); ++i) {
            out.push_back(VocabularyCore{names[i], cores[i]});
        }
        return out;
    }
};

[[nodiscard]] Vocabulary vocabularyFor(GrammarSchema const& sch, DataModel dm,
                                       LongDoubleFormat ldf) {
    Vocabulary v;
    for (auto const& ts : sch.semantics().typeSpecifiers) {
        if (ts.name.empty()) continue;
        auto const core = ts.resolveCore(dm, ldf);
        if (!core.has_value()) continue;                       // unrealized axis
        if (std::find(v.names.begin(), v.names.end(), ts.name) != v.names.end())
            continue;                                          // already recorded
        v.names.push_back(ts.name);
        v.cores.push_back(*core);
    }
    return v;
}

[[nodiscard]] std::string firstError(DiagnosticReporter const& rep) {
    for (auto const& d : rep.all()) {
        if (d.severity == DiagnosticSeverity::Error) return d.actual;
    }
    return "<none>";
}

// Read every descriptor for ONE (arch × format × dataModel) and feed it to ONE
// checker. Returns how many descriptors were actually CHECKED — the caller
// asserts a floor on it, because a sweep that silently skipped everything (a
// read regression, a moved directory) would otherwise be vacuously green.
std::size_t checkOneTarget(fs::path const& root, GrammarSchema const& sch,
                           std::vector<fs::path> const& descriptors,
                           std::string_view arch, TargetAxis const& ax,
                           DiagnosticReporter& rep) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    // stdio.json's `vfprintf` spells the ABI alias `va_list`; without a binding
    // the read fails loud. Any consistent stand-in works here — the checker only
    // compares TypeIds, and the SAME id is bound for every descriptor.
    std::array<NamedTypeBinding, 1> const named{
        NamedTypeBinding{"va_list",
                         interner.pointer(interner.primitive(TypeKind::Void))}};

    Vocabulary const vocab = vocabularyFor(sch, ax.dm, ax.ldf);
    auto const rows = vocab.rows();
    ShippedTypeConsistency checker{interner, rows, ax.format};

    std::size_t checked = 0;
    for (auto const& path : descriptors) {
        DiagnosticReporter readRep;   // read failures are a DIFFERENT invariant
        auto desc = readShippedLibDescriptor(path, interner, typeReg, readRep,
                                             ax.dm, arch, ax.format, named);
        // A descriptor that does not READ is a different invariant (pinned by
        // test_shipped_lib_descriptor) — but it MUST NOT silently shrink this
        // sweep, so it is surfaced here rather than skipped in silence.
        EXPECT_TRUE(desc.has_value())
            << path.filename().string() << " failed to read on this target: "
            << firstError(readRep);
        if (!desc.has_value()) continue;
        // The per-target availability gate the analyzer applies before injection:
        // a header that does not EXIST on this format declares nothing here.
        if (!objectFormatInAvailabilitySet(desc->availableObjectFormats, ax.format))
            continue;
        (void)checker.add(fs::relative(path, root / "shippedLibs").generic_string(),
                          *desc, rep);
        ++checked;
    }
    return checked;
}

// ── THE EXHAUSTIVE SWEEP ────────────────────────────────────────────────────

TEST(ShippedTypeConsistency, EveryDescriptorAgreesOnEveryTagAndTypedefPerTarget) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty()) << "could not locate src/dss-config above cwd";
    auto const descriptors = allDescriptors(root);
    auto const axes        = allTargetAxes(root);
    auto const arches      = allArches(root);
    auto const schema      = loadCSubset(root);
    ASSERT_TRUE(schema != nullptr) << "c-subset.lang.json failed to load";
    // Guard the ENUMERATION itself: a glob that silently matched nothing would
    // make this whole test vacuously green — the exact failure mode it exists to
    // prevent. The floors are well under today's counts, so they never churn.
    ASSERT_GE(descriptors.size(), 30u) << "shippedLibs enumeration collapsed";
    ASSERT_GE(axes.size(), 4u)         << "object-format enumeration collapsed";
    ASSERT_GE(arches.size(), 2u)       << "target enumeration collapsed";

    for (auto const& ax : axes) {
        for (auto const& arch : arches) {
            SCOPED_TRACE(ax.exampleConfig + " / arch=" + arch);
            DiagnosticReporter rep;
            std::size_t const checked =
                checkOneTarget(root, *schema, descriptors, arch, ax, rep);
            EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedTypeIdentityConflict),
                      0u)
                << "shipped descriptors disagree on this target: " << firstError(rep);
            // Vacuity guard: the availability gate legitimately drops headers
            // that do not exist on a format (windows.json on elf, the POSIX set
            // on pe, and most of the tree on the not-yet-shipped spirv/wasm
            // formats, which almost every descriptor excludes explicitly). The
            // floor is per-format so it stays meaningful on the three REAL
            // formats without going vacuous on the reserved two. If it is ever
            // missed, the sweep stopped sweeping.
            bool const realFormat = ax.format == ObjectFormatKind::Elf
                                 || ax.format == ObjectFormatKind::MachO
                                 || ax.format == ObjectFormatKind::Pe;
            EXPECT_GE(checked, realFormat ? 20u : 8u)
                << "only " << checked << " descriptors were checked on this "
                   "target — the sweep is no longer exhaustive";
        }
    }
}

// ── The guard's own red-on-disable pins ─────────────────────────────────────
//
// The sweep above is green precisely BECAUSE the descriptors were fixed, so on
// its own it cannot show that the checker would have caught them. These two
// rebuild the exact pre-fix shapes on synthetic descriptors.

[[nodiscard]] fs::path writeDesc(ScratchDir const& dir, std::string const& name,
                                 std::string const& content) {
    fs::path const p = dir.path() / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
}

// The `struct timeval` shape: two descriptors, same tag, ONE field's vocabulary
// tag different — including through the INLINE `struct "N" {…}` form that lives
// inside another struct's field text (the form `rusage.ru_utime` uses, and the
// one a `structs`-entry-only check would miss).
TEST(ShippedTypeConsistency, RevertedTimevalTagWouldBeCaught) {
    ScratchDir dir{Location::Temp, "shipped-consistency"};
    auto const tagged = writeDesc(dir, "time.json", R"({
        "header": "sys/time.h",
        "structs": [ { "name": "timeval", "fields": [
            { "name": "tv_sec",  "type": "i64 \"long\"" },
            { "name": "tv_usec", "type": "i64 \"long\"" } ] } ]
    })");
    auto const bare = writeDesc(dir, "resource.json", R"({
        "header": "sys/resource.h",
        "structs": [ { "name": "rusage", "fields": [
            { "name": "ru_utime", "type": "struct \"timeval\" {i64, i64}" } ] } ]
    })");

    for (bool taggedFirst : {true, false}) {
        SCOPED_TRACE(taggedFirst ? "tagged first" : "bare first");
        TypeInterner interner{CompilationUnitId{1}};
        TypeRegistry typeReg;
        DiagnosticReporter rep;
        ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{}};
        for (auto const& p : taggedFirst ? std::vector{tagged, bare}
                                         : std::vector{bare, tagged}) {
            auto d = readShippedLibDescriptor(p, interner, typeReg, rep);
            ASSERT_TRUE(d.has_value());
            (void)checker.add(p.filename().string(), *d, rep);
        }
        // BOTH orders must fail — the pre-fix defect was ORDER-DEPENDENT, which
        // is exactly why a one-order pin would have been vacuous.
        EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedTypeIdentityConflict), 1u)
            << "a divergent second declaration of a tag must FAIL LOUD";
    }

    // ... and the IDENTICAL pair stays clean (the guard is not a blanket reject).
    auto const alsoTagged = writeDesc(dir, "resource_ok.json", R"({
        "header": "sys/resource.h",
        "structs": [ { "name": "rusage", "fields": [
            { "name": "ru_utime",
              "type": "struct \"timeval\" {i64 \"long\", i64 \"long\"}" } ] } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{}};
    for (auto const& p : {tagged, alsoTagged}) {
        auto d = readShippedLibDescriptor(p, interner, typeReg, rep);
        ASSERT_TRUE(d.has_value());
        (void)checker.add(p.filename().string(), *d, rep);
    }
    EXPECT_FALSE(rep.hasErrors()) << firstError(rep);
}

// A typedef NAME declared twice with different types — the same first-wins
// injection hazard in the ORDINARY namespace rather than the tag namespace.
TEST(ShippedTypeConsistency, DivergentTypedefAcrossDescriptorsIsCaught) {
    ScratchDir dir{Location::Temp, "shipped-consistency"};
    auto const a = writeDesc(dir, "a.json", R"({
        "header": "a.h", "typedefs": [ { "name": "off_t", "type": "i64 \"long\"" } ]
    })");
    auto const b = writeDesc(dir, "b.json", R"({
        "header": "b.h", "typedefs": [ { "name": "off_t", "type": "i64" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;
    DiagnosticReporter rep;
    ShippedTypeConsistency checker{interner, std::span<VocabularyCore const>{}};
    for (auto const& p : {a, b}) {
        auto d = readShippedLibDescriptor(p, interner, typeReg, rep);
        ASSERT_TRUE(d.has_value());
        (void)checker.add(p.filename().string(), *d, rep);
    }
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_ShippedTypeIdentityConflict), 1u);
}

// The `ssize_t`-on-LLP64 shape: a vocabulary tag whose width the ACTIVE
// LANGUAGE cannot produce under this data model. Nothing about the descriptor
// alone is wrong — it is wrong RELATIVE to the target, which is why the check
// takes the language vocabulary as data.
TEST(ShippedTypeConsistency, PhantomVocabularyTagIsCaught) {
    ScratchDir dir{Location::Temp, "shipped-consistency"};
    auto const p = writeDesc(dir, "types.json", R"({
        "header": "sys/types.h",
        "typedefs": [ { "name": "ssize_t", "type": "i64 \"long\"" } ]
    })");
    TypeInterner interner{CompilationUnitId{1}};
    TypeRegistry typeReg;

    // LLP64: the language's `long` is I32, so (I64, "long") is unproducible.
    std::vector<VocabularyCore> const llp64{{"long", TypeKind::I32},
                                            {"long long", TypeKind::I64}};
    DiagnosticReporter bad;
    {
        ShippedTypeConsistency checker{interner, llp64};
        auto d = readShippedLibDescriptor(p, interner, typeReg, bad);
        ASSERT_TRUE(d.has_value());
        EXPECT_FALSE(checker.add("sys/types.json", *d, bad));
    }
    EXPECT_EQ(countCode(bad, DiagnosticCode::F_ShippedTypeIdentityConflict), 1u)
        << "an unproducible (representation, identity) pair must FAIL LOUD";

    // LP64: the SAME descriptor text is correct, so the guard stays silent —
    // proving it checks the pair against the target, not the spelling.
    std::vector<VocabularyCore> const lp64{{"long", TypeKind::I64},
                                           {"long long", TypeKind::I64}};
    DiagnosticReporter good;
    {
        ShippedTypeConsistency checker{interner, lp64};
        auto d = readShippedLibDescriptor(p, interner, typeReg, good);
        ASSERT_TRUE(d.has_value());
        EXPECT_TRUE(checker.add("sys/types.json", *d, good));
    }
    EXPECT_FALSE(good.hasErrors()) << firstError(good);

    // A tag the language does NOT declare is opaque — never a conflict (a
    // descriptor may model a type this source language has no word for).
    DiagnosticReporter unknown;
    {
        std::vector<VocabularyCore> const other{{"__wide", TypeKind::I128}};
        ShippedTypeConsistency checker{interner, other};
        auto d = readShippedLibDescriptor(p, interner, typeReg, unknown);
        ASSERT_TRUE(d.has_value());
        EXPECT_TRUE(checker.add("sys/types.json", *d, unknown));
    }
    EXPECT_FALSE(unknown.hasErrors());
}

// The REAL end-to-end shape the analyzer sees: `sys/time.json` and
// `sys/resource.json` read for one target and fed to one checker, in BOTH
// include orders. Red the moment either file's `timeval` diverges again.
TEST(ShippedTypeConsistency, RealSysTimeAndSysResourceAgreeInBothOrders) {
    fs::path const root = configRoot();
    ASSERT_FALSE(root.empty());
    fs::path const timePath = root / "shippedLibs" / "sys" / "time.json";
    fs::path const resPath  = root / "shippedLibs" / "sys" / "resource.json";
    ASSERT_TRUE(fs::exists(timePath)) << timePath.generic_string();
    ASSERT_TRUE(fs::exists(resPath))  << resPath.generic_string();

    for (ObjectFormatKind const fmt : {ObjectFormatKind::Elf,
                                       ObjectFormatKind::MachO}) {
        for (std::string_view const arch : {"x86_64", "arm64"}) {
            for (bool timeFirst : {true, false}) {
                SCOPED_TRACE(std::string{objectFormatKindName(fmt)} + "/" +
                             std::string{arch} +
                             (timeFirst ? " time-first" : " resource-first"));
                TypeInterner interner{CompilationUnitId{1}};
                TypeRegistry typeReg;
                DiagnosticReporter rep;
                ShippedTypeConsistency checker{interner,
                                               std::span<VocabularyCore const>{}};
                for (auto const& p : timeFirst ? std::vector{timePath, resPath}
                                               : std::vector{resPath, timePath}) {
                    auto d = readShippedLibDescriptor(p, interner, typeReg, rep,
                                                      DataModel::Lp64, arch, fmt);
                    ASSERT_TRUE(d.has_value());
                    (void)checker.add(p.filename().string(), *d, rep);
                }
                EXPECT_FALSE(rep.hasErrors()) << firstError(rep);
            }
        }
    }
}

} // namespace
