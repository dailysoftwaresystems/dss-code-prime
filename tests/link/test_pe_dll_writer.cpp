// PE32+ DLL writer tests — c152, the D-LK2-4 anchor (the PE mirror of
// the c150 ELF `.so` / tests/link/test_elf_dyn_writer.cpp).
//
// Pins the dynamic-link-library contract the REAL Windows loader
// consumes (`ctypes.CDLL('dsslib.dll').dss_add(2, 40)`):
//   * IMAGE_FILE_DLL (0x2000) + EXECUTABLE_IMAGE (0x0002) in the file
//     header Characteristics (cl /LD ground truth: 0x2022);
//     AddressOfEntryPoint == 0 (no DllMain — D-LK2-DLL-DLLMAIN-ENTRY
//     is the pinned follow-up); DllCharacteristics keeps DYNAMIC_BASE
//     (the loader may rebase; `.reloc` must therefore be complete);
//     ImageBase = 0x180000000 (MSVC dll convention).
//   * `.edata` EXPORTS wired into data-directory[0]: externally-
//     visible defined functions + data globals from `module.symbols`
//     under their REAL names; the Name Pointer Table is
//     LEXICOGRAPHICALLY SORTED (GetProcAddress binary-searches it —
//     the strongest pin feeds deliberately OUT-OF-ORDER names and
//     asserts the emitted table is sorted AND each name's ordinal
//     lands on the right Export Address Table RVA); a data export's
//     EAT RVA points into its data section; Local (static) symbols
//     never export.
//   * `.reloc` completeness: an abs64 fn-ptr-table slot gets an
//     IMAGE_REL_BASED_DIR64 entry (red-on-disable — the test computes
//     the expected site RVA and finds the exact entry).
//   * Loader-bound import slots (P69, design c2 —
//     D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB): a slot holding an
//     import's address (function OR datum) is the FirstThunk of an import
//     descriptor of its own, holds its lookup entry and carries no DIR64
//     row; a read-only one lies under the IAT directory in a read-only
//     `.idata`. (A function slot USED to bake the import THUNK with a
//     DIR64 row, and a data slot was refused.)
//   * The RESIDUE design c2 cannot express (P69 review M1,
//     D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE): an import's address PLUS an
//     addend, or in a thread-local template, is written at load by a
//     synthesized RUNNER the TLS directory names as its first callback; a
//     read-only item holding one is laid out writable. (A DATA import there
//     USED to be refused by name.)
//   * Fail-loud belts: an
//     ABSOLUTE function reloc into `.text` of a DYNAMIC_BASE image
//     rejects (D-LK-PE-IMAGE-TEXT-ABS-RELOC); an imageEntryOverride
//     on a dll module rejects (a dll has no image entry); a duplicate
//     export name rejects (the name table is a binary-searched
//     unique-key table).
//   * validate() shape rules: no entry cluster / no entryPoint on a
//     dll schema; IMAGE_FILE_DLL required on dll + forbidden on exec.
//   * Policy: `allowsUndefinedImports()` is FALSE (Windows has no
//     ld.so-style deferred global scope for implicitly-linked DLLs).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
#include "format_reject_support.hpp"   // countAtPath / rejectSummary

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::rejectSummary;

namespace {

using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

// ── Fixed PE32+ image file offsets (see pe.cpp layout constants) ──
// [0x84] IMAGE_FILE_HEADER: NumberOfSections @0x86, Characteristics
// @0x96. [0x98] IMAGE_OPTIONAL_HEADER64: AddressOfEntryPoint @+16,
// ImageBase @+24, DllCharacteristics @+70, data directories @+112
// (16 x {u32 rva, u32 size}). [0x188] IMAGE_SECTION_HEADER x N.
constexpr std::size_t kOptHdrOff        = 0x98;
constexpr std::size_t kEntryPointOff    = kOptHdrOff + 16;
constexpr std::size_t kImageBaseOff     = kOptHdrOff + 24;
constexpr std::size_t kDllCharsOff      = kOptHdrOff + 70;
constexpr std::size_t kDataDirOff       = kOptHdrOff + 112;
constexpr std::size_t kExportDirOff     = kDataDirOff + 0 * 8;
constexpr std::size_t kBaseRelocDirOff  = kDataDirOff + 5 * 8;
constexpr std::size_t kSectionHdrsOff   = 0x188;

struct SectionView {
    std::uint32_t virtualSize    = 0;
    std::uint32_t virtualAddress = 0;
    std::uint32_t sizeOfRawData  = 0;
    std::uint32_t rawPointer     = 0;
    bool          found          = false;
};

[[nodiscard]] SectionView findSection(std::vector<std::uint8_t> const& img,
                                      std::string_view name) {
    SectionView out;
    std::uint16_t const n = readU16LE(img, 0x86);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const h =
            kSectionHdrsOff + static_cast<std::size_t>(i) * 40u;
        bool eq = true;
        for (std::size_t b = 0; b < 8; ++b) {
            char const c = b < name.size() ? name[b] : '\0';
            if (static_cast<char>(img[h + b]) != c) { eq = false; break; }
        }
        if (!eq) continue;
        out.virtualSize    = readU32LE(img, h + 8);
        out.virtualAddress = readU32LE(img, h + 12);
        out.sizeOfRawData  = readU32LE(img, h + 16);
        out.rawPointer     = readU32LE(img, h + 20);
        out.found          = true;
        return out;
    }
    return out;
}

// RVA -> file offset within a known section.
[[nodiscard]] std::size_t fileOff(SectionView const& s, std::uint32_t rva) {
    return static_cast<std::size_t>(s.rawPointer) + (rva - s.virtualAddress);
}

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::size_t off) {
    std::string s;
    for (std::size_t p = off; p < b.size() && b[p] != 0; ++p)
        s.push_back(static_cast<char>(b[p]));
    return s;
}

constexpr std::size_t kImportDirOff = kDataDirOff + 1 * 8;
constexpr std::size_t kIatDirOff    = kDataDirOff + 12 * 8;

// The section whose [virtualAddress, +virtualSize) holds `rva`.
[[nodiscard]] SectionView sectionHolding(std::vector<std::uint8_t> const& img,
                                         std::uint32_t rva) {
    std::uint16_t const n = readU16LE(img, 0x86);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const h = kSectionHdrsOff + static_cast<std::size_t>(i) * 40u;
        SectionView s;
        s.virtualSize    = readU32LE(img, h + 8);
        s.virtualAddress = readU32LE(img, h + 12);
        s.sizeOfRawData  = readU32LE(img, h + 16);
        s.rawPointer     = readU32LE(img, h + 20);
        s.found          = true;
        if (rva >= s.virtualAddress && rva < s.virtualAddress + s.virtualSize) return s;
    }
    return SectionView{};
}

