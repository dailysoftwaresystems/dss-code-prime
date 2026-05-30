// PE/COFF .obj writer tests — plan 14 LK2 cycle 1.
//
// Pins golden byte-level invariants of the emitted PE/COFF `.obj`:
//   * IMAGE_FILE_HEADER Machine = IMAGE_FILE_MACHINE_AMD64 (0x8664).
//   * NumberOfSections = 1, SizeOfOptionalHeader = 0 for .obj.
//   * Single IMAGE_SECTION_HEADER for `.text` with Characteristics
//     = 0x60500020 (CODE | ALIGN_16BYTES | EXECUTE | READ).
//   * IMAGE_SYMBOL records are 18 bytes packed, no padding.
//   * IMAGE_RELOCATION records are 10 bytes packed; r_info high 32
//     bits are NOT used (different from ELF) — SymbolTableIndex is
//     a separate u32 + Type a separate u16.
//   * `r_addend` does NOT exist in PE — addend is the bytes at the
//     patch site within `.text`.
//   * String table has 4-byte u32 size prefix.
//
// Also pins that the shipped `pe64-x86_64-windows.format.json`
// loads cleanly via `loadShipped`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/pe.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::uint16_t readU16LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    return static_cast<std::uint16_t>(b[off])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] std::uint32_t readU32LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b[off + i]) << (i * 8);
    return v;
}
[[nodiscard]] std::int16_t readI16LE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    return static_cast<std::int16_t>(readU16LE(b, off));
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

[[nodiscard]] Loaded loadShipped() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(pe64-x86_64-windows) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

[[nodiscard]] AssembledModule makeTrivialModule(std::vector<std::uint8_t> bytes,
                                                  std::uint32_t symId) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{symId};
    fn.bytes = std::move(bytes);
    mod.functions.push_back(std::move(fn));
    return mod;
}

} // namespace

// ── Shipped JSON loads ───────────────────────────────────────────

TEST(PeFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Pe);
    EXPECT_EQ(loaded.format->name(), "pe64-x86_64-windows");
    EXPECT_EQ(loaded.format->pe().machine, 0x8664u);
    EXPECT_EQ(loaded.format->pe().characteristics, 0u);
    EXPECT_NE(loaded.format->sectionByKind(SectionKind::Text), nullptr);
    // PE has NO IMAGE_SECTION_HEADER for symbol/string tables — they
    // live at IMAGE_FILE_HEADER.PointerToSymbolTable + immediately
    // after. Shipped JSON correctly omits the rows; PE walker uses
    // optional lookup (architect convergence: prior cycle erroneously
    // required them via `requireSection`, breaking legitimate PE
    // JSONs).
    EXPECT_EQ(loaded.format->sectionByKind(SectionKind::Symtab), nullptr);
    EXPECT_EQ(loaded.format->sectionByKind(SectionKind::Strtab), nullptr);
    auto const* pc32 =
        loaded.format->relocationByKind(RelocationKind{1});
    ASSERT_NE(pc32, nullptr);
    EXPECT_EQ(pc32->name, "IMAGE_REL_AMD64_REL32");
    EXPECT_EQ(pc32->nativeId, 4u);
}

// ── IMAGE_FILE_HEADER golden bytes ──────────────────────────────

TEST(PeWriter, FileHeaderMatchesPECoffSpec) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GE(bytes.size(), 20u);
    EXPECT_EQ(rep.errorCount(), 0u);

    // Machine = IMAGE_FILE_MACHINE_AMD64
    EXPECT_EQ(readU16LE(bytes, 0), 0x8664u);
    // NumberOfSections = 1 (.text only)
    EXPECT_EQ(readU16LE(bytes, 2), 1u);
    // TimeDateStamp = 0 (deterministic)
    EXPECT_EQ(readU32LE(bytes, 4), 0u);
    // SizeOfOptionalHeader = 0 (.obj, not .exe)
    EXPECT_EQ(readU16LE(bytes, 16), 0u);
    // Characteristics = 0 (relocatable .obj)
    EXPECT_EQ(readU16LE(bytes, 18), 0u);
}

// ── Section header: .text Characteristics ──────────────────────

