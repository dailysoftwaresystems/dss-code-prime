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
//     the expected site RVA and finds the exact entry); a
//     FUNCTION-extern-targeted slot legally bakes the import-THUNK VA
//     (the c112 addr_import shape) and gets its DIR64 row.
//   * Fail-loud belts: a data slot targeting an extern DATA import
//     rejects naming D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR (the registered
//     c150-CRITICAL PE half — pinned on the dll AND exec arms); an
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

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

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
// selectable): the D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR shapes.
[[nodiscard]] AssembledModule makeExternSlotModule(bool externIsData) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData slot;
    slot.symbol    = SymbolId{5};
    slot.section   = DataSectionKind::Data;
    slot.bytes     = std::vector<std::uint8_t>(8, 0);
    slot.alignment = Alignment::of<8>();
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{2};   // abs64
    rel.addend = 0;
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
    // Serves the lib profile (the .so mirror), not cli.
    ASSERT_EQ(loaded.format->artifactProfiles().size(), 1u);
    EXPECT_EQ(loaded.format->artifactProfiles()[0], "lib");
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

TEST(PeDllWriter, FunctionExternSlotBakesThunkVaAndGetsDir64) {
    // The c112 `addr_import` shape (sqlite aSyscall[]) inside a DLL:
    // a data slot targeting a FUNCTION extern legally bakes the FF 25
    // import-THUNK VA (call-correct; MSVC's own `&puts` binds the
    // local thunk) and the slot gets its DIR64 row so a rebase keeps
    // it callable. This is the LEGAL half of
    // D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR.
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/false);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);

    SectionView const text = findSection(img, ".text");
    ASSERT_TRUE(text.found);
    SectionView const dataSec = findSection(img, ".data");
    ASSERT_TRUE(dataSec.found);
    // One 1-byte function then the thunk block: thunk VA = imageBase
    // + textRva + 1.
    std::uint64_t const thunkVa = 0x180000000ull + text.virtualAddress + 1u;
    std::uint32_t const siteRva = dataSec.virtualAddress + 0u;
    EXPECT_EQ(readU64LE(img, fileOff(dataSec, siteRva)), thunkVa);
    auto const sites = readDir64Sites(img);
    EXPECT_TRUE(std::find(sites.begin(), sites.end(), siteRva)
                != sites.end());
}

// ── (4) D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR: the DATA-extern reject ──

TEST(PeDllWriter, DataItemRelocTargetingExternDataFailsLoud) {
    auto loaded = loadShippedDll();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/true);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(
        rep, "D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR"));
    EXPECT_TRUE(sawDiagnosticContaining(rep, "_fmode"));
}

TEST(PeExecWriterExternSlot, DataItemRelocTargetingExternDataFailsLoudOnExecToo) {
    // The registered anchor is the PE IMAGE walker's (the exec arm has
    // the identical latent bake since c149) — pin the reject on exec.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeExternSlotModule(/*externIsData=*/true);
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GT(rep.errorCount(), 0u);
    EXPECT_TRUE(sawDiagnosticContaining(
        rep, "D-LK-IMAGE-DATA-SLOT-EXTERN-ADDR"));
}

// ── (5) The remaining fail-loud belts ────────────────────────────

TEST(PeDllWriter, AbsoluteTextRelocFailsLoud) {
    // D-LK-PE-IMAGE-TEXT-ABS-RELOC: an absolute fixup in `.text` of a
    // DYNAMIC_BASE image has no `.reloc` site collector — rejecting
    // beats shipping a preferred-base address the loader never
    // adjusts.
    auto loaded = loadShippedDll();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xC3}; // mov rax, imm64
    Relocation rel;
    rel.offset = 2;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{2};   // abs64 — Linear, !pcRelative
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
      "dataModel": "LLP64",
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
      "processExit": { "mechanism": "by-name-import", "importLibraryPath": "kernel32.dll", "importMangledName": "ExitProcess" },
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
      "dataModel": "LLP64",
      "format": {"name":"t-exe","kind":"pe"},
      "pe": { "machine": 34404, "characteristics": 8226, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "dllCharacteristics": 33120, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── (7) Exec byte-surface guard ──────────────────────────────────

TEST(PeExecWriterExportGuard, ExecImageKeepsZeroExportDirectoryAndNoEdata) {
    // The dll machinery must be invisible on the exec arm: export
    // directory zero, no .edata section (the pre-c152 byte surface).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeExportModule();   // symbols present!
    DiagnosticReporter rep;
    auto img = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_FALSE(img.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU32LE(img, kExportDirOff), 0u);
    EXPECT_EQ(readU32LE(img, kExportDirOff + 4), 0u);
    EXPECT_FALSE(findSection(img, ".edata").found);
}