// The characteristics of the section whose header is named `name`.
[[nodiscard]] std::uint32_t sectionCharacteristics(std::vector<std::uint8_t> const& img,
                                                   std::string_view name) {
    std::uint16_t const n = readU16LE(img, 0x86);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const h = kSectionHdrsOff + static_cast<std::size_t>(i) * 40u;
        if (readCStr(img, h).substr(0, 8) == name) return readU32LE(img, h + 36);
    }
    return 0;
}

// IMAGE_IMPORT_DESCRIPTOR, read back from the image bytes (PE/COFF §6.4.1):
// the loader walks these until the all-zero one.
struct ImportDescriptorView {
    std::uint32_t originalFirstThunk = 0;
    std::uint32_t name               = 0;
    std::uint32_t firstThunk         = 0;
};
[[nodiscard]] std::vector<ImportDescriptorView>
readImportDescriptors(std::vector<std::uint8_t> const& img) {
    std::vector<ImportDescriptorView> out;
    std::uint32_t const rva = readU32LE(img, kImportDirOff);
    SectionView const s = sectionHolding(img, rva);
    if (!s.found) return out;
    for (std::size_t off = fileOff(s, rva);; off += 20) {
        ImportDescriptorView d{readU32LE(img, off), readU32LE(img, off + 12),
                               readU32LE(img, off + 16)};
        if (d.originalFirstThunk == 0 && d.name == 0 && d.firstThunk == 0
            && readU32LE(img, off + 4) == 0 && readU32LE(img, off + 8) == 0) {
            break;
        }
        out.push_back(d);
    }
    return out;
}
[[nodiscard]] std::uint64_t readU64AtRva(std::vector<std::uint8_t> const& img,
                                         std::uint32_t rva) {
    return readU64LE(img, fileOff(sectionHolding(img, rva), rva));
}
[[nodiscard]] std::string readCStrAtRva(std::vector<std::uint8_t> const& img,
                                        std::uint32_t rva) {
    return readCStr(img, fileOff(sectionHolding(img, rva), rva));
}

// Every (siteRva) of the `.reloc` table's IMAGE_REL_BASED_DIR64 rows.
[[nodiscard]] std::vector<std::uint32_t>
readDir64Sites(std::vector<std::uint8_t> const& img) {
    std::vector<std::uint32_t> out;
    SectionView const reloc = findSection(img, ".reloc");
    if (!reloc.found) return out;
    std::uint32_t const tableSize = readU32LE(img, kBaseRelocDirOff + 4);
    std::size_t p = reloc.rawPointer;
    std::size_t const end = reloc.rawPointer + tableSize;
    while (p + 8 <= end) {
        std::uint32_t const pageRva   = readU32LE(img, p);
        std::uint32_t const blockSize = readU32LE(img, p + 4);
        if (blockSize < 8) break;
        for (std::size_t e = p + 8; e + 2 <= p + blockSize; e += 2) {
            std::uint16_t const entry = readU16LE(img, e);
            if ((entry >> 12) == 10u) {   // IMAGE_REL_BASED_DIR64
                out.push_back(pageRva + (entry & 0x0FFFu));
            }
        }
        p += blockSize;
    }
    return out;
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShippedPe(std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

[[nodiscard]] Loaded loadShippedDll() {
    return loadShippedPe("pe64-x86_64-windows-dll");
}
[[nodiscard]] Loaded loadShippedExec() {
    return loadShippedPe("pe64-x86_64-windows-exec");
}

// ★ The one door the EXEC-arm guards below use to reach `pe::encode`.
// D-LK10-ENTRY entry gate (`resolveEntryFnIdx`): a format declaring
// `processExit` contracts that its image entry is the synthesized
// `_start` trampoline, which ONLY `linker::link` injects (it stamps
// `imageEntryOverride = 0`). These are byte-level writer pins driven
// straight at the walker, so they state the untrampolined intent
// themselves — index 0 is what the pre-gate default gave implicitly.
// The DLL sites deliberately keep calling `pe::encode` raw: an
// override on a dll module is itself a fail-loud case, pinned by
// PeDllWriter.ImageEntryOverrideFailsLoud below.
[[nodiscard]] std::vector<std::uint8_t>
encodeUntrampolined(AssembledModule           mod,  // by value: stamped copy
                    TargetSchema const&       target,
                    ObjectFormatSchema const& fmt,
                    DiagnosticReporter&       reporter) {
    mod.imageEntryOverride = std::size_t{0};
    return pe::encode(mod, target, fmt, reporter);
}

// ── Module builders (mirror test_elf_dyn_writer.cpp) ─────────────

// THREE exported functions whose names arrive deliberately OUT OF
// LEXICOGRAPHIC ORDER (zeta, alpha, mid) + one LOCAL (static)
// function that must NOT export + one exported int global in `.data`.
// The sort-invariant pin drives GetProcAddress's binary-search
// contract from this input.
[[nodiscard]] AssembledModule makeExportModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 4;
    auto addFn = [&](std::uint32_t sym) {
        AssembledFunction fn;
        fn.symbol = SymbolId{sym};
        fn.bytes  = {0xC3};
        mod.functions.push_back(std::move(fn));
    };
    addFn(1);   // dss_zeta  @ .text+0
    addFn(2);   // dss_alpha @ .text+1
    addFn(3);   // dss_mid   @ .text+2
    addFn(4);   // hidden_helper (Local) @ .text+3
    AssembledData d;
    d.symbol    = SymbolId{5};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_zeta",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "dss_alpha",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{3}, "dss_mid",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{4}, "hidden_helper",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "dss_global",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// One exported function `dss_dispatch` + a RELRO fn-ptr table {&f} —
// the W2 shape: the const table slot carries an abs64 (kind 2) reloc
// to the function; the dll image must patch the preferred-base VA AND
// emit an IMAGE_REL_BASED_DIR64 row for the slot.
[[nodiscard]] AssembledModule makeFnPtrTableModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData tab;
    tab.symbol    = SymbolId{5};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes     = std::vector<std::uint8_t>(8, 0);
    tab.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{2};   // abs64
    rel.addend = 0;
    tab.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(tab));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_dispatch",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "dss_tab",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// A data slot whose abs64 reloc targets an EXTERN import (isData
// selectable): the D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR shapes. The slot sits at
// offset 8 of a 24-byte item (a struct member / array element), so a check
// that reads the item's FIRST word cannot pass by accident.
[[nodiscard]] AssembledModule makeExternSlotModule(
        bool externIsData, DataSectionKind section = DataSectionKind::Data,
        std::int64_t addend = 0) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData slot;
    slot.symbol    = SymbolId{5};
    slot.section   = section;
    slot.bytes     = std::vector<std::uint8_t>(24, 0);
    slot.bytes[0]  = 0x11;   // the neighbours of the slot keep their bytes
    slot.bytes[16] = 0x22;
    slot.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 8;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{2};   // abs64
    rel.addend = addend;
    slot.relocations.push_back(rel);
    mod.dataItems.push_back(std::move(slot));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = externIsData ? "_fmode" : "puts";
    imp.libraryPath = "msvcrt.dll";
    imp.isData      = externIsData;
    mod.externImports.push_back(std::move(imp));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{5}, "dss_slot",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

// Every diagnostic's text, for a failure message that explains itself.
[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) out += "\n  " + d.actual;
    return out;
}