TEST(PeWriter, TextSectionCharacteristicsMatchesMsvcConvention) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GE(bytes.size(), 60u);
    EXPECT_EQ(rep.errorCount(), 0u);

    // First IMAGE_SECTION_HEADER starts at byte 20.
    // Name[8] = ".text\0\0\0"
    EXPECT_EQ(bytes[20], '.');
    EXPECT_EQ(bytes[21], 't');
    EXPECT_EQ(bytes[22], 'e');
    EXPECT_EQ(bytes[23], 'x');
    EXPECT_EQ(bytes[24], 't');
    EXPECT_EQ(bytes[25], 0u);
    EXPECT_EQ(bytes[26], 0u);
    EXPECT_EQ(bytes[27], 0u);
    // VirtualSize @ +8 = 0 for .obj
    EXPECT_EQ(readU32LE(bytes, 28), 0u);
    // VirtualAddress @ +12 = 0 for .obj
    EXPECT_EQ(readU32LE(bytes, 32), 0u);
    // SizeOfRawData @ +16 = byte length of .text (1: just 0xC3)
    EXPECT_EQ(readU32LE(bytes, 36), 1u);
    // PointerToLinenumbers @ +28 = 0
    EXPECT_EQ(readU32LE(bytes, 48), 0u);
    // NumberOfRelocations @ +32 = 0 (no relocs in this test)
    EXPECT_EQ(readU16LE(bytes, 52), 0u);
    // NumberOfLinenumbers @ +34 = 0
    EXPECT_EQ(readU16LE(bytes, 54), 0u);
    // Characteristics @ +36 = 0x60500020
    EXPECT_EQ(readU32LE(bytes, 56), 0x60500020u);
}

// ── Symbol record layout ────────────────────────────────────────

TEST(PeWriter, SymbolRecordsAre18BytesPackedNoPadding) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    // PointerToSymbolTable lives at IMAGE_FILE_HEADER + 8.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    std::uint32_t const numSyms = readU32LE(bytes, 12);
    ASSERT_EQ(numSyms, 1u) << "one function => one symbol";
    ASSERT_LE(symPtr + 18u, bytes.size());

    // Symbol 0: Name field = "sym_7\0\0\0" (5 chars NUL-padded).
    EXPECT_EQ(bytes[symPtr + 0], 's');
    EXPECT_EQ(bytes[symPtr + 1], 'y');
    EXPECT_EQ(bytes[symPtr + 2], 'm');
    EXPECT_EQ(bytes[symPtr + 3], '_');
    EXPECT_EQ(bytes[symPtr + 4], '7');
    EXPECT_EQ(bytes[symPtr + 5], 0u);
    // Value @ +8 = 0 (function at start of .text)
    EXPECT_EQ(readU32LE(bytes, symPtr + 8), 0u);
    // SectionNumber @ +12 = 1 (.text is section 1)
    EXPECT_EQ(readI16LE(bytes, symPtr + 12), 1);
    // Type @ +14 = 0x0020 (DT_FUNCTION)
    EXPECT_EQ(readU16LE(bytes, symPtr + 14), 0x0020u);
    // StorageClass @ +16 = 2 (IMAGE_SYM_CLASS_EXTERNAL)
    EXPECT_EQ(bytes[symPtr + 16], 2u);
    // NumberOfAuxSymbols @ +17 = 0
    EXPECT_EQ(bytes[symPtr + 17], 0u);
}

// ── String table starts with 4-byte u32 size including itself ──

TEST(PeWriter, StringTableHasSizePrefixAndIncludesItself) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    // String table immediately follows the symbol table.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    std::uint32_t const numSyms = readU32LE(bytes, 12);
    std::uint32_t const strtabPtr = symPtr + numSyms * 18u;
    ASSERT_LE(strtabPtr + 4u, bytes.size());
    std::uint32_t const strtabSize = readU32LE(bytes, strtabPtr);
    // Minimum value is 4 (empty table with just the size prefix).
    // All sym names ≤ 8 chars in this test → inlined → strtab is
    // exactly 4 bytes (just the size prefix).
    EXPECT_EQ(strtabSize, 4u);
}

// ── IMAGE_RELOCATION: 10 bytes, no addend field ─────────────────

