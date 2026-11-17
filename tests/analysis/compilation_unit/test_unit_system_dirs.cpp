// P69 L2b2 (lane `lm`), ruling 570: A UNIT HAS ITS LANGUAGES' SYSTEM DIRS BY CONSTRUCTION.
//
// A `UnitBuilder` takes the system include directories of every language it compiles
// (`semantics.shippedLibDirs`, through `LanguageSystemDirs`) when that language joins it:
// at a constructor, at `registerSchema`, or at an explicit-schema `addInMemory`. It used
// to be a call every channel had to remember after constructing (`applySystemDirs`,
// deleted): the driver, the editor and the FFI header reader made it, and a unit built
// without it is one the build refuses — every predefined macro whose `requires` claim
// names a shipped header is unbacked (C_UnbackedPredefinedMacro, an unsuppressable Error)
// and every angle include misses. ✔MEASURED 2026-11-13 (Mac, host run
// 20261113-202529-993afa48): 3676 failing cases on that one code across ~135 test
// binaries once a test fails on a driver error it did not ask for; and (host run
// 20261113-211922-9a805016) a mixed unit whose c tree was not its primary took only the
// primary's dirs. A caller whose subject is a unit with no system dirs, or with only its
// own, says so: `clearSystemDirs()`.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/diagnostic_budget.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"    // GrammarSchemaData: a language with no document behind it
#include "core/types/parse_diagnostic.hpp"
#include "diagnostic_count.hpp"
#include "driver_diagnostics_listener.hpp"   // AskedDriverCodes
#include "ffi/shipped_lib_descriptor.hpp"     // shippedDescriptorCacheStats (the corpus work counted)
#include "scratch_dir.hpp"
#include "shipped_schema_or_throw.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using dss::CompilationUnit;
using dss::DiagnosticBudget;
using dss::DiagnosticCode;
using dss::GrammarSchema;
using dss::LanguageSystemDirs;
using dss::UnitBuilder;
using dss::test_support::countCode;

constexpr char const* kAngleInclude = "#include <stddef.h>\nsize_t n;\n";

// The unit a c tree compiled with its language's dirs: no unbacked predefine, the angle
// include resolved to its shipped descriptor.
void expectTheCTreeHasItsDirs(CompilationUnit const& cu) {
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::C_UnbackedPredefinedMacro), 0u)
        << "every predefined macro's `requires` claim is backed by the language's own corpus";
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::F_ShippedHeaderNotFound), 0u)
        << "the angle include resolves on the language's own system dirs";
    EXPECT_FALSE(cu.shippedLibDescriptors().empty())
        << "<stddef.h> reached its shipped descriptor";
}

}  // namespace

TEST(UnitSystemDirs, AUnitHasItsLanguagesSystemDirsByConstruction) {
    UnitBuilder b{dss::test_support::shippedSchemaOrThrow("c"),
                  DiagnosticBudget::libraryDefault()};
    b.addInMemory(kAngleInclude, "<mem>");
    expectTheCTreeHasItsDirs(std::move(b).finish());
}

TEST(UnitSystemDirs, DirsResolvedAheadBuildTheSameUnit) {
    auto const schema = dss::test_support::shippedSchemaOrThrow("c");
    LanguageSystemDirs const dirs{*schema};
    ASSERT_FALSE(dirs.dirs().empty()) << "the c language ships system dirs";
    UnitBuilder b{schema, DiagnosticBudget::libraryDefault(), dirs};
    b.addInMemory(kAngleInclude, "<mem>");
    auto const cu = std::move(b).finish();
    EXPECT_FALSE(cu.driverDiagnostics().hasErrors());
    EXPECT_FALSE(cu.shippedLibDescriptors().empty());
}

// A language that is not the unit's primary brings its dirs too, by every way a language
// joins a unit. The primary here (toy) ships none.
TEST(UnitSystemDirs, EveryLanguageOfAMixedUnitBringsItsDirs) {
    auto const toy = dss::test_support::shippedSchemaOrThrow("toy");
    auto const c   = dss::test_support::shippedSchemaOrThrow("c");
    {
        SCOPED_TRACE("a constructor's second schema");
        UnitBuilder b{std::vector<std::shared_ptr<GrammarSchema const>>{toy, c},
                      DiagnosticBudget::libraryDefault()};
        b.addInMemory(kAngleInclude, "a.c", c);
        expectTheCTreeHasItsDirs(std::move(b).finish());
    }
    {
        SCOPED_TRACE("a registered schema");
        UnitBuilder b{toy, DiagnosticBudget::libraryDefault()};
        b.registerSchema(c);
        b.addInMemory(kAngleInclude, "a.c", c);
        expectTheCTreeHasItsDirs(std::move(b).finish());
    }
    {
        SCOPED_TRACE("an explicit-schema in-memory source");
        UnitBuilder b{toy, DiagnosticBudget::libraryDefault()};
        b.addInMemory(kAngleInclude, "a.c", c);
        expectTheCTreeHasItsDirs(std::move(b).finish());
    }
}