[[nodiscard]] bool sawDiagnosticContaining(DiagnosticReporter const& rep,
                                           std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ── Shipped JSON loads + policy predicates ───────────────────────

TEST(PeDllFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShippedDll();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Pe);
    EXPECT_EQ(loaded.format->name(), "pe64-x86_64-windows-dll");
    EXPECT_EQ(loaded.format->pe().machine, 0x8664u);
    EXPECT_EQ(loaded.format->pe().objectType, PeObjectType::Dll);
    // cl /LD ground truth: EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE |
    // IMAGE_FILE_DLL = 0x2022.
    EXPECT_EQ(loaded.format->pe().characteristics, 0x2022u);
    // MSVC dll conventions: ImageBase 0x180000000; DllCharacteristics
    // HIGH_ENTROPY_VA | DYNAMIC_BASE | NX_COMPAT = 0x160 (dumpbin
    // ground truth — no TERMINAL_SERVER_AWARE on DLLs).
    EXPECT_EQ(loaded.format->peOptionalHeader().imageBase,
              0x180000000ull);
    EXPECT_EQ(loaded.format->peOptionalHeader().dllCharacteristics,
              0x160u);
    // Entry-less library shape: no entry cluster, no entryPoint.
    EXPECT_FALSE(loaded.format->processExit().has_value());
    EXPECT_FALSE(loaded.format->processArgs().has_value());
    EXPECT_TRUE(loaded.format->entryCallingConvention().empty());
    EXPECT_TRUE(loaded.format->entryPoint().empty());
    // Serves the LIBRARY profiles (the .so mirror), not cli. `module` joined
    // `lib` here 2026-08-15: a module IS a library — it builds standalone like
    // any other library project and only its CONSUMPTION differs
    // (`SourceMerge` takes its sources and ignores the artifact), so it is
    // served by every format that serves `lib` or `staticlib`. Pinned as the
    // exact set, in order, so a future profile cannot be added here silently.
    ASSERT_EQ(loaded.format->artifactProfiles().size(), 2u);
    EXPECT_EQ(loaded.format->artifactProfiles()[0], "lib");
    EXPECT_EQ(loaded.format->artifactProfiles()[1], "module");
    // Image flavor, but NO deferred-import scope: Windows binds every
    // import at load from a NAMED module — a referenced no-library
    // extern still rejects loud at build time (the c143/c150 gate).
    EXPECT_TRUE(loaded.format->isImageFlavor());
    EXPECT_FALSE(loaded.format->allowsUndefinedImports());
    // Data sections: relro rides .rdata (c145); NO thread-locals
    // (D-LK-DLL-TLS-MODEL — the gate rejects by absence).
    EXPECT_TRUE(loaded.format->acceptsDataSection(DataSectionKind::RelRoConst));
    EXPECT_TRUE(loaded.format->acceptsDataSection(DataSectionKind::Data));
    EXPECT_FALSE(loaded.format->acceptsDataSection(DataSectionKind::Tdata));
    EXPECT_FALSE(loaded.format->acceptsDataSection(DataSectionKind::Tbss));
    EXPECT_FALSE(loaded.format->tlsAccess().has_value());
}

// ── (1) Header pins ──────────────────────────────────────────────

TEST(PeDllWriter, HeaderPinsDllFlagEntryZeroDynamicBase) {
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExportModule();
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);

    // IMAGE_FILE_HEADER.Characteristics = 0x2022: IMAGE_FILE_DLL
    // (0x2000) set, EXECUTABLE_IMAGE (0x0002) set.
    std::uint16_t const chars = readU16LE(img, 0x96);
    EXPECT_EQ(chars, 0x2022u);
    EXPECT_NE(chars & 0x2000u, 0u) << "IMAGE_FILE_DLL must be set";
    // AddressOfEntryPoint == 0: NO DllMain — the loader skips the
    // notification call (D-LK2-DLL-DLLMAIN-ENTRY is the follow-up).
    EXPECT_EQ(readU32LE(img, kEntryPointOff), 0u);
    // ImageBase = 0x180000000 (the dll preferred base the loader
    // rebases FROM — the DIR64 delta's subtrahend).
    EXPECT_EQ(readU64LE(img, kImageBaseOff), 0x180000000ull);
    // DllCharacteristics = 0x160 — DYNAMIC_BASE (0x0040) kept, so
    // ASLR exercises the .reloc machinery on every load.
    std::uint16_t const dllChars = readU16LE(img, kDllCharsOff);
    EXPECT_EQ(dllChars, 0x160u);
    EXPECT_NE(dllChars & 0x0040u, 0u) << "DYNAMIC_BASE must be set";
}

// ── (2) .edata export directory ──────────────────────────────────

