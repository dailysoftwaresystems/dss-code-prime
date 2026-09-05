// D-LINK-MACHO-IMAGE-STATIC-FN-EMITTED-N-EXT — the Mach-O IMAGE tier's nlist
// bands.
//
// Both image nlist builders (`macho::encodeExec`, `macho::encodeExecDynamic`,
// the latter serving the exec AND the dylib flavor) stamped `N_SECT|N_EXT` on
// EVERY defined function, so a `static` reached the final image under its real
// name (D-LINK-MACHO-IMAGE-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS) with the
// EXTERNAL bit set, and LC_DYSYMTAB published `nlocalsym 0`.
// ✔MEASURED 2026-09-04 on Apple Silicon (macOS 25.6.0, Apple clang 21.0.0,
// ld-1267), same source through both toolchains:
//   * Apple: `nm -m -p` reads `(__TEXT,__text) non-external _static_helper` as
//     the FIRST nlist record; LC_DYSYMTAB `ilocalsym 0 nlocalsym 1 iextdefsym 1`.
//   * DSS before the fix: `external _static_helper`, `nlocalsym 0 iextdefsym 0`.
//   * DSS after the fix: the local band opens the table — the nameless
//     linker-injected entry trampoline (`_sym_<id>`) FIRST, then
//     `non-external _static_helper` — under `nlocalsym 2 iextdefsym 2`; the
//     program still exits 42 and `codesign --verify` still passes. (DSS emits
//     the trampoline as a defined function and ld64 does not, which is why
//     the two toolchains agree on the BAND and differ on its first record.)
//
// The binding is the ONE format-neutral `definedBinding` decision, mapped
// through the same `definedNType` the MH_OBJECT writer uses; the ORDER follows
// LC_DYSYMTAB's three contiguous bands (locals first). These pins assert:
//   1. on both ports × all three image arms: the exact nlist SEQUENCE (names
//      AND n_types), the six LC_DYSYMTAB fields exactly, every indirect-symbol
//      entry still naming the import it named before, and the band predicate
//      holding over the emitted table. ⚠ FIVE of those six cells reach the
//      bands: the arm64 STATIC cell pins the
//      D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION refusal instead, because that
//      document declares `image.buildVersion`. Each port DECLARES which
//      outcome it expects and the fixture is asserted against the
//      declaration, so the coverage cannot empty itself silently;
//   2. every refusal arm of `machoDysymtabBandBreach` fires and names the
//      offender (the writer's belt reads it, and a belt that cannot fire is
//      worse than none);
//   3. the same shape through the REAL pipeline, reading the corpus example's
//      own source so the RUN witness (`examples/c/macho_static_fn_image`, run
//      on the darwin leg) and this structural pin describe one artifact.
//
// RED-ON-DISABLE (REMOVE-direction): restore `N_SECT|N_EXT` for a Local in
// `appendImageDefinedBands` (delete the Local arm of the mapping) and (1)/(3)
// fail on n_type AND on the bands; emit the functions in module order instead
// of band order and the writer's own `imageBandsAgree` belt refuses the image.

#include <algorithm>

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_symtab_bands.hpp"
#include "link/object_format_schema.hpp"
#include "macho_test_support.hpp"
#include "program/program.hpp"
#include "repo_root.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using dss::macho::test::findLoadCommand;
using dss::macho::test::findSegment;
using dss::macho::test::readU32LE;
using dss::macho::test::readU64LE;