TEST(UnitSystemDirs, AUnitWhoseDirsAreClearedHasNone) {
    // The subject IS a unit with no system dirs: both of its consequences are asked for,
    // and a language that joins it afterwards brings none either.
    dss::test_support::AskedDriverCodes const asked{DiagnosticCode::F_ShippedHeaderNotFound,
                                                    DiagnosticCode::C_UnbackedPredefinedMacro};
    for (bool const joinsAfter : {false, true}) {
        SCOPED_TRACE(joinsAfter ? "c joins after the clear" : "c is the primary");
        auto const c = dss::test_support::shippedSchemaOrThrow("c");
        UnitBuilder b{joinsAfter ? dss::test_support::shippedSchemaOrThrow("toy") : c,
                      DiagnosticBudget::libraryDefault()};
        b.clearSystemDirs();
        if (joinsAfter) b.registerSchema(c);
        b.addInMemory(kAngleInclude, "a.c", c);
        auto const cu = std::move(b).finish();
        EXPECT_GE(countCode(cu.driverDiagnostics(), DiagnosticCode::F_ShippedHeaderNotFound), 1u);
        EXPECT_GE(countCode(cu.driverDiagnostics(), DiagnosticCode::C_UnbackedPredefinedMacro), 1u);
        EXPECT_TRUE(cu.shippedLibDescriptors().empty());
    }
}

// ── A UNIT'S SHIPPED-SURFACE CHECK DOES ITS CORPUS WORK ONCE (P69 L2b2, ruling 602) ──────────────────
// Every unit's `finish` checks each predefined macro's `surface` claim against the shipped corpus
// (`ffi::validateShippedSurfaceRequirements`). ✔MEASURED on MSVC Release: 79.4 ms per check in every
// unit, a mean 0.59 ms per surface harvest (so about 135 harvests per check), 71 lookups of 10
// distinct descriptors — the in-process suite ran 8–32x slower once every test unit had its
// language's dirs (570). The check now reads each descriptor once and resolves its path's identity
// once (a read scope), resolves each (claim header, format) closure once, and a descriptor's surface
// on a format is harvested once per bytes (the cache entry's memo). These pins COUNT that work
// through the cache's own counters, never a clock, and they pin the exactness the memo must keep:
// the NEXT check reads the corpus again.
namespace {

// The (header, format) pairs the language family's GATED `surface` claims name — what one unit's check
// resolves with no active format (the test fixtures' case): an ungated claim has no format to hold on.
std::size_t distinctGatedClaimClosures(GrammarSchema const& c) {
    std::set<std::pair<std::string, std::string>> pairs;
    for (auto const& pm : c.preprocess().predefinedMacros) {
        if (pm.impliedSurface.kind != dss::ImpliedSurfaceKind::Surface) continue;
        for (std::string const& fmt : pm.availableObjectFormats) {
            for (auto const& claim : pm.impliedSurface.headers) pairs.emplace(claim.header, fmt);
        }
    }
    return pairs.size();
}

std::size_t descriptorFilesOn(std::span<std::filesystem::path const> dirs) {
    std::size_t n = 0;
    for (auto const& d : dirs) {
        std::error_code ec;
        for (auto const& e : std::filesystem::recursive_directory_iterator(d, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".json") ++n;
        }
    }
    return n;
}

struct CheckWork {
    std::size_t reads;
    std::size_t harvests;
    std::size_t walks;
};

// The corpus work of one trivial unit's `finish`.
CheckWork workOfOneUnit(std::shared_ptr<GrammarSchema const> const& c) {
    auto const s0 = dss::ffi::shippedDescriptorCacheStats();
    UnitBuilder b{c, DiagnosticBudget::libraryDefault()};
    b.addInMemory("int main(void) { return 0; }\n", "<mem>");
    auto const cu = std::move(b).finish();
    EXPECT_EQ(countCode(cu.driverDiagnostics(), DiagnosticCode::C_UnbackedPredefinedMacro), 0u)
        << "the check's verdict is the corpus's: every claim is backed";
    auto const s1 = dss::ffi::shippedDescriptorCacheStats();
    return CheckWork{(s1.parses - s0.parses) + (s1.revalidatedHits - s0.revalidatedHits),
                     s1.surfaceHarvests - s0.surfaceHarvests, s1.closureWalks - s0.closureWalks};
}

}  // namespace