TEST(PeDllWriter, ExportDirectoryWiredSortedAndOrdinalCorrect) {
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExportModule();   // names arrive z, a, m
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);

    // Data-directory[0] names the export block, whose span equals the
    // .edata payload (forwarder detection is RVA-range-based, so the
    // size must cover exactly the synthesized bytes).
    std::uint32_t const dirRva  = readU32LE(img, kExportDirOff);
    std::uint32_t const dirSize = readU32LE(img, kExportDirOff + 4);
    ASSERT_NE(dirRva, 0u);
    ASSERT_NE(dirSize, 0u);
    SectionView const edata = findSection(img, ".edata");
    ASSERT_TRUE(edata.found);
    EXPECT_EQ(dirRva, edata.virtualAddress);
    EXPECT_EQ(dirSize, edata.virtualSize);

    // IMAGE_EXPORT_DIRECTORY fields.
    std::size_t const dirOff = fileOff(edata, dirRva);
    EXPECT_EQ(readU32LE(img, dirOff + 16), 1u);   // Base (ordinal)
    std::uint32_t const numFuncs = readU32LE(img, dirOff + 20);
    std::uint32_t const numNames = readU32LE(img, dirOff + 24);
    // dss_zeta + dss_alpha + dss_mid + dss_global export;
    // hidden_helper (Local) must NOT.
    ASSERT_EQ(numFuncs, 4u);
    ASSERT_EQ(numNames, 4u);
    std::uint32_t const eatRva  = readU32LE(img, dirOff + 28);
    std::uint32_t const nameRva = readU32LE(img, dirOff + 32);
    std::uint32_t const ordRva  = readU32LE(img, dirOff + 36);

    // ★ THE SORT INVARIANT: the Name Pointer Table must be
    // lexicographically sorted (GetProcAddress binary-searches it;
    // input order was zeta, alpha, mid, global — red-on-disable: drop
    // the walker's sort and this assertion fails on the input order).
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < numNames; ++i) {
        std::uint32_t const nRva =
            readU32LE(img, fileOff(edata, nameRva) + 4u * i);
        names.push_back(readCStr(img, fileOff(edata, nRva)));
    }
    ASSERT_EQ(names.size(), 4u);
    EXPECT_TRUE(std::is_sorted(names.begin(), names.end()))
        << "Name Pointer Table must be lexicographically sorted";
    EXPECT_EQ(names[0], "dss_alpha");
    EXPECT_EQ(names[1], "dss_global");
    EXPECT_EQ(names[2], "dss_mid");
    EXPECT_EQ(names[3], "dss_zeta");
    for (auto const& n : names) EXPECT_NE(n, "hidden_helper");

    // Ordinal[i] -> EAT[ordinal] must land each name on ITS OWN
    // symbol's RVA (the functions were laid out zeta@+0, alpha@+1,
    // mid@+2 in .text at RVA 0x1000; a mis-permuted EAT/ordinal
    // mapping resolves the WRONG function silently).
    SectionView const text = findSection(img, ".text");
    ASSERT_TRUE(text.found);
    SectionView const dataSec = findSection(img, ".data");
    ASSERT_TRUE(dataSec.found);
    auto eatOf = [&](std::uint32_t nameIdx) {
        std::uint16_t const ord =
            readU16LE(img, fileOff(edata, ordRva) + 2u * nameIdx);
        return readU32LE(img, fileOff(edata, eatRva) + 4u * ord);
    };
    EXPECT_EQ(eatOf(0), text.virtualAddress + 1u);   // dss_alpha
    EXPECT_EQ(eatOf(2), text.virtualAddress + 2u);   // dss_mid
    EXPECT_EQ(eatOf(3), text.virtualAddress + 0u);   // dss_zeta
    // The DATA export's EAT RVA points INTO .data (an object export).
    std::uint32_t const globalRva = eatOf(1);        // dss_global
    EXPECT_GE(globalRva, dataSec.virtualAddress);
    EXPECT_LT(globalRva, dataSec.virtualAddress + dataSec.virtualSize);
    // No export RVA may fall inside the export block's own span (it
    // would be read as a FORWARDER string).
    for (std::uint32_t i = 0; i < numNames; ++i) {
        EXPECT_TRUE(eatOf(i) < dirRva || eatOf(i) >= dirRva + dirSize);
    }
}

TEST(PeDllWriter, DuplicateExportNameFailsLoud) {
    auto loaded = loadShippedDll();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    for (std::uint32_t s : {1u, 2u}) {
        AssembledFunction fn;
        fn.symbol = SymbolId{s};
        fn.bytes  = {0xC3};
        mod.functions.push_back(std::move(fn));
    }
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_dup",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "dss_dup",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(rep, "duplicate export name"));
}

TEST(PeDllWriter, NoExportsEmitsNoEdataAndZeroDirectory) {
    // A dll whose module carries no ModuleSymbol rows (hand-built
    // substrate) is legal PE: no .edata, zero export directory — the
    // gcc `-shared`-with-no-visible-symbols analog.
    auto loaded = loadShippedDll();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU32LE(img, kExportDirOff), 0u);
    EXPECT_EQ(readU32LE(img, kExportDirOff + 4), 0u);
    EXPECT_FALSE(findSection(img, ".edata").found);
}

// ── (3) .reloc completeness ──────────────────────────────────────

TEST(PeDllWriter, FnPtrTableSlotGetsDir64BaseRelocation) {
    auto loaded = loadShippedDll();
    AssembledModule mod = makeFnPtrTableModule();
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);

    // The relro table folds into .rdata (c145); it is the only rdata
    // item, so its slot sits at section offset 0.
    SectionView const rdata = findSection(img, ".rdata");
    ASSERT_TRUE(rdata.found);
    std::uint32_t const siteRva = rdata.virtualAddress + 0u;

    // The slot bytes hold the PREFERRED-BASE absolute VA of the
    // function (imageBase + textRva + 0) — the loader adjusts by the
    // load delta through the DIR64 row (NO base-0 trick: PE keeps its
    // preferred ImageBase, unlike the ELF dyn arm).
    SectionView const text = findSection(img, ".text");
    ASSERT_TRUE(text.found);
    std::uint64_t const slotValue =
        readU64LE(img, fileOff(rdata, siteRva));
    EXPECT_EQ(slotValue, 0x180000000ull + text.virtualAddress);

    // RED-ON-DISABLE: the exact DIR64 site row must exist and the
    // base-reloc directory must be wired.
    EXPECT_NE(readU32LE(img, kBaseRelocDirOff), 0u);
    EXPECT_NE(readU32LE(img, kBaseRelocDirOff + 4), 0u);
    auto const sites = readDir64Sites(img);
    EXPECT_TRUE(std::find(sites.begin(), sites.end(), siteRva)
                != sites.end())
        << "fn-ptr-table slot RVA 0x" << std::hex << siteRva
        << " must carry an IMAGE_REL_BASED_DIR64 entry";
}

// ── (4) Design c2 (D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB, the PE
// half): a slot holding an import's address is bound BY THE LOADER, through an
// import descriptor of its own. Every fact asserted by `expectLoaderBoundSlot` is
// one the Windows loader was MEASURED to need (pe.cpp states each at the pass
// that finds the slots); the runtime witness is the example
// `library_function_address_equals_getprocaddress`. ──