namespace {

namespace fs = std::filesystem;

constexpr std::uint8_t kNTypeSectLocal = 0x0E;   // N_SECT, no N_EXT
constexpr std::uint8_t kNTypeSectExt   = 0x0F;   // N_SECT | N_EXT
constexpr std::uint8_t kNTypeUndfExt   = 0x01;   // N_UNDF | N_EXT

// One nlist_64 record as this file reads it back: name, n_type, n_value.
struct NlistRecord {
    std::string   name;
    std::uint8_t  nType = 0;
    std::uint64_t nValue = 0;
    bool operator==(NlistRecord const&) const = default;
};

// The whole LC_SYMTAB table in FILE ORDER — the order LC_DYSYMTAB's bands index.
[[nodiscard]] std::vector<NlistRecord>
readNlist(std::vector<std::uint8_t> const& bytes) {
    std::vector<NlistRecord> out;
    auto const lc = findLoadCommand(bytes, /*LC_SYMTAB=*/0x02u);
    if (!lc) return out;
    std::uint32_t const symOff = readU32LE(bytes, *lc + 8);
    std::uint32_t const nsyms  = readU32LE(bytes, *lc + 12);
    std::uint32_t const strOff = readU32LE(bytes, *lc + 16);
    for (std::uint32_t i = 0; i < nsyms; ++i) {
        std::size_t const rec = static_cast<std::size_t>(symOff) + i * 16u;
        if (rec + 16 > bytes.size()) break;
        NlistRecord r;
        std::size_t p = static_cast<std::size_t>(strOff) + readU32LE(bytes, rec);
        while (p < bytes.size() && bytes[p] != 0)
            r.name.push_back(static_cast<char>(bytes[p++]));
        r.nType  = bytes[rec + 4];
        r.nValue = readU64LE(bytes, rec + 8);
        out.push_back(std::move(r));
    }
    return out;
}

// The raw nlist bytes, for the predicate.
[[nodiscard]] std::vector<std::uint8_t>
nlistBytesOf(std::vector<std::uint8_t> const& bytes) {
    auto const lc = findLoadCommand(bytes, /*LC_SYMTAB=*/0x02u);
    if (!lc) return {};
    std::uint32_t const symOff = readU32LE(bytes, *lc + 8);
    std::uint32_t const nsyms  = readU32LE(bytes, *lc + 12);
    std::size_t const end = static_cast<std::size_t>(symOff) + nsyms * 16u;
    if (end > bytes.size()) return {};
    return {bytes.begin() + static_cast<std::ptrdiff_t>(symOff),
            bytes.begin() + static_cast<std::ptrdiff_t>(end)};
}

struct DysymtabView {
    bool found = false;
    link::format::MachoDysymtabBands bands;
    std::vector<std::uint32_t> indirect;
};

[[nodiscard]] DysymtabView readDysymtab(std::vector<std::uint8_t> const& bytes) {
    DysymtabView v;
    auto const lc = findLoadCommand(bytes, /*LC_DYSYMTAB=*/0x0Bu);
    if (!lc) return v;
    v.found            = true;
    v.bands.ilocalsym  = readU32LE(bytes, *lc + 8);
    v.bands.nlocalsym  = readU32LE(bytes, *lc + 12);
    v.bands.iextdefsym = readU32LE(bytes, *lc + 16);
    v.bands.nextdefsym = readU32LE(bytes, *lc + 20);
    v.bands.iundefsym  = readU32LE(bytes, *lc + 24);
    v.bands.nundefsym  = readU32LE(bytes, *lc + 28);
    std::uint32_t const indirectOff = readU32LE(bytes, *lc + 56);
    std::uint32_t const nindirect   = readU32LE(bytes, *lc + 60);
    for (std::uint32_t k = 0; k < nindirect; ++k) {
        std::size_t const at = static_cast<std::size_t>(indirectOff) + k * 4u;
        if (at + 4 > bytes.size()) break;
        v.indirect.push_back(readU32LE(bytes, at));
    }
    return v;
}

[[nodiscard]] bool sawDiagnosticContaining(DiagnosticReporter const& rep,
                                           std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] std::vector<std::uint8_t> readFileBytes(fs::path const& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The three FINAL-IMAGE arms, named by the walker each selects.
enum class ImageArm { StaticExec, DynamicExec, Dylib };

struct MachoPortSpec {
    char const*               label;
    char const*               targetName;
    char const*               execFormat;
    char const*               dylibFormat;
    std::vector<std::uint8_t> retBytes;
    // ⚠⚠ STATED PER PORT, NEVER DERIVED FROM THE FIXTURE, and that is the
    // whole point of the field. This cell used to compute the static arm's
    // expected outcome from `(*fmt)->machoImage().buildVersion.has_value()`
    // — so whichever way the fixture went, the cell agreed with it. The
    // arm64 exec document declares `buildVersion`, which the static walker
    // refuses (D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION), leaving the x86_64
    // cell as the ONLY one of the six that reaches the bands on that arm; the
    // day the x86_64 document gained a `buildVersion` too, BOTH static cells
    // would have short-circuited into the refusal branch and this pin would
    // have covered the static arm's bands NOWHERE, staying green throughout.
    // Stating the expectation and asserting the fixture against it turns that
    // silent emptying into a red that names the document that moved.
    bool                      staticArmRefusesOnBuildVersion;
};

std::vector<MachoPortSpec> const kPorts{
    {"arm64", "arm64", "macho64-arm64-darwin-exec", "macho64-arm64-darwin-dylib",
     {0xC0, 0x03, 0x5F, 0xD6},                                  // RET
     /*staticArmRefusesOnBuildVersion=*/true},
    {"x86_64", "x86_64", "macho64-x86_64-darwin-exec",
     "macho64-x86_64-darwin-dylib", {0xC3},                     // ret
     /*staticArmRefusesOnBuildVersion=*/false},
};

// A 16-byte nlist_64 record carrying only the n_type the predicate reads.
[[nodiscard]] std::vector<std::uint8_t> rec(std::uint8_t nType) {
    std::vector<std::uint8_t> r(16, 0);
    r[4] = nType;
    return r;
}

[[nodiscard]] std::vector<std::uint8_t>
table(std::initializer_list<std::uint8_t> types) {
    std::vector<std::uint8_t> out;
    for (std::uint8_t t : types) {
        auto r = rec(t);
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

} // namespace

// ── (1) THE MATRIX: both ports × all three image arms ───────────────────────

TEST(MachoImageSymtabBands, StaticFunctionIsLocalAndSortsFirstOnEveryImageArm) {
    auto runCell = [](MachoPortSpec const& port, ImageArm arm) -> void {
        char const* const armLabel =
            arm == ImageArm::StaticExec  ? " [static exec arm]"
          : arm == ImageArm::DynamicExec ? " [dynamic exec arm]"
                                         : " [dylib arm]";
        std::string const label = std::string{port.label} + armLabel;
        bool const isDylibCell = arm == ImageArm::Dylib;
        bool const wantsExtern = arm != ImageArm::StaticExec;

        auto target = TargetSchema::loadShipped(port.targetName);
        ASSERT_TRUE(target.has_value()) << label;
        // The static cell drives the UNSIGNED counterpart of the shipped exec
        // document (D-LK-MACHO-ADHOC-SIGNATURE-DROPPED-ON-STATIC-ARM): every
        // shipped Darwin exec requests a signature, which `encodeExec` cannot
        // host, so the shipped document would turn this cell into a test of
        // that refusal.
        using FmtLoad = LoadResult<std::shared_ptr<ObjectFormatSchema>>;
        auto fmt = (arm == ImageArm::StaticExec)
            ? FmtLoad{dss::macho::test::loadUnsignedExec(port.execFormat)}
            : ObjectFormatSchema::loadShipped(
                  isDylibCell ? port.dylibFormat : port.execFormat);
        ASSERT_TRUE(fmt.has_value()) << label;
        ASSERT_NE(*fmt, nullptr) << label;

        // fn #7 — a `static` (Local row WITH a declared name): THE case.
        // fn #8 — externally visible, plus a second GLOBAL name (an alias).
        // fn #9 — NO `ModuleSymbol` row: the linker-injected trampoline's
        //         shape, which `definedBinding` resolves to Local as well.
        // Module order 7, 8, 9 — so the local band (7, 9) is NOT a prefix of
        // module order, and the pin sees the REORDER, not just the bit.
        AssembledModule mod;
        mod.expectedFuncCount = 3;
        for (std::uint32_t id : {7u, 8u, 9u}) {
            AssembledFunction f;
            f.symbol = SymbolId{id};
            f.bytes  = port.retBytes;
            mod.functions.push_back(std::move(f));
        }
        mod.symbols.push_back(ModuleSymbol{SymbolId{7}, "_img_static_fn",
                                           SymbolBinding::Local,
                                           SymbolVisibility::Default});
        mod.symbols.push_back(ModuleSymbol{SymbolId{8}, "_img_global_fn",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});
        mod.symbols.push_back(ModuleSymbol{SymbolId{8}, "_img_global_alias",
                                           SymbolBinding::Global,
                                           SymbolVisibility::Default});
        // TWO function externs, so the indirect symbol table carries a stub
        // band AND a __got band with more than one distinct index each.
        if (wantsExtern) {
            for (auto const& [id, name] :
                 {std::pair{98u, "_puts"}, std::pair{99u, "_write"}}) {
                ExternImport imp;
                imp.symbol      = SymbolId{id};
                imp.mangledName = name;
                imp.libraryPath = "/usr/lib/libSystem.B.dylib";
                mod.externImports.push_back(std::move(imp));
            }
        }
        if (!isDylibCell) mod.imageEntryOverride = std::size_t{0};

        // The arm64 exec schema declares image.buildVersion, which the static
        // walker refuses (D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION) — pinned by
        // its sibling; there the cell asserts the boundary and stops. The
        // expectation is DECLARED by the port (see the field's comment) and
        // the fixture is asserted AGAINST it, so a document that gains or
        // loses `buildVersion` reds here instead of quietly moving this cell
        // from "pins the bands" to "pins the refusal".
        bool const staticArmRefusedByBuildVersion =
            arm == ImageArm::StaticExec && port.staticArmRefusesOnBuildVersion;
        if (arm == ImageArm::StaticExec) {
            ASSERT_EQ((*fmt)->machoImage().buildVersion.has_value(),
                      port.staticArmRefusesOnBuildVersion)
                << label
                << ": the exec document's buildVersion no longer matches what "
                   "this port DECLARES. Update the port row deliberately — if "
                   "both ports come to refuse, the static arm's bands are "
                   "pinned by no cell at all.";
        }

        DiagnosticReporter rep;
        auto const bytes = dss::macho::encode(mod, **target, **fmt, rep);
        std::string diags;
        for (auto const& d : rep.all()) diags += d.actual + "\n";

        if (staticArmRefusedByBuildVersion) {
            EXPECT_TRUE(bytes.empty()) << label << "\n" << diags;
            EXPECT_TRUE(sawDiagnosticContaining(
                rep, "D-LK10-ENTRY-MACHO-STATIC-BUILD-VERSION"))
                << label << "\n" << diags;
            return;
        }
        ASSERT_EQ(rep.errorCount(), 0u) << label << "\n" << diags;
        ASSERT_FALSE(bytes.empty()) << label << "\n" << diags;

        // Each cell must reach the arm it names.
        EXPECT_EQ(readU32LE(bytes, 12), isDylibCell ? 6u : 2u)
            << label << ": wrong MH_ filetype for the arm this cell names";
        if (!isDylibCell) {
            EXPECT_EQ(findSegment(bytes, "__LINKEDIT").has_value(),
                      arm == ImageArm::DynamicExec)
                << label << ": this cell did not reach the exec walker it names";
        }

        // ── the exact table, in file order ──────────────────────────────────
        auto const nlist = readNlist(bytes);
        std::vector<std::string>  names;
        std::vector<std::uint8_t> types;
        for (auto const& r : nlist) {
            names.push_back(r.name);
            types.push_back(r.nType);
        }
        std::vector<std::string> expectedNames{
            "_img_static_fn", "_sym_9", "_img_global_fn", "_img_global_alias"};
        std::vector<std::uint8_t> expectedTypes{
            kNTypeSectLocal, kNTypeSectLocal, kNTypeSectExt, kNTypeSectExt};
        if (wantsExtern) {
            expectedNames.insert(expectedNames.end(), {"_puts", "_write"});
            expectedTypes.insert(expectedTypes.end(),
                                 {kNTypeUndfExt, kNTypeUndfExt});
        }
        EXPECT_EQ(names, expectedNames)
            << label
            << ": the local band (the `static` AND the nameless trampoline "
               "shape) must come FIRST, then the externally-defined band with "
               "the alias right after its canonical, then the imports";
        EXPECT_EQ(types, expectedTypes)
            << label
            << ": a Local definition is bare N_SECT (0x0E), an externally "
               "visible one N_SECT|N_EXT (0x0F), an import N_UNDF|N_EXT (0x01)";
        ASSERT_EQ(nlist.size(), expectedNames.size()) << label;

        // ── n_value: reordering the records must not move an address ───────
        // Module order is 7, 8, 9 in __text, so VA(7) < VA(8) < VA(9); the
        // table order is 7, 9, 8, alias(8).
        EXPECT_GT(nlist[0].nValue, 0u) << label;
        EXPECT_LT(nlist[0].nValue, nlist[2].nValue)
            << label << ": fn #7 precedes fn #8 in __text";
        EXPECT_LT(nlist[2].nValue, nlist[1].nValue)
            << label << ": fn #8 precedes fn #9 in __text";
        EXPECT_EQ(nlist[2].nValue, nlist[3].nValue)
            << label << ": the alias resolves to its canonical's ONE address";

        // ── the bands, exactly ──────────────────────────────────────────────
        auto const dysym = readDysymtab(bytes);
        if (arm == ImageArm::StaticExec) {
            EXPECT_FALSE(dysym.found)
                << label << ": the static walker emits no LC_DYSYMTAB";
            return;
        }
        ASSERT_TRUE(dysym.found) << label;
        EXPECT_EQ(dysym.bands.ilocalsym,  0u) << label;
        EXPECT_EQ(dysym.bands.nlocalsym,  2u)
            << label << ": fn #7 (static) and fn #9 (no declared name)";
        EXPECT_EQ(dysym.bands.iextdefsym, 2u) << label;
        EXPECT_EQ(dysym.bands.nextdefsym, 2u)
            << label << ": fn #8 and its alias";
        EXPECT_EQ(dysym.bands.iundefsym,  4u) << label;
        EXPECT_EQ(dysym.bands.nundefsym,  2u) << label;
        EXPECT_EQ(link::format::machoDysymtabBandBreach(nlistBytesOf(bytes),
                                                        dysym.bands),
                  "")
            << label << ": the emitted table must satisfy the predicate the "
                        "writer's belt reads";

        // ── every indirect-symbol entry still names the import it named ────
        // before the reorder: `numDefs + <extern index>`, stubs band then
        // __got band, each in extern order.
        std::vector<std::uint32_t> const expectedIndirect{4u, 5u, 4u, 5u};
        EXPECT_EQ(dysym.indirect, expectedIndirect)
            << label
            << ": the stub band and the __got band each list the two imports "
               "in extern order, at the indices the undefined band occupies";
        for (std::uint32_t const idx : dysym.indirect) {
            ASSERT_LT(idx, nlist.size()) << label;
            EXPECT_EQ(nlist[idx].nType, kNTypeUndfExt)
                << label << ": indirect entry #" << idx
                << " must land on an undefined import";
        }
        EXPECT_EQ(nlist[4].name, "_puts")  << label;
        EXPECT_EQ(nlist[5].name, "_write") << label;
    };

    // The static arm's bands are reached by whichever ports do NOT refuse on
    // `buildVersion`. If that set is ever empty the six cells still all pass
    // — five pinning bands, one pinning a refusal — with the static arm's
    // band layout pinned by nothing. Say so here rather than discovering it.
    ASSERT_TRUE(std::any_of(kPorts.begin(), kPorts.end(),
                            [](MachoPortSpec const& p) {
                                return !p.staticArmRefusesOnBuildVersion;
                            }))
        << "every port now refuses the static exec arm on buildVersion, so no "
           "cell below reaches that arm's LC_DYSYMTAB band layout";

    for (auto const& port : kPorts) {
        runCell(port, ImageArm::StaticExec);
        runCell(port, ImageArm::DynamicExec);
        runCell(port, ImageArm::Dylib);
    }
}

// ── (2) EVERY REFUSAL ARM OF THE PREDICATE FIRES, and names its offender ────

TEST(MachoImageSymtabBands, BandPredicateHoldsOnAWellFormedTable) {
    using link::format::MachoDysymtabBands;
    using link::format::machoDysymtabBandBreach;
    // local, extdef, undef — one of each, bands tiling three records.
    EXPECT_EQ(machoDysymtabBandBreach(
                  table({kNTypeSectLocal, kNTypeSectExt, kNTypeUndfExt}),
                  MachoDysymtabBands{0, 1, 1, 1, 2, 1}),
              "");
    // No locals at all (an image whose every function is exported).
    EXPECT_EQ(machoDysymtabBandBreach(table({kNTypeSectExt, kNTypeUndfExt}),
                                      MachoDysymtabBands{0, 0, 0, 1, 1, 1}),
              "");
    // No undefined band (the static arm's shape).
    EXPECT_EQ(machoDysymtabBandBreach(table({kNTypeSectLocal, kNTypeSectExt}),
                                      MachoDysymtabBands{0, 1, 1, 1, 2, 0}),
              "");
    // An empty table with empty bands.
    EXPECT_EQ(machoDysymtabBandBreach({}, MachoDysymtabBands{}), "");
}

TEST(MachoImageSymtabBands, EveryBreachArmFiresAndNamesTheOffender) {
    using link::format::MachoDysymtabBands;
    using link::format::machoDysymtabBandBreach;

    std::vector<std::uint8_t> ragged(17, 0);
    std::string const b1 = machoDysymtabBandBreach(ragged, MachoDysymtabBands{});
    EXPECT_NE(b1.find("16-byte records"), std::string::npos) << b1;

    auto const good = table({kNTypeSectLocal, kNTypeSectExt, kNTypeUndfExt});

    std::string const b2 =
        machoDysymtabBandBreach(good, MachoDysymtabBands{1, 1, 1, 1, 2, 1});
    EXPECT_NE(b2.find("ilocalsym is 1"), std::string::npos) << b2;

    std::string const b3 =
        machoDysymtabBandBreach(good, MachoDysymtabBands{0, 1, 0, 2, 2, 1});
    EXPECT_NE(b3.find("iextdefsym is 0"), std::string::npos) << b3;

    std::string const b4 =
        machoDysymtabBandBreach(good, MachoDysymtabBands{0, 1, 1, 1, 3, 0});
    EXPECT_NE(b4.find("iundefsym is 3"), std::string::npos) << b4;

    std::string const b5 =
        machoDysymtabBandBreach(good, MachoDysymtabBands{0, 1, 1, 1, 2, 5});
    EXPECT_NE(b5.find("must tile LC_SYMTAB.nsyms"), std::string::npos) << b5;

    // An externally visible record inside the local band — the exact shape
    // the pre-fix writer produced for every `static`.
    std::string const b6 = machoDysymtabBandBreach(
        table({kNTypeSectExt, kNTypeSectExt, kNTypeUndfExt}),
        MachoDysymtabBands{0, 1, 1, 1, 2, 1});
    EXPECT_NE(b6.find("symbol #0"), std::string::npos) << b6;
    EXPECT_NE(b6.find("local band"), std::string::npos) << b6;

    // A Local record that spilled into the externally-defined band.
    std::string const b7 = machoDysymtabBandBreach(
        table({kNTypeSectLocal, kNTypeSectLocal, kNTypeUndfExt}),
        MachoDysymtabBands{0, 1, 1, 1, 2, 1});
    EXPECT_NE(b7.find("symbol #1"), std::string::npos) << b7;
    EXPECT_NE(b7.find("externally-defined band"), std::string::npos) << b7;

    // A definition inside the undefined band — the alias-past-the-boundary
    // shape every indirect index would then be short by.
    std::string const b8 = machoDysymtabBandBreach(
        table({kNTypeSectLocal, kNTypeSectExt, kNTypeSectExt}),
        MachoDysymtabBands{0, 1, 1, 1, 2, 1});
    EXPECT_NE(b8.find("symbol #2"), std::string::npos) << b8;
    EXPECT_NE(b8.find("undefined band"), std::string::npos) << b8;
}

// ── (3) THE REAL PIPELINE, on the corpus example's own source ──────────────

TEST(MachoImageSymtabBands, RealPipelineExecPublishesLocalBandForStaticsAndTrampoline) {
    using dss::test_support::Location;
    using dss::test_support::ScratchDir;

    // The example is read from the tree, never copied here: the RUN witness on
    // the darwin leg and this structural pin must describe ONE source.
    fs::path const src = dss::test::repoRoot() / "examples" / "c"
                         / "macho_static_fn_image" / "main.c";
    ASSERT_TRUE(fs::exists(src)) << src.generic_string();

    ScratchDir scratch{Location::InsideRepo, "p60_macho_image_symtab_bands"};
    scratch.useAsCwd();
    auto const outDir = scratch.path() / "out";

    Program            prog;
    DiagnosticReporter rep;
    prog.setOutputDir(outDir);
    int const rc = prog.compileFiles({src.generic_string()}, "c",
                                     {"arm64:macho64-arm64-darwin-exec"}, rep);
    std::string diags;
    for (auto const& d : rep.all()) diags += "\n  " + d.actual;
    ASSERT_EQ(rc, 0) << "compile failed:" << diags;
    ASSERT_EQ(rep.errorCount(), 0u) << diags;

    auto const artifact = outDir / "main";
    ASSERT_TRUE(fs::exists(artifact)) << artifact.generic_string();
    auto const bytes = readFileBytes(artifact);
    ASSERT_FALSE(bytes.empty());

    auto const nlist = readNlist(bytes);
    ASSERT_EQ(nlist.size(), 6u)
        << "trampoline + static_helper + other_static + global_helper + main "
           "+ the `_exit` import";
    // The entry trampoline is functions[0] with a minted SymbolId and no
    // declared name, so it keeps the `_sym_<id>` fallback; the id is minted
    // per build and is not asserted, its PREFIX and its band are.
    EXPECT_EQ(nlist[0].name.rfind("_sym_", 0), 0u)
        << "the trampoline must keep the `_sym_<id>` fallback: " << nlist[0].name;
    EXPECT_EQ(nlist[0].nType, kNTypeSectLocal)
        << "the nameless trampoline is Local (no `ModuleSymbol` row)";
    EXPECT_EQ(nlist[1].name,  "_static_helper");
    EXPECT_EQ(nlist[1].nType, kNTypeSectLocal) << "a `static` is bare N_SECT";
    EXPECT_EQ(nlist[2].name,  "_other_static");
    EXPECT_EQ(nlist[2].nType, kNTypeSectLocal) << "a `static` is bare N_SECT";
    EXPECT_EQ(nlist[3].name,  "_global_helper");
    EXPECT_EQ(nlist[3].nType, kNTypeSectExt);
    EXPECT_EQ(nlist[4].name,  "_main");
    EXPECT_EQ(nlist[4].nType, kNTypeSectExt);
    EXPECT_EQ(nlist[5].name,  "_exit");
    EXPECT_EQ(nlist[5].nType, kNTypeUndfExt);

    auto const dysym = readDysymtab(bytes);
    ASSERT_TRUE(dysym.found);
    EXPECT_EQ(dysym.bands.ilocalsym,  0u);
    EXPECT_EQ(dysym.bands.nlocalsym,  3u);
    EXPECT_EQ(dysym.bands.iextdefsym, 3u);
    EXPECT_EQ(dysym.bands.nextdefsym, 2u);
    EXPECT_EQ(dysym.bands.iundefsym,  5u);
    EXPECT_EQ(dysym.bands.nundefsym,  1u);
    EXPECT_EQ(link::format::machoDysymtabBandBreach(nlistBytesOf(bytes),
                                                    dysym.bands),
              "");
    // One function import: a stub-band entry and a __got-band entry, both
    // naming `_exit` at index 5.
    std::vector<std::uint32_t> const expectedIndirect{5u, 5u};
    EXPECT_EQ(dysym.indirect, expectedIndirect);
}