TEST(UnitShippedSurfaceCheck, AUnitsCheckReadsEachDescriptorOnceAndTheNextCheckReadsAgain) {
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    LanguageSystemDirs const dirs{*c};
    std::size_t const files = descriptorFilesOn(dirs.dirs());
    ASSERT_GT(files, 0u) << "the language's dirs hold the shipped corpus";
    (void)workOfOneUnit(c);   // the thread's first unit: whatever it parses, the next one reads
    CheckWork const second = workOfOneUnit(c);
    EXPECT_GT(second.reads, 0u)
        << "the next check READS the corpus again: a read served across checks would answer a "
           "descriptor rewritten between them from its old bytes";
    EXPECT_LE(second.reads, files)
        << "one check reads each descriptor at most once (" << files << " on the dirs), never once per "
           "claim or per closure member";
}

TEST(UnitShippedSurfaceCheck, ADescriptorsSurfaceOnAFormatIsHarvestedOncePerBytes) {
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    (void)workOfOneUnit(c);
    CheckWork const second = workOfOneUnit(c);
    EXPECT_EQ(second.harvests, 0u)
        << "a unit whose corpus bytes did not change harvests no descriptor's surface again";
}

TEST(UnitShippedSurfaceCheck, ACheckResolvesEachClaimHeaderOncePerFormat) {
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    std::size_t const pairs = distinctGatedClaimClosures(*c);
    ASSERT_GT(pairs, 0u) << "the language declares gated `surface` claims";
    EXPECT_EQ(workOfOneUnit(c).walks, pairs)
        << "one closure walk per distinct (claim header, format), however many claims name it";
}

// The check alone, with no unit around it, so every identity it counts was asked inside its read scope.
TEST(UnitShippedSurfaceCheck, ACheckResolvesEachDescriptorPathOnceForItsOneRead) {
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    LanguageSystemDirs const dirs{*c};
    auto const oneCheck = [&] {
        auto const s0 = dss::ffi::shippedDescriptorCacheStats();
        dss::DiagnosticReporter rep;
        (void)dss::ffi::validateShippedSurfaceRequirements(c->preprocess().predefinedMacros, "c.lang.json",
                                                           dirs.dirs(), std::nullopt, rep);
        EXPECT_EQ(rep.errorCount(), 0u) << "the check's verdict is the corpus's: every claim is backed";
        auto const s1 = dss::ffi::shippedDescriptorCacheStats();
        return std::pair{(s1.parses - s0.parses) + (s1.revalidatedHits - s0.revalidatedHits),
                         s1.identityResolutions - s0.identityResolutions};
    };
    (void)oneCheck();   // the thread's first check: whatever it parses, the next one reads
    auto const [reads, resolutions] = oneCheck();
    ASSERT_GT(reads, 0u) << "the next check reads the corpus again";
    EXPECT_EQ(resolutions, reads)
        << "one check resolves each descriptor path's identity once, for its one read: never once per "
           "claim that names the header, nor per visit of a closure member";
}

// ── ONE SNAPSHOT OF THE SHIPPED CORPUS PER UNIT OPERATION (P69 L2b2, ruling 602) ─────────────────────
// ✔MEASURED before (MSVC Release, warm, per unit): a unit including six headers made 82 descriptor
// lookups outside the check, each a checked read and a path identity; `<stddef.h>` alone 10 reads and 13
// identities of one descriptor. Each operation of a builder (the parse of one source, its finish) now
// reads each descriptor once and resolves each path once, and the next operation reads again. A scratch
// system dir holds a two-descriptor closure (snapouter includes snapinner), so the counts are exact.
namespace {

constexpr char kSnapshotGroup[] = "unit-descriptor-snapshot";

void writeDescriptor(std::filesystem::path const& p, std::string const& text) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
    out.flush();
    if (!out.good()) throw std::runtime_error("could not write the descriptor " + p.generic_string());
}

std::string snapOuter(std::string const& typedefName) {
    return R"({ "header": "snapouter.h", "includes": ["snapinner.h"], "typedefs": [ { "name": ")"
           + typedefName + R"(", "type": "i32" } ] })";
}

constexpr char kSnapInner[] =
    R"({ "header": "snapinner.h", "typedefs": [ { "name": "SnapInner", "type": "i32" } ] })";

struct OperationWork {
    std::size_t reads;
    std::size_t identities;
    std::size_t staleEvictions;
};