namespace {
void expectLoaderBoundSlot(std::vector<std::uint8_t> const& img, std::uint32_t slotRva,
                           std::string_view importName, std::string_view dllName) {
    SCOPED_TRACE(std::string{importName});
    auto const descs = readImportDescriptors(img);
    ImportDescriptorView const* mine = nullptr;
    for (auto const& d : descs) {
        if (d.firstThunk == slotRva) mine = &d;
    }
    ASSERT_NE(mine, nullptr) << "no import descriptor names the slot at RVA 0x" << std::hex
                             << slotRva << " as its FirstThunk: the loader never writes it";
    ASSERT_NE(mine->originalFirstThunk, 0u)
        << "a descriptor without a lookup array fails the load (0xC0000139, measured)";
    std::uint64_t const lookup = readU64AtRva(img, mine->originalFirstThunk);
    EXPECT_EQ(readU64AtRva(img, mine->originalFirstThunk + 8), 0u)
        << "the lookup array ends after its one entry";
    ASSERT_EQ(lookup >> 63, 0u) << "imported by name, not by ordinal";
    EXPECT_EQ(readCStrAtRva(img, static_cast<std::uint32_t>(lookup) + 2), importName);
    EXPECT_EQ(readCStrAtRva(img, mine->name), dllName);
    EXPECT_EQ(readU64AtRva(img, slotRva), lookup)
        << "until the loader binds it the slot holds its lookup entry (a zero word is left "
           "unbound, measured)";
    auto const sites = readDir64Sites(img);
    EXPECT_EQ(std::count(sites.begin(), sites.end(), slotRva), 0)
        << "a DIR64 row on a loader-bound slot would add the load delta to its lookup entry";
}
}  // namespace

TEST(PeDllWriter, AFunctionImportSlotIsBoundByTheLoaderThroughADescriptorOfItsOwn) {
    // The c112 `addr_import` shape (sqlite aSyscall[]) inside a DLL. It USED to
    // bake the image's own FF 25 thunk with a DIR64 row: callable, but not the
    // function's address, so `t[1] == puts` compared this image's thunk with the
    // library's function and answered false.
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    SectionView const dataSec = findSection(img, ".data");
    ASSERT_TRUE(dataSec.found) << "a WRITABLE slot stays in .data";
    expectLoaderBoundSlot(img, dataSec.virtualAddress + 8u, "puts", "msvcrt.dll");
    EXPECT_EQ(img[fileOff(dataSec, dataSec.virtualAddress)], 0x11u);
    EXPECT_EQ(img[fileOff(dataSec, dataSec.virtualAddress + 16u)], 0x22u);
    // The library's own descriptor comes first and its IAT opens the IAT
    // directory; the slot's descriptor follows it.
    auto const descs = readImportDescriptors(img);
    ASSERT_EQ(descs.size(), 2u) << "the library's descriptor and the slot's own";
    EXPECT_EQ(descs[0].firstThunk, readU32LE(img, kIatDirOff));
}

TEST(PeExecWriterExternSlot, AFunctionImportSlotIsBoundByTheLoaderOnExecToo) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false);
    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    SectionView const dataSec = findSection(img, ".data");
    ASSERT_TRUE(dataSec.found);
    expectLoaderBoundSlot(img, dataSec.virtualAddress + 8u, "puts", "msvcrt.dll");
}

TEST(PeDllWriter, ADataImportSlotIsBoundByTheLoaderToo) {
    // D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR, THE PE ARM: `int *p = &_fmode;` as a
    // static initializer was REFUSED — a slot filled by a link-time relocation
    // could only have held the IAT slot's own address, one indirection off. The
    // loader writes the datum's address into a slot of its own.
    for (bool const exec : {false, true}) {
        SCOPED_TRACE(exec ? "exec" : "dll");
        auto loaded = exec ? loadShippedExec() : loadShippedDll();
        AssembledModule mod = makeExternSlotModule(/*externIsData=*/true);
        DiagnosticReporter rep;
        auto img = exec ? encodeUntrampolined(mod, *loaded.target, *loaded.format, rep)
                        : pe::encode(mod, *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        SectionView const dataSec = findSection(img, ".data");
        ASSERT_TRUE(dataSec.found);
        expectLoaderBoundSlot(img, dataSec.virtualAddress + 8u, "_fmode", "msvcrt.dll");
    }
}

TEST(PeDllWriter, AReadOnlySlotLiesUnderTheIatDirectoryAndIdataIsReadOnly) {
    // A `const` table of import addresses — and the GOT slot the link mints for
    // `&puts` in code — is READ-ONLY, so it must lie inside the IAT directory,
    // the range the loader unprotects while it binds: outside it the load
    // crashed with 0xC0000005, inside it the slot was bound and read
    // PAGE_READONLY afterwards (measured). The item leaves `.rdata` for `.idata`,
    // which is itself read-only now.
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false,
                                               DataSectionKind::RelRoConst);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(findSection(img, ".rdata").found) << "the only read-only item moved to .idata";
    auto const descs = readImportDescriptors(img);
    ASSERT_EQ(descs.size(), 2u);
    std::uint32_t const slotRva = descs[1].firstThunk;
    std::uint32_t const iatRva = readU32LE(img, kIatDirOff);
    std::uint32_t const iatSize = readU32LE(img, kIatDirOff + 4);
    EXPECT_GE(slotRva, iatRva);
    EXPECT_LE(slotRva + 8u, iatRva + iatSize) << "a read-only slot must lie inside the IAT directory";
    expectLoaderBoundSlot(img, slotRva, "puts", "msvcrt.dll");
    SectionView const home = sectionHolding(img, slotRva);
    EXPECT_EQ(img[fileOff(home, slotRva - 8u)], 0x11u) << "the whole item moved, not just its slot";
    EXPECT_EQ(img[fileOff(home, slotRva + 8u)], 0x22u);
    EXPECT_EQ(sectionCharacteristics(img, ".idata"), 0x40000040u)
        << "IMAGE_SCN_CNT_INITIALIZED_DATA | MEM_READ, from the document's `dynamic` row: no MEM_WRITE";

    // ★ THE ITEM IS EXPORTED (`dss_slot`, Global), and `.edata` is laid out
    // BEFORE `.idata`: its EAT entry is written once `.idata` is placed. It
    // must name the item's NEW home — the item starts 8 bytes before its slot —
    // or GetProcAddress hands the caller the RVA the item would have had.
    std::uint32_t const dirRva = readU32LE(img, kExportDirOff);
    ASSERT_NE(dirRva, 0u);
    SectionView const edata = sectionHolding(img, dirRva);
    std::size_t const dirOff = fileOff(edata, dirRva);
    std::uint32_t const numNames = readU32LE(img, dirOff + 24);
    std::uint32_t const eatRva   = readU32LE(img, dirOff + 28);
    std::uint32_t const nameRva  = readU32LE(img, dirOff + 32);
    std::uint32_t const ordRva   = readU32LE(img, dirOff + 36);
    std::optional<std::uint32_t> slotItemRva;
    for (std::uint32_t i = 0; i < numNames; ++i) {
        std::uint32_t const nRva = readU32LE(img, fileOff(edata, nameRva) + 4u * i);
        if (readCStr(img, fileOff(edata, nRva)) != "dss_slot") continue;
        std::uint16_t const ord = readU16LE(img, fileOff(edata, ordRva) + 2u * i);
        slotItemRva = readU32LE(img, fileOff(edata, eatRva) + 4u * ord);
    }
    ASSERT_TRUE(slotItemRva.has_value()) << "`dss_slot` must be exported";
    EXPECT_EQ(*slotItemRva, slotRva - 8u)
        << "the export must name the item where it now lies, inside `.idata`";
}