TEST(PeWriter, RelocationsArePackedTenBytesAndCarryNoAddend) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    // 5-byte call rel32 with zero displacement.
    caller.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};        // extern
    rel.kind   = RelocationKind{1};  // rel32 → IMAGE_REL_AMD64_REL32
    rel.addend = 0;                  // PE convention: link.exe applies
                                     // RIP-bias intrinsically; addend
                                     // lives in patch bytes, not on
                                     // the IMAGE_RELOCATION record
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    // PointerToRelocations lives in IMAGE_SECTION_HEADER @ +24 from
    // header start; section header starts at 20 → reloc-ptr at 44.
    std::uint32_t const relocPtr = readU32LE(bytes, 44);
    std::uint16_t const relocCount = readU16LE(bytes, 52);
    ASSERT_EQ(relocCount, 1u);
    ASSERT_GT(relocPtr, 0u);
    ASSERT_LE(relocPtr + 10u, bytes.size());

    // IMAGE_RELOCATION { VirtualAddress(u32), SymbolTableIndex(u32),
    //                    Type(u16) } — 10 bytes, no padding.
    EXPECT_EQ(readU32LE(bytes, relocPtr + 0), 1u);  // VA = patch site
    // SymbolTableIndex = 1 (sym 0 = caller, sym 1 = extern target).
    EXPECT_EQ(readU32LE(bytes, relocPtr + 4), 1u);
    EXPECT_EQ(readU16LE(bytes, relocPtr + 8), 4u);  // REL32 = 4
}

// ── Long symbol names use the string-table offset form ────────

TEST(PeWriter, LongSymbolNamesUseStringTableOffsetForm) {
    // Names ≤ 8 chars inline-NUL-pad into the symbol record. Longer
    // names ([4 zeros][u32 offset] form) populate the string table.
    // Pin the offset-form encoding so a regression in
    // `encodeSymbolName`'s long-form arm cannot ship silently
    // (test-analyzer gap 1, criticality 9).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    // Pin a deliberate >8-char synthesized name. `sym_` + 8 digits
    // = 12 chars > 8 → triggers the offset-form encoding path.
    caller.symbol = SymbolId{99999999u};
    caller.bytes = {0xC3};
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    ASSERT_LE(symPtr + 18u, bytes.size());
    // Long-form Name[8] = [0,0,0,0][u32 offset into strtab]
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(bytes[symPtr + i], 0u)
            << "long-form Name must start with 4 zero bytes";
    }
    std::uint32_t const offset = readU32LE(bytes, symPtr + 4);
    EXPECT_GE(offset, 4u)
        << "string-table offset must include the 4-byte size prefix";
    // String table size includes itself; with one >8-char name +
    // size prefix + NUL terminator, size > 4.
    std::uint32_t const numSyms = readU32LE(bytes, 12);
    std::uint32_t const strtabPtr = symPtr + numSyms * 18u;
    ASSERT_LE(strtabPtr + 4u, bytes.size());
    EXPECT_GT(readU32LE(bytes, strtabPtr), 4u);
}

// ── Wrong-format kind rejection ────────────────────────────────

TEST(PeWriter, NonPeFormatKindEmitsK_NoMatchingObjectFormat) {
    auto loaded = loadShipped();
    // Pass an ELF schema to the PE walker — should fail loud
    // (symmetric mirror of the ELF walker's NonElfFormatKind test).
    auto elfRes = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(elfRes.has_value());
    auto const& elf = *elfRes.value();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0xC3};
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, elf, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── pe.machine = 0 validate rejection ──────────────────────────

TEST(PeFormatJson, ZeroMachineRejectedByValidate) {
    // The validate() rule requires `pe.machine != 0`. Pin the
    // rejection so a regression that drops the rule ships loud.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-pe","kind":"pe"},
      "pe": { "machine": 0 }
    })");
    ASSERT_FALSE(r.has_value());
}

// ── PE relocation addend=non-zero fails loud ───────────────────

TEST(PeWriter, NonZeroAddendFailsLoud) {
    // PE has no addend field on IMAGE_RELOCATION (addend lives in
    // the patch bytes). If the assembler stamped a non-zero addend
    // (ELF convention) the walker MUST surface a diagnostic instead
    // of silently dropping (silent-failure C2 convergence).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    rel.addend = -4;
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

// ── End-to-end via the format-blind `link()` dispatch ──────────

TEST(LinkerEndToEnd, PeDispatchProducesNonEmptyBytes) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::Pe);
    EXPECT_FALSE(image.bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // Magic: machine word at byte 0 should be 0x8664 LE.
    ASSERT_GE(image.bytes.size(), 2u);
    EXPECT_EQ(image.bytes[0], 0x64u);
    EXPECT_EQ(image.bytes[1], 0x86u);
}