OperationWork workSince(dss::ffi::ShippedDescriptorCacheStats const& s0) {
    auto const s1 = dss::ffi::shippedDescriptorCacheStats();
    return OperationWork{(s1.parses - s0.parses) + (s1.revalidatedHits - s0.revalidatedHits),
                         s1.identityResolutions - s0.identityResolutions, s1.staleEvictions - s0.staleEvictions};
}

}  // namespace

TEST(UnitShippedDescriptorSnapshot, AParseReadsEachDescriptorOfItsClosureOnce) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kSnapshotGroup};
    writeDescriptor(dir.path() / "snapouter.json", snapOuter("SnapOuter"));
    writeDescriptor(dir.path() / "snapinner.json", kSnapInner);
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    char const* const source = "#include <snapouter.h>\nSnapOuter o;\nSnapInner i;\n";
    {   // the thread's first parse of these files, and whatever a first use builds: not counted
        UnitBuilder warm{c, DiagnosticBudget::libraryDefault()};
        warm.addSystemDir(dir.path());
        warm.addInMemory(source, "<snapshot-warm>");
    }
    UnitBuilder b{c, DiagnosticBudget::libraryDefault()};
    b.addSystemDir(dir.path());
    auto const s0 = dss::ffi::shippedDescriptorCacheStats();
    b.addInMemory(source, "<snapshot-parse>");
    OperationWork const w = workSince(s0);
    EXPECT_EQ(w.reads, 2u) << "one parse reads each descriptor of its closure once (snapouter, snapinner), "
                              "however many of the preprocessor's readers and the name seed ask for it";
    EXPECT_EQ(w.identities, w.reads) << "one parse resolves each descriptor path once, for its one read";
}

TEST(UnitShippedDescriptorSnapshot, AFinishReadsEachDescriptorOnceForItsCheckAndItsImports) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kSnapshotGroup};
    writeDescriptor(dir.path() / "snapouter.json", snapOuter("SnapOuter"));
    writeDescriptor(dir.path() / "snapinner.json", kSnapInner);
    UnitBuilder b{dss::test_support::shippedSchemaOrThrow("c"), DiagnosticBudget::libraryDefault()};
    b.addSystemDir(dir.path());
    b.addInMemory("#include <snapouter.h>\nSnapOuter o;\nSnapInner i;\n", "<snapshot-finish>");
    auto const s0 = dss::ffi::shippedDescriptorCacheStats();
    auto const cu = std::move(b).finish();
    OperationWork const w = workSince(s0);
    EXPECT_EQ(cu.shippedLibDescriptors().size(), 2u) << "the import resolution records both descriptors";
    ASSERT_GT(w.reads, 0u) << "a finish reads the corpus again: its check and its import resolution";
    EXPECT_EQ(w.identities, w.reads)
        << "one finish resolves each descriptor path once, for its one read: the check and the import "
           "resolution share one snapshot";
}

TEST(UnitShippedDescriptorSnapshot, ABuilderKeptAcrossTwoOperationsReadsADescriptorRewrittenBetweenThem) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kSnapshotGroup};
    writeDescriptor(dir.path() / "snapouter.json", snapOuter("SnapFirst"));
    writeDescriptor(dir.path() / "snapinner.json", kSnapInner);
    UnitBuilder b{dss::test_support::shippedSchemaOrThrow("c"), DiagnosticBudget::libraryDefault()};
    b.addSystemDir(dir.path());
    b.addInMemory("#include <snapouter.h>\nSnapFirst a;\n", "<snapshot-first>");
    writeDescriptor(dir.path() / "snapouter.json", snapOuter("SnapSecond"));
    auto const s0 = dss::ffi::shippedDescriptorCacheStats();
    b.addInMemory("#include <snapouter.h>\nSnapSecond b;\n", "<snapshot-second>");
    OperationWork const w = workSince(s0);
    EXPECT_EQ(w.staleEvictions, 1u)
        << "the second operation reads the rewritten descriptor from its new bytes: a snapshot that outlived "
           "the first operation would answer it from the old ones";
}