// ── (4b) What design c2 cannot express, because the loader writes an import's
// address and nothing else: that address PLUS an addend (`&arr[3]` of a DLL
// datum), and any such slot in a thread-local TEMPLATE (the loader copies the
// starting thread's block before it binds). The image's RESIDUE RUNNER writes
// them at load (P69 review M1 (a)/(b), D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE): a
// synthesized function the TLS directory names as its FIRST callback, run on
// DLL_PROCESS_ATTACH after the loader has bound the IAT and before any other
// code of the image. Pinned here on hand-built modules: the callback array,
// what the runner reads and where it writes, the file word of a residue slot,
// and that a read-only residue item is laid out writable. The run is
// `library_function_address_equals_getprocaddress`. ──

namespace {

constexpr std::size_t kTlsDirOff = kDataDirOff + 9 * 8;

// The TLS directory's callback array, read back: every entry up to the null.
struct TlsCallbacksView {
    std::uint32_t              dirRva   = 0;
    std::uint32_t              arrayRva = 0;
    std::vector<std::uint64_t> entries;
};
[[nodiscard]] TlsCallbacksView readTlsCallbacks(std::vector<std::uint8_t> const& img) {
    TlsCallbacksView out;
    out.dirRva = readU32LE(img, kTlsDirOff);
    if (out.dirRva == 0) return out;
    std::uint64_t const imageBase = readU64LE(img, kImageBaseOff);
    std::uint64_t const array = readU64AtRva(img, out.dirRva + 24u);   // AddressOfCallBacks
    if (array == 0) return out;
    out.arrayRva = static_cast<std::uint32_t>(array - imageBase);
    for (std::uint32_t at = out.arrayRva;; at += 8u) {
        std::uint64_t const entry = readU64AtRva(img, at);
        if (entry == 0) break;
        out.entries.push_back(entry);
    }
    return out;
}

// The runner's first instructions, as the x86_64 target encodes them: a 32-bit
// `cmp edx, 1` (the callback's `reason` against DLL_PROCESS_ATTACH), then the
// two-target `jne done` (`0F 85 rel32` + `E9 rel32`), then the first fix-up's
// `lea rax, [rip + IAT entry]`. Returns the RVA that lea names, or nullopt when
// the bytes are not that shape.
[[nodiscard]] std::optional<std::uint32_t>
runnerFirstSourceSlotRva(std::vector<std::uint8_t> const& img, std::uint32_t runnerRva) {
    SectionView const text = sectionHolding(img, runnerRva);
    std::size_t const at = fileOff(text, runnerRva);
    std::array<std::uint8_t, 6> const cmpEdx1{0x81, 0xFA, 0x01, 0x00, 0x00, 0x00};
    for (std::size_t i = 0; i < cmpEdx1.size(); ++i) {
        if (img[at + i] != cmpEdx1[i]) return std::nullopt;
    }
    if (img[at + 6] != 0x0F || img[at + 7] != 0x85 || img[at + 12] != 0xE9) return std::nullopt;
    std::size_t const lea = at + 17;
    if (img[lea] != 0x48 || img[lea + 1] != 0x8D || (img[lea + 2] & 0xC7) != 0x05) {
        return std::nullopt;
    }
    auto const disp = static_cast<std::int32_t>(readU32LE(img, lea + 3));
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(runnerRva) + 17 + 7 + disp);
}

}  // namespace

TEST(PeDllWriter, ADataImportSlotWithAnAddendIsWrittenByTheResidueRunner) {
    for (bool const exec : {false, true}) {
        SCOPED_TRACE(exec ? "exec" : "dll");
        auto loaded = exec ? loadShippedExec() : loadShippedDll();
        AssembledModule mod = makeExternSlotModule(/*externIsData=*/true,
                                                   DataSectionKind::Data, /*addend=*/24);
        DiagnosticReporter rep;
        auto img = exec ? encodeUntrampolined(mod, *loaded.target, *loaded.format, rep)
                        : pe::encode(mod, *loaded.target, *loaded.format, rep);
        ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
        EXPECT_EQ(rep.errorCount(), 0u);
        SectionView const text = findSection(img, ".text");
        SectionView const dataSec = findSection(img, ".data");
        ASSERT_TRUE(text.found);
        ASSERT_TRUE(dataSec.found);
        std::uint32_t const siteRva = dataSec.virtualAddress + 8u;
        // The file word: 0 — a datum's address plus 24 is known only at load.
        EXPECT_EQ(readU64LE(img, fileOff(dataSec, siteRva)), 0u);
        auto const sites = readDir64Sites(img);
        EXPECT_EQ(std::count(sites.begin(), sites.end(), siteRva), 0)
            << "a DIR64 row would add the load delta to a word the runner overwrites";
        auto const descs = readImportDescriptors(img);
        ASSERT_EQ(descs.size(), 1u) << "no descriptor of its own: the loader cannot bind S+A";
        // The runner: the ONE TLS callback, right after the module's 1-byte function.
        auto const cbs = readTlsCallbacks(img);
        ASSERT_NE(cbs.dirRva, 0u) << "an image with residue carries a TLS directory";
        ASSERT_EQ(cbs.entries.size(), 1u) << "the residue runner, then the null";
        std::uint64_t const imageBase = readU64LE(img, kImageBaseOff);
        std::uint32_t const runnerRva = static_cast<std::uint32_t>(cbs.entries[0] - imageBase);
        EXPECT_EQ(runnerRva, text.virtualAddress + 1u);
        EXPECT_EQ(std::count(sites.begin(), sites.end(), cbs.dirRva + 24u), 1)
            << "AddressOfCallBacks is a pointer into the image";
        EXPECT_EQ(std::count(sites.begin(), sites.end(), cbs.arrayRva), 1)
            << "and so is the runner's entry in the array";
        // What it reads: the import's IAT entry — the library descriptor's FirstThunk,
        // `_fmode` being its one import.
        auto const source = runnerFirstSourceSlotRva(img, runnerRva);
        ASSERT_TRUE(source.has_value()) << "the runner opens with cmp edx,1 / jne / lea rax,[rip+..]";
        EXPECT_EQ(*source, descs[0].firstThunk) << "the fix-up starts from the bound IAT entry";
    }
}