// ── THE CONSTANTS VIEW IS KEPT PER WHAT ITS DECODE READ (P69 L2b2, ruling 630) ───────────────────────
// `ffi::readShippedLibConstants`, the preprocessor's read of a descriptor's `constants`, decodes the
// descriptor's typedefs and constants through a function-local lattice at every call, and a unit asks it
// once per descriptor of its closure. Its answer is now kept in the descriptor's cache entry, keyed by
// every input the decode reads, by value; what the decode reads beyond the descriptor's bytes (a typedef's
// owner, the search that found it) is recorded and must still hold; a decode that said anything is never
// kept. These pins COUNT decodes and memo hits through the cache's own counters, over a scratch corpus in
// which each constant reads one key field.
namespace {

constexpr char kConstantsMemoGroup[] = "constants-memo";

// One constant per input: each changes its value (or whether it exists) with that input alone. `cm_size`
// is its OWNER's (cmown.h), so CM_SIZE_MAX reads a second descriptor and the search that found it.
std::string memoDescriptor(int elfFormatValue) {
    return std::string{R"({ "header": "cm.h",
  "typedefs": [ { "name": "cm_size", "shippedTypedef": { "header": "cmown.h" } } ],
  "constants": [
    { "name": "CM_ARCH", "variants": [ { "when": { "arch": "x86_64" }, "type": "i32", "value": 1 },
                                       { "when": { "arch": "arm64" }, "type": "i32", "value": 2 } ] },
    { "name": "CM_FORMAT", "variants": [ { "when": { "format": "elf" }, "type": "i32", "value": )"}
           + std::to_string(elfFormatValue) + R"( },
                                         { "when": { "format": "pe" }, "type": "i32", "value": 20 } ] },
    { "name": "CM_LONG_MAX", "of": "long", "limit": "max" },
    { "name": "CM_CHAR_MAX", "of": "char", "limit": "max" },
    { "name": "CM_WCHAR_MAX", "of": { "abiTypedef": "wchar_t" }, "limit": "max" },
    { "name": "CM_LD", "variants": [ { "when": { "longDoubleFormat": "x87-80" }, "type": "i32", "value": 80 },
                                     { "when": { "longDoubleFormat": "f64" }, "type": "i32", "value": 64 } ] },
    { "name": "CM_SIZE_MAX", "of": "cm_size", "limit": "max" }
  ] })";
}

std::string memoOwner(char const* sizeType) {
    return std::string{R"({ "header": "cmown.h", "typedefs": [ { "name": "cm_size", "type": ")"} + sizeType
           + R"(" } ] })";
}

// A directory holding the descriptor and (unless `withOwner` is false) its typedef's owner.
std::filesystem::path memoCorpus(std::filesystem::path const& under, std::string const& name, bool withOwner = true) {
    std::filesystem::path const dir = under / name;
    std::filesystem::create_directories(dir);
    writeDescriptor(dir / "cm.json", memoDescriptor(1));
    if (withOwner) writeDescriptor(dir / "cmown.json", memoOwner("u32"));
    return dir;
}

// The inputs of one call.
struct MemoCall {
    std::optional<std::string>                   target;
    std::optional<dss::ObjectFormatKind>         format;
    std::optional<dss::ffi::ShippedPairFacts>    pair;
};

// The pair every row of the table varies one field of: LP64, signed char, a 32-bit wchar_t, x87 long double.
MemoCall baseCall(GrammarSchema const& c) {
    return MemoCall{std::string{"x86_64"}, dss::ObjectFormatKind::Elf,
                    dss::ffi::ShippedPairFacts{&c, dss::DataModel::Lp64, false,
                                               {{"wchar_t", dss::TypeKind::I32}}, dss::LongDoubleFormat::X87_80}};
}

std::optional<std::vector<dss::ffi::ShippedPpConstant>>
readConstants(std::filesystem::path const& descriptor, MemoCall const& call, dss::DiagnosticReporter& rep) {
    std::optional<std::string_view> const target =
        call.target.has_value() ? std::optional<std::string_view>{*call.target} : std::nullopt;
    return dss::ffi::readShippedLibConstants(descriptor, rep, target, call.format,
                                             call.pair.has_value() ? &*call.pair : nullptr);
}

// A view as text — every field of every row, so two views compare whole.
std::string render(std::optional<std::vector<dss::ffi::ShippedPpConstant>> const& view) {
    if (!view.has_value()) return "<no view>";
    std::string out;
    for (auto const& k : *view) {
        out += k.name + "=" + std::to_string(k.value) + (k.isUnsigned ? "u" : "s") + std::to_string(k.width) + ":"
               + std::to_string(static_cast<int>(k.core)) + ":" + k.vocabularyName + ";";
    }
    return out;
}

std::optional<std::int64_t> valueOf(std::optional<std::vector<dss::ffi::ShippedPpConstant>> const& view,
                                    std::string const& name) {
    if (!view.has_value()) return std::nullopt;
    for (auto const& k : *view) {
        if (k.name == name) return k.value;
    }
    return std::nullopt;
}

struct MemoWork {
    std::size_t decodes;
    std::size_t hits;
};

MemoWork memoWorkSince(dss::ffi::ShippedDescriptorCacheStats const& s0) {
    auto const s1 = dss::ffi::shippedDescriptorCacheStats();
    return MemoWork{s1.constantsDecodes - s0.constantsDecodes, s1.constantsMemoHits - s0.constantsMemoHits};
}

}  // namespace

TEST(ShippedConstantsMemo, EqualInputsAreDecodedOnceAndServedFromTheMemo) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    std::filesystem::path const descriptor = memoCorpus(dir.path(), "count") / "cm.json";
    // Two calls with EQUAL inputs held in DIFFERENT objects: the key is their values, never their addresses.
    MemoCall const first  = baseCall(*c);
    MemoCall const second = baseCall(*c);
    dss::DiagnosticReporter rep;
    auto const s0  = dss::ffi::shippedDescriptorCacheStats();
    auto const one = readConstants(descriptor, first, rep);
    auto const two = readConstants(descriptor, second, rep);
    MemoWork const w = memoWorkSince(s0);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_TRUE(one.has_value());
    EXPECT_EQ(valueOf(one, "CM_SIZE_MAX"), std::optional<std::int64_t>{4294967295})
        << "the corpus realizes every constant on the base pair, its owner's included";
    EXPECT_EQ(render(two), render(one)) << "the kept view is the decoded one";
    EXPECT_EQ(w.decodes, 1u) << "two calls with equal inputs decode once";
    EXPECT_EQ(w.hits, 1u) << "the second call is served from the descriptor's entry";
}

TEST(ShippedConstantsMemo, EachInputTheDecodeReadsIsAKeyField) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    struct Row {
        char const* field;
        MemoCall    a;
        MemoCall    b;
        char const* constant;   // the constant the two views differ on; nullptr: the views are equal
        bool        respell;    // the B call names the descriptor by a second spelling of one file
    };
    MemoCall const base = baseCall(*c);
    auto const with = [&](auto&& change) {
        MemoCall call = base;
        change(call);
        return call;
    };
    MemoCall emptyPair = base;
    emptyPair.pair = dss::ffi::ShippedPairFacts{};
    MemoCall noPair = base;
    noPair.pair.reset();
    std::vector<Row> const rows{
        {"target", base, with([](MemoCall& m) { m.target = "arm64"; }), "CM_ARCH", false},
        {"format", base, with([](MemoCall& m) { m.format = dss::ObjectFormatKind::Pe; }), "CM_FORMAT", false},
        {"dataModel", base, with([](MemoCall& m) { m.pair->dataModel = dss::DataModel::Llp64; }), "CM_LONG_MAX", false},
        {"charIsUnsigned", base, with([](MemoCall& m) { m.pair->charIsUnsigned = true; }), "CM_CHAR_MAX", false},
        {"abiTypedefs", base, with([](MemoCall& m) { m.pair->abiTypedefs = {{"wchar_t", dss::TypeKind::U16}}; }),
         "CM_WCHAR_MAX", false},
        {"longDoubleFormat", base, with([](MemoCall& m) { m.pair->longDoubleFormat = dss::LongDoubleFormat::F64; }),
         "CM_LD", false},
        {"language", base, with([](MemoCall& m) { m.pair->language = nullptr; }), "CM_LONG_MAX", false},
        {"pair given", emptyPair, noPair, nullptr, false},
        {"spelling", base, base, nullptr, true},
    };
    for (Row const& row : rows) {
        SCOPED_TRACE(row.field);
        std::filesystem::path const corpus = memoCorpus(dir.path(), row.field);
        std::filesystem::path const a      = corpus / "cm.json";
        std::filesystem::path const b      = row.respell ? corpus / "." / "cm.json" : a;
        dss::DiagnosticReporter rep;
        auto const s0     = dss::ffi::shippedDescriptorCacheStats();
        auto const viewA  = readConstants(a, row.a, rep);
        auto const viewB  = readConstants(b, row.b, rep);
        auto const againA = readConstants(a, row.a, rep);
        auto const againB = readConstants(b, row.b, rep);
        MemoWork const w = memoWorkSince(s0);
        EXPECT_EQ(rep.errorCount(), 0u);
        ASSERT_TRUE(viewA.has_value() && viewB.has_value());
        if (row.constant != nullptr) {
            EXPECT_NE(valueOf(viewA, row.constant), valueOf(viewB, row.constant))
                << "two calls differing only in this input read different views";
        } else {
            EXPECT_EQ(render(viewA), render(viewB)) << "this input is read, and the views it gives agree";
        }
        EXPECT_EQ(w.decodes, 2u) << "each value of this input is its own kept view: one decode each";
        EXPECT_EQ(w.hits, 2u) << "a call repeating either value is served from its own kept view";
        EXPECT_EQ(render(againA), render(viewA));
        EXPECT_EQ(render(againB), render(viewB));
    }
}