TEST(PeDllWriter, AFunctionImportSlotWithAnAddendHoldsItsThunkInTheFileAndIsRewritten) {
    // A FUNCTION's residue slot keeps its thunk (+A) with its DIR64 row in the FILE —
    // callable even in a thread that copied a template before the runner ran — and the
    // runner overwrites it with the bound address + A at load.
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false,
                                               DataSectionKind::Data, /*addend=*/4);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    SectionView const text = findSection(img, ".text");
    SectionView const dataSec = findSection(img, ".data");
    ASSERT_TRUE(text.found);
    ASSERT_TRUE(dataSec.found);
    auto const cbs = readTlsCallbacks(img);
    ASSERT_EQ(cbs.entries.size(), 1u) << "the residue runner";
    std::uint64_t const imageBase = readU64LE(img, kImageBaseOff);
    std::uint32_t const runnerRva = static_cast<std::uint32_t>(cbs.entries[0] - imageBase);
    std::uint32_t const iatEntryRva = readImportDescriptors(img)[0].firstThunk;
    // The file word: the import THUNK + 4 — `FF 25 rel32`, a jmp through puts's IAT entry.
    std::uint32_t const siteRva = dataSec.virtualAddress + 8u;
    std::uint64_t const fileWord = readU64LE(img, fileOff(dataSec, siteRva));
    std::uint32_t const thunkRva = static_cast<std::uint32_t>(fileWord - 4u - imageBase);
    std::size_t const thunkOff = fileOff(text, thunkRva);
    ASSERT_EQ(img[thunkOff], 0xFFu);
    ASSERT_EQ(img[thunkOff + 1], 0x25u);
    EXPECT_EQ(thunkRva + 6u + readU32LE(img, thunkOff + 2), iatEntryRva)
        << "the file word is the thunk through puts's IAT entry, plus 4";
    auto const sites = readDir64Sites(img);
    EXPECT_EQ(std::count(sites.begin(), sites.end(), siteRva), 1);
    for (auto const& d : readImportDescriptors(img)) EXPECT_NE(d.firstThunk, siteRva);
    auto const source = runnerFirstSourceSlotRva(img, runnerRva);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(*source, iatEntryRva);
}

TEST(PeDllWriter, AReadOnlyResidueItemIsLaidOutWritable) {
    // The runner writes after the loader has sealed `.rdata` and `.idata`, so a const
    // table holding `&arr[1]` of a DLL datum lives in `.data` — neither where c2's
    // read-only slots go (`.idata`) nor in `.rdata`.
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/true,
                                               DataSectionKind::RelRoConst, /*addend=*/4);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    EXPECT_FALSE(findSection(img, ".rdata").found) << "the only read-only item moved";
    SectionView const dataSec = findSection(img, ".data");
    ASSERT_TRUE(dataSec.found) << "to WRITABLE .data";
    EXPECT_NE(sectionCharacteristics(img, ".data") & 0x80000000u, 0u) << "IMAGE_SCN_MEM_WRITE";
    EXPECT_EQ(img[fileOff(dataSec, dataSec.virtualAddress)], 0x11u) << "the whole item moved";
    EXPECT_EQ(img[fileOff(dataSec, dataSec.virtualAddress + 16u)], 0x22u);
    EXPECT_EQ(readTlsCallbacks(img).entries.size(), 1u);
}

TEST(PeExecWriterExternSlot, AThreadLocalTemplateSlotIsWrittenByTheRunnerInTheTemplateAndTheThread) {
    // `_Thread_local put_fn f = puts;` — the loader copies the main thread's block from
    // the template BEFORE it binds (measured, variant T1), so no loader-bound slot can
    // serve it: the runner writes the TEMPLATE (every later thread copies it) and the
    // RUNNING thread's own copy, reached through gs:[0x58][_tls_index].
    auto loaded = loadShippedExec();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false, DataSectionKind::Tdata,
                                               /*addend=*/0);
    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    auto const cbs = readTlsCallbacks(img);
    ASSERT_EQ(cbs.entries.size(), 1u);
    std::uint32_t const runnerRva =
        static_cast<std::uint32_t>(cbs.entries[0] - readU64LE(img, kImageBaseOff));
    SectionView const text = sectionHolding(img, runnerRva);
    std::size_t const begin = fileOff(text, runnerRva);
    // `mov r, gs:[0x58]` — the format's tlsAccess slot read: 65 4x 8B /r with an
    // absolute SIB (04|r<<3, 25) and disp32 0x58.
    bool sawTebRead = false;
    for (std::size_t p = begin; p + 9 <= begin + 200 && p + 9 <= img.size(); ++p) {
        if (img[p] == 0x65 && (img[p + 1] & 0xF0) == 0x40 && img[p + 2] == 0x8B
            && (img[p + 3] & 0xC7) == 0x04 && img[p + 4] == 0x25 && readU32LE(img, p + 5) == 0x58) {
            sawTebRead = true;
        }
    }
    EXPECT_TRUE(sawTebRead) << "the runner reaches the running thread's block through gs:[0x58]";
    auto const source = runnerFirstSourceSlotRva(img, runnerRva);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(*source, readImportDescriptors(img)[0].firstThunk);
}

TEST(PeExecWriterExternSlot, AnImageWithNoResidueCarriesNoRunner) {
    // Byte-identity for every other image: no residue, no runner, no callback array —
    // and no TLS directory at all for an image with no thread-local data.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false);
    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << diagnosticsOf(rep);
    EXPECT_EQ(readU32LE(img, kTlsDirOff), 0u);
    EXPECT_FALSE(findSection(img, ".tls").found);
}

// ── (5) The remaining fail-loud belts ────────────────────────────