TEST(ShippedConstantsMemo, AKeptViewIsDroppedWithItsDescriptorsBytes) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    std::filesystem::path const descriptor = memoCorpus(dir.path(), "rewrite") / "cm.json";
    MemoCall const call = baseCall(*c);
    dss::DiagnosticReporter rep;
    EXPECT_EQ(valueOf(readConstants(descriptor, call, rep), "CM_FORMAT"), std::optional<std::int64_t>{1});
    writeDescriptor(descriptor, memoDescriptor(7));
    auto const s0 = dss::ffi::shippedDescriptorCacheStats();
    auto const rewritten = readConstants(descriptor, call, rep);
    MemoWork const w = memoWorkSince(s0);
    EXPECT_EQ(dss::ffi::shippedDescriptorCacheStats().staleEvictions - s0.staleEvictions, 1u);
    EXPECT_EQ(w.decodes, 1u) << "the rewritten descriptor is decoded again: its kept view went with its entry";
    EXPECT_EQ(valueOf(rewritten, "CM_FORMAT"), std::optional<std::int64_t>{7})
        << "the view is the new bytes' answer";
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(ShippedConstantsMemo, AKeptViewIsDecodedAgainWhenWhatItReadBeyondItsBytesChanges) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    std::filesystem::path const corpus     = memoCorpus(dir.path(), "owner", /*withOwner=*/false);
    std::filesystem::path const descriptor = corpus / "cm.json";
    std::filesystem::path const owner      = corpus / "cmown.json";
    MemoCall const call = baseCall(*c);
    dss::DiagnosticReporter rep;
    struct Read {
        std::optional<std::int64_t> size;
        MemoWork                    work;
    };
    auto const read = [&] {
        auto const s0   = dss::ffi::shippedDescriptorCacheStats();
        auto const view = readConstants(descriptor, call, rep);
        return Read{valueOf(view, "CM_SIZE_MAX"), memoWorkSince(s0)};
    };
    Read const absent = read();
    EXPECT_FALSE(absent.size.has_value()) << "with no owner the typedef is not realized, nor the constant of it";
    EXPECT_EQ(absent.work.decodes, 1u);
    Read const stillAbsent = read();
    EXPECT_FALSE(stillAbsent.size.has_value());
    EXPECT_EQ(stillAbsent.work.hits, 1u) << "the owner's search still finds nothing: the kept view is served";
    writeDescriptor(owner, memoOwner("u32"));
    Read const created = read();
    EXPECT_EQ(created.size, std::optional<std::int64_t>{4294967295})
        << "the owner created: the search the kept view recorded now answers otherwise";
    EXPECT_EQ(created.work.decodes, 1u);
    Read const unchanged = read();
    EXPECT_EQ(unchanged.work.hits, 1u) << "the owner unchanged: the kept view is served";
    writeDescriptor(owner, memoOwner("u64"));
    Read const rewritten = read();
    EXPECT_EQ(rewritten.size, std::optional<std::int64_t>{-1})
        << "the owner rewritten: the descriptor the kept view read has new bytes";
    EXPECT_EQ(rewritten.work.decodes, 1u);
    std::filesystem::remove(owner);
    Read const deleted = read();
    EXPECT_FALSE(deleted.size.has_value()) << "the owner deleted: the kept view's read of it fails";
    EXPECT_EQ(deleted.work.decodes, 1u);
    EXPECT_EQ(rep.errorCount(), 0u) << "an owner's absence is said to the typedef decode's own scratch alone";
}

TEST(ShippedConstantsMemo, ADecodeThatSaidSomethingIsNotKeptAndSaysItAgain) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    std::filesystem::path const descriptor = dir.path() / "cmbad.json";
    writeDescriptor(descriptor,
                    R"({ "header": "cmbad.h", "constants": [ { "name": "CM_BAD", "of": "long", "limit": "nope" } ] })");
    MemoCall const call = baseCall(*c);
    auto const s0 = dss::ffi::shippedDescriptorCacheStats();
    for (int k = 0; k < 2; ++k) {
        SCOPED_TRACE(k == 0 ? "the first call" : "the second call");
        dss::DiagnosticReporter rep;
        EXPECT_FALSE(readConstants(descriptor, call, rep).has_value());
        EXPECT_GE(rep.errorCount(), 1u) << "the refusal is said at every call, never answered silently from a memo";
    }
    MemoWork const w = memoWorkSince(s0);
    EXPECT_EQ(w.decodes, 2u) << "a decode that said something is not kept";
    EXPECT_EQ(w.hits, 0u);
}