TEST(PeDllWriter, AbsoluteTextRelocIsCollectedIntoDotReloc) {
    // D-LK-PE-IMAGE-TEXT-ABS-RELOC — CLOSED. An 8-byte absolute fixup in
    // `.text` is no longer refused: the walker collects its site into `.reloc`,
    // so the loader adjusts the baked VA on rebase exactly as it adjusts a data
    // pointer.
    //
    // ★★ THIS TEST USED TO ASSERT THE REFUSAL, and the refusal was correct
    // until a producer appeared. One did: an ordinary `gcc -c -O2` object whose
    // function carries `movabsq $g, %rax` emits `IMAGE_REL_AMD64_ADDR64` into
    // `.text`, native gcc + ld link it into a binary that RUNS to exit 42, and
    // DSS ingests such objects through the shipped `--resolve-library`
    // static-link path. Refusing what a reference toolchain accepts puts DSS
    // below `(gcc ∪ clang ∪ MSVC) ∪ ISO C`, so collecting is the close and
    // rejecting is no longer available.
    //
    // ★ THE ASSERTION IS THE EMITTED TABLE, not the absence of a diagnostic.
    // `readDir64Sites` re-decodes `.reloc` from the image bytes independently of
    // the code that wrote it, and the expected RVA is computed from the
    // instruction layout: the `movabs` opcode is 2 bytes, so its imm64 operand
    // — the slot the loader must adjust — begins at `.text` + 2.
    auto loaded = loadShippedDll();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3}; // mov rax, imm64
    Relocation rel;
    rel.offset = 2;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{2};   // abs64 — Linear, !pcRelative, width 8
    rel.addend = 0;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty()) << "an absolute in .text must now LINK";
    EXPECT_EQ(rep.errorCount(), 0u);

    SectionView const text = findSection(img, ".text");
    ASSERT_TRUE(text.found);
    std::uint32_t const wantRva = text.virtualAddress + 2u;
    auto const sites = readDir64Sites(img);
    EXPECT_NE(std::find(sites.begin(), sites.end(), wantRva), sites.end())
        << "the imm64 slot at .text+2 (RVA " << wantRva << ") must carry an "
           "IMAGE_REL_BASED_DIR64 row, or a rebased image keeps the "
           "preferred-base address in code the loader never adjusts";
}

TEST(PeDllWriter, AbsoluteTextRelocOfUnrepresentableWidthStillFailsLoud) {
    // ★★★ THE BELT WAS NARROWED, NOT DELETED, AND THIS IS ITS REMAINING
    // TRIGGER. `IMAGE_REL_BASED_DIR64` is the only base-relocation form this
    // writer emits, so an absolute of any OTHER width has no representation in
    // `.reloc`. Collecting it is impossible and shipping it uncollected is the
    // silent wrong-address the belt exists to stop — it must still refuse.
    // Without this case the belt would have no reachable trigger left, and a
    // guard nothing can fire is a guard the next reader deletes as dead code.
    auto loaded = loadShippedDll();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xB8, 0, 0, 0, 0, 0xC3};   // mov eax, imm32
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{3};   // abs32 — !pcRelative, !tls, width 4
    rel.addend = 0;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(
        rep, "D-LK-PE-IMAGE-TEXT-ABS-RELOC"));
}

TEST(PeDllWriter, ImageEntryOverrideFailsLoud) {
    // A dll has no image entry; a caller-provided trampoline override
    // is a producer-contract breach (the linker never injects one for
    // a schema without processExit).
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExportModule();
    mod.imageEntryOverride = 0u;
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(rep, "imageEntryOverride"));
}

// ── (6) validate() shape rules ───────────────────────────────────

namespace {
// A minimal dll JSON with a splice point for extra top-level fields.
[[nodiscard]] std::string dllJsonWith(std::string_view extraTopLevel,
                                      std::string_view characteristics
                                          = "8226") {
    std::string s = R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "ms_x64" },
      "outputExtension": ".dll",
      "dataModel": "LLP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"t-dll","kind":"pe"},
      )";
    s += extraTopLevel;
    s += R"(
      "pe": { "machine": 34404, "characteristics": )";
    s += characteristics;
    s += R"(, "type": "dll" },
      "optionalHeader": { "magic": 523, "imageBase": 6442450944, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 2, "dllCharacteristics": 352, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })";
    return s;
}
} // namespace

TEST(PeDllFormatJsonValidate, MinimalDllShapeAccepted) {
    auto r = ObjectFormatSchema::loadFromText(dllJsonWith(""));
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ((*r)->pe().objectType, PeObjectType::Dll);
}

TEST(PeDllFormatJsonValidate, EntryClusterRejected) {
    // A dll schema declaring processExit (+ its paired cc) is the
    // DllMain-shaped config this cycle does NOT ship — reject loud
    // (D-LK2-DLL-DLLMAIN-ENTRY is the pinned follow-up).
    auto r = ObjectFormatSchema::loadFromText(dllJsonWith(R"(
      "entryCallingConvention": "ms_x64",
      "entryTransition": "called",
      "runtimeLibraries": [{"role":"cLibrary","image":"kernel32.dll"}],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "ExitProcess" },
    )"));
    ASSERT_FALSE(r.has_value());
}

TEST(PeDllFormatJsonValidate, EntryPointRejected) {
    auto r = ObjectFormatSchema::loadFromText(dllJsonWith(R"(
      "entryPoint": "DllMain",
    )"));
    ASSERT_FALSE(r.has_value());
}

TEST(PeDllFormatJsonValidate, MissingImageFileDllBitRejected) {
    // characteristics 0x0022 (exec-shaped) on a dll schema: the
    // loader would refuse LoadLibrary semantics — reject at validate.
    auto r = ObjectFormatSchema::loadFromText(dllJsonWith("", "34"));
    ASSERT_FALSE(r.has_value());
}

TEST(PeDllFormatJsonValidate, ExecWithImageFileDllBitRejected) {
    // The symmetric copy-paste guard: 0x2022 on an EXEC schema.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "cCallingConvention": { "convention": "ms_x64" },
      "outputExtension": ".exe",
      "dataModel": "LLP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"t-exe","kind":"pe"},
      "$entryClusterComment": "D-LK10-ENTRY 2.13: validate() REJECTS an exec-flavored format declaring no processExit, and this fixture is pe.type=exec -- without the pair it would be rejected for a reason unrelated to the IMAGE_FILE_DLL bit it exists to pin. Verbatim from the shipped pe64-x86_64-windows-exec.format.json; inert here (a load test builds no trampoline).",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "entryTransition": "called",
      "pe": { "machine": 34404, "characteristics": 8226, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "dllCharacteristics": 33120, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    // Pin THE diagnostic, not merely "something was wrong": `validate()`
    // accumulates, so a bare has_value() check stays green when a future
    // rule rejects this fixture for an unrelated reason -- which is how
    // D-LK10-ENTRY 2.13 silently emptied this family once already. The
    // `/processExit` zero-count is that anti-subsumption half.
    EXPECT_EQ(countAtPath(r, "/pe/characteristics"), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

// ── (7) Exec byte-surface guard ──────────────────────────────────

TEST(PeExecWriterExportGuard, ExecImageKeepsZeroExportDirectoryAndNoEdata) {
    // The dll machinery must be invisible on the exec arm: export
    // directory zero, no .edata section (the pre-c152 byte surface).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeExportModule();   // symbols present!
    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU32LE(img, kExportDirOff), 0u);
    EXPECT_EQ(readU32LE(img, kExportDirOff + 4), 0u);
    EXPECT_FALSE(findSection(img, ".edata").found);
}