TEST(ShippedConstantsMemo, ALanguageKnownByNoDocumentDigestIsNeverKept) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    std::filesystem::path const descriptor = dir.path() / "cmflat.json";
    writeDescriptor(descriptor,
                    R"({ "header": "cmflat.h", "constants": [ { "name": "CM_FLAT", "type": "i32", "value": 7 } ] })");
    auto const bare = std::make_shared<GrammarSchema const>(dss::detail::GrammarSchemaData{});
    ASSERT_TRUE(bare->contentDigest().empty()) << "a language built with no document has no digest";
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    ASSERT_FALSE(c->contentDigest().empty());
    for (bool const known : {false, true}) {
        SCOPED_TRACE(known ? "the shipped c language" : "a language with no digest");
        MemoCall call;
        call.pair = dss::ffi::ShippedPairFacts{known ? c.get() : bare.get()};
        dss::DiagnosticReporter rep;
        auto const s0 = dss::ffi::shippedDescriptorCacheStats();
        EXPECT_EQ(valueOf(readConstants(descriptor, call, rep), "CM_FLAT"), std::optional<std::int64_t>{7});
        EXPECT_EQ(valueOf(readConstants(descriptor, call, rep), "CM_FLAT"), std::optional<std::int64_t>{7});
        MemoWork const w = memoWorkSince(s0);
        EXPECT_EQ(rep.errorCount(), 0u);
        EXPECT_EQ(w.decodes, known ? 1u : 2u)
            << "a language is keyed by its document's digest; with none it has no value to key by";
    }
}

// ── ONE OPERATION SEARCHES EACH HEADER ONCE (P69 L2b2, ruling 630) ───────────────────────────────────
// A decode's descriptor searches — a typedef's owner, a header a derived row's `of` names — are a fresh
// search of the file system each (≈0.09 ms on Windows, MEASURED quiet), and one unit's decodes and the checks
// of its kept constants views asked the same ones again and again. Inside a read scope each (header, dirs,
// matching) is searched once; the next scope searches again, so an owner created between two operations is
// found by the second.
TEST(ShippedDescriptorSearchSnapshot, AnOperationSearchesEachHeaderOnceAndTheNextOperationSearchesAgain) {
    dss::test_support::ScratchDir const dir{dss::test_support::Location::Temp, kConstantsMemoGroup};
    auto const c = dss::test_support::shippedSchemaOrThrow("c");
    std::filesystem::path const corpus = dir.path() / "search";
    std::filesystem::create_directories(corpus);
    // Two descriptors whose typedef names ONE owner, not written yet.
    for (char const* name : {"cma", "cmb"}) {
        writeDescriptor(corpus / (std::string{name} + ".json"),
                        std::string{R"({ "header": ")"} + name + R"(.h",
  "typedefs": [ { "name": "cm_size", "shippedTypedef": { "header": "cmown.h" } } ],
  "constants": [ { "name": "CM_SIZE_MAX", "of": "cm_size", "limit": "max" } ] })");
    }
    MemoCall const call = baseCall(*c);
    dss::DiagnosticReporter rep;
    struct Operation {
        std::size_t searches;
        std::size_t scopeHits;
        bool        realized;
    };
    auto const operation = [&] {
        dss::ffi::ShippedDescriptorReadScope const scope;
        auto const s0 = dss::ffi::shippedDescriptorCacheStats();
        auto const a  = readConstants(corpus / "cma.json", call, rep);
        auto const b  = readConstants(corpus / "cmb.json", call, rep);
        auto const s1 = dss::ffi::shippedDescriptorCacheStats();
        return Operation{s1.descriptorSearches - s0.descriptorSearches, s1.searchScopeHits - s0.searchScopeHits,
                         valueOf(a, "CM_SIZE_MAX").has_value() && valueOf(b, "CM_SIZE_MAX").has_value()};
    };
    Operation const first = operation();
    EXPECT_EQ(first.searches, 1u) << "one operation searches the owner once, however many descriptors name it";
    EXPECT_GE(first.scopeHits, 1u) << "the second descriptor's search is the snapshot's answer";
    EXPECT_FALSE(first.realized) << "no owner yet: neither constant is realized";
    writeDescriptor(corpus / "cmown.json", memoOwner("u32"));
    Operation const second = operation();
    EXPECT_EQ(second.searches, 1u) << "the next operation searches again, and once";
    EXPECT_TRUE(second.realized) << "the next operation finds the owner created between the two";
    EXPECT_EQ(rep.errorCount(), 0u);
}
