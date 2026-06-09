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
#include "link_test_support.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// D-TEST-LE-READ-HELPERS CLOSED at 8aabc04 audit fold; complete-
// hoist at 5ac97ae audit fold per code-architect Q1.
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;
// readI16LE is PE-only (used at one .reloc site); keep local until
// a 2nd signed-LE consumer lands. Anchor D-TEST-LE-READ-SIGNED.
[[nodiscard]] std::int16_t readI16LE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    return static_cast<std::int16_t>(readU16LE(b, off));
}

// Find a PE32+ EXEC section header by its 8-byte name; returns
// {virtualAddress, pointerToRawData}, or {0,0} if absent (the caller
// asserts). Section headers begin at file offset 0x188 (immediately
// after the 240-byte optional header at 0x98); each is 40 bytes;
// NumberOfSections is the u16 at IMAGE_FILE_HEADER+2 = 0x86.
[[nodiscard]] std::pair<std::uint32_t, std::uint32_t>
findExecSection(std::vector<std::uint8_t> const& img,
                std::array<char, 8> const&       name) {
    std::uint16_t const n = readU16LE(img, 0x86);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const h = 0x188u + static_cast<std::size_t>(i) * 40u;
        bool eq = true;
        for (std::size_t b = 0; b < 8; ++b) {
            if (static_cast<char>(img[h + b]) != name[b]) { eq = false; break; }
        }
        if (eq) return {readU32LE(img, h + 12), readU32LE(img, h + 20)};
    }
    return {0u, 0u};
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

// ── PE section with `virtualAddress` field set rejected ──

TEST(PeFormatJson, NonZeroVirtualAddressRejected) {
    // PE derives runtime VAs from IMAGE_OPTIONAL_HEADER.ImageBase
    // + section RVA at link time (cycle-2 PE32+ path D-LK2-1). The
    // substrate `virtualAddress` field is meaningless for PE; pin
    // the validate-rejection so a future PE-row edit can't silently
    // no-op. Added in LK1 cycle 2 alongside the new field.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-pe-va","kind":"pe"},
      "pe": { "machine": 34404 },
      "sections":[{"kind":"text","name":".text","type":1615855648,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── PE section with `segment` field rejected (cross-format check) ──

TEST(PeFormatJson, SegmentFieldRejectedOnPeSection) {
    // Mach-O is the only format that uses the two-level (segment,
    // section) naming. validate() rejects non-empty `segment` on
    // ELF and PE rows symmetrically — pin the PE half so a future
    // split of the rule (e.g. under D-LK2-1 image-side work) can't
    // silently drop the PE arm (test-analyzer convergence).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-pe-seg","kind":"pe"},
      "pe": { "machine": 34404 },
      "sections":[{"kind":"text","name":".text","segment":"__TEXT","type":1615855648,"flags":0,"addrAlign":0,"entrySize":0}]
    })");
    ASSERT_FALSE(r.has_value());
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
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::Pe);
    EXPECT_FALSE(image.bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // Magic: machine word at byte 0 should be 0x8664 LE.
    ASSERT_GE(image.bytes.size(), 2u);
    EXPECT_EQ(image.bytes[0], 0x64u);
    EXPECT_EQ(image.bytes[1], 0x86u);
}

// ── LK2 cycle 2: PE32+ executable image (.exe) writer ──────────

namespace {
[[nodiscard]] Loaded loadShippedExec() {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(pe64-x86_64-windows-exec) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}
} // namespace

TEST(PeExecFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Pe);
    EXPECT_EQ(loaded.format->pe().objectType, PeObjectType::Exec);
    auto const& oh = loaded.format->peOptionalHeader();
    EXPECT_EQ(oh.magic, 0x20Bu);                       // PE32+
    EXPECT_EQ(oh.imageBase, 0x140000000ull);
    EXPECT_EQ(oh.sectionAlignment, 0x1000u);
    EXPECT_EQ(oh.fileAlignment, 0x200u);
    EXPECT_EQ(oh.subsystem, 3u);                       // WINDOWS_CUI
}

TEST(PeExecWriter, DosHeaderMzSignature) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 0x40u);
    EXPECT_EQ(bytes[0], 'M');
    EXPECT_EQ(bytes[1], 'Z');
    // e_lfanew at offset 0x3C points to PE signature at 0x80.
    EXPECT_EQ(readU32LE(bytes, 0x3C), 0x80u);
}

TEST(PeExecWriter, PeSignatureAtZero80) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 0x84u);
    EXPECT_EQ(bytes[0x80], 'P');
    EXPECT_EQ(bytes[0x81], 'E');
    EXPECT_EQ(bytes[0x82], 0);
    EXPECT_EQ(bytes[0x83], 0);
}

TEST(PeExecWriter, OptionalHeaderFieldsByteForByte) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // IMAGE_FILE_HEADER at 0x84:
    //   [0x84] Machine (u16) = 0x8664
    //   [0x86] NumberOfSections (u16) = 1
    //   [0x88] TimeDateStamp (u32) = 0
    //   [0x8C] PointerToSymbolTable (u32) = 0
    //   [0x90] NumberOfSymbols (u32) = 0
    //   [0x94] SizeOfOptionalHeader (u16) = 240
    //   [0x96] Characteristics (u16) = 0x22 (EXECUTABLE_IMAGE|LARGE_ADDR_AWARE)
    EXPECT_EQ(readU16LE(bytes, 0x84), 0x8664u);
    EXPECT_EQ(readU16LE(bytes, 0x86), 1u);
    EXPECT_EQ(readU32LE(bytes, 0x88), 0u);
    EXPECT_EQ(readU32LE(bytes, 0x8C), 0u);
    EXPECT_EQ(readU32LE(bytes, 0x90), 0u);
    EXPECT_EQ(readU16LE(bytes, 0x94), 240u);
    EXPECT_EQ(readU16LE(bytes, 0x96), 0x22u);

    // IMAGE_OPTIONAL_HEADER64 starts at 0x98.
    //   [0x98] Magic (u16) = 0x20B (PE32+)
    EXPECT_EQ(readU16LE(bytes, 0x98), 0x20Bu);
    //   [0xA8] AddressOfEntryPoint (u32) = secText.RVA + 0 = 0x1000
    EXPECT_EQ(readU32LE(bytes, 0xA8), 0x1000u);
    //   [0xB0] ImageBase (u64) = 0x140000000
    EXPECT_EQ(readU64LE(bytes, 0xB0), 0x140000000ull);
    //   [0xB8] SectionAlignment (u32) = 0x1000
    EXPECT_EQ(readU32LE(bytes, 0xB8), 0x1000u);
    //   [0xBC] FileAlignment (u32) = 0x200
    EXPECT_EQ(readU32LE(bytes, 0xBC), 0x200u);
    //   [0xDC] Subsystem (u16) = 3 (WINDOWS_CUI) — after CheckSum(0xD8)
    EXPECT_EQ(readU16LE(bytes, 0xDC), 3u);
}

TEST(PeExecWriter, IntraModuleRel32CallAppliedByteForByte) {
    // Multi-function PE EXEC: fn[0] calls fn[1] via rel32.
    // sectionVa = imageBase + secText.RVA = 0x140001000.
    // fn[0] @ +0 (6 B: E8 ?? ?? ?? ?? C3); fn[1] @ +6 (1 B: C3).
    // P = sectionVa + 1, S = sectionVa + 6, A = 0.
    // value = S - P - 4 = 1.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // Locate .text file offset from IMAGE_SECTION_HEADER at 0x188
    //   [0x188 .. 0x188+8) Name
    //   [0x190] virtualSize (u32)
    //   [0x194] virtualAddress (u32)
    //   [0x198] sizeOfRawData (u32)
    //   [0x19C] pointerToRawData (u32) ← file offset of .text
    std::uint32_t const textFileOff =
        readU32LE(bytes, 0x188 + 20);
    ASSERT_GE(bytes.size(), textFileOff + 6u);
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 1u);
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);
}

// LK11b cross-CU thunk slot — the ASLR base-relocation correctness pin. A rodata pointer
// slot carrying ONE abs64 (DIR64) fixup to a function is the exact shape the cross-CU merge
// mints (linker.cpp). Because the exec keeps ASLR (DYNAMIC_BASE), the walker must (a) write
// the slot's PREFERRED VA (imageBase + targetRVA) into the slot bytes AND (b) emit a `.reloc`
// IMAGE_REL_BASED_DIR64 entry at the SLOT's RVA so the loader rebases it. This locks the
// RVA-vs-VA distinction — the .reloc entry carries an RVA, the slot value carries a VA — the
// single highest-risk arithmetic in the cross-CU feature. It runs CROSS-PLATFORM and purely
// in memory (no process spawn), so Linux CI catches a base-reloc regression that the
// Windows-only `examples/c-subset/cross_cu_call` run cannot.
TEST(PeExecWriter, Abs64RodataSlotEmitsDir64BaseRelocation) {
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};                       // ret — the first .text fn, lands at RVA 0x1000
    mod.functions.push_back(std::move(fn));
    AssembledData slot;
    slot.symbol  = SymbolId{2};
    slot.section = DataSectionKind::Rodata;
    slot.bytes.assign(8, std::uint8_t{0});    // 8-byte pointer slot — the abs64 fixup site
    Relocation slotRel;
    slotRel.offset = 0;
    slotRel.target = SymbolId{1};             // the slot points at fn#1
    slotRel.kind   = RelocationKind{2};       // abs64 (x86_64 target kind 2: width 8, !pcRel)
    slotRel.addend = 0;
    slot.relocations.push_back(slotRel);
    mod.dataItems.push_back(std::move(slot));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    constexpr std::uint64_t kImageBase = 0x140000000ull;
    // Sections present (no .idata — the module has no extern imports): .text, .rdata, .reloc.
    auto const text  = findExecSection(bytes, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const rdata = findExecSection(bytes, {'.', 'r', 'd', 'a', 't', 'a', 0, 0});
    auto const reloc = findExecSection(bytes, {'.', 'r', 'e', 'l', 'o', 'c', 0, 0});
    ASSERT_NE(text.first, 0u)  << ".text section must exist";
    ASSERT_NE(rdata.first, 0u) << ".rdata section (the rodata slot) must exist";
    ASSERT_NE(reloc.first, 0u) << ".reloc section must exist for the abs64 slot";

    // (a) The slot VALUE is a VA: imageBase + fn#1's RVA. fn#1 is the first .text function
    //     (RVA == text.first); the slot is the first (only) .rdata item, at section offset 0.
    EXPECT_EQ(readU64LE(bytes, rdata.second), kImageBase + text.first)
        << "the rodata slot must hold the def's PREFERRED VA (imageBase + targetRVA)";

    // (b) DataDirectory[5] (BASE RELOCATION TABLE): RVA @ file 0x130, Size @ 0x134 (the
    //     PE32+ optional header is at 0x98; its DataDirectory array begins at 0x98+0x70=0x108;
    //     entry 5 is +0x28 → 0x130). One DIR64 fixup → one block: 8-byte header + 1 two-byte
    //     entry + 2-byte ABSOLUTE pad (odd entry count) = 12 bytes.
    EXPECT_EQ(readU32LE(bytes, 0x130), reloc.first) << "data dir[5] RVA == .reloc section RVA";
    EXPECT_EQ(readU32LE(bytes, 0x134), 12u)         << "one DIR64 block, padded to 4 bytes";

    // (c) The IMAGE_BASE_RELOCATION block at the .reloc file offset:
    //       [+0] PageRVA (u32)     = slotRVA & ~0xFFF  — an RVA (NOT a VA), the slot's page
    //       [+4] SizeOfBlock (u32) = 12
    //       [+8] entry (u16)       = (IMAGE_REL_BASED_DIR64=10 << 12) | (slotRVA & 0xFFF)
    //       [+10] pad (u16)        = 0 (ABSOLUTE no-op)
    std::uint32_t const slotRva = rdata.first;  // first .rdata item is at section offset 0
    EXPECT_EQ(readU32LE(bytes, reloc.second + 0), slotRva & ~0xFFFu)
        << "PageRVA must be the slot's RVA page — an RVA, never the slot's VA";
    EXPECT_LT(readU32LE(bytes, reloc.second + 0), kImageBase)
        << "PageRVA must be an RVA (< imageBase): proof no VA leaked into the .reloc table";
    EXPECT_EQ(readU32LE(bytes, reloc.second + 4), 12u);
    EXPECT_EQ(readU16LE(bytes, reloc.second + 8),
              static_cast<std::uint16_t>((10u << 12) | (slotRva & 0x0FFFu)))
        << "entry = (DIR64 << 12) | page-offset";
    EXPECT_EQ(readU16LE(bytes, reloc.second + 10), 0u)
        << "odd DIR64 entry count → one ABSOLUTE(0) u16 pad to 4-byte alignment";
}

TEST(PeExecWriter, ExternTargetFailsLoudAsUndefined) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};       // extern
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(PeExecFormatJsonValidate, MissingImageBaseRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-pe-exec","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecFormatJsonValidate, ObjWithOptionalHeaderRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"obj-with-opt-hdr","kind":"pe"},
      "pe": { "machine": 34404, "type": "obj" },
      "optionalHeader": { "magic": 523 }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecFormatJsonValidate, NonPow2SectionAlignmentRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"odd-align","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 3000, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── New tests folded from 7-agent review of LK2 cycle 2 ────────

TEST(PeExecFormatJsonValidate, SectionAlignmentBelowPageSizeRejected) {
    // PE/COFF §3.4 + code-reviewer C3 + silent-failure C3: PE32+
    // sectionAlignment must be >= 4096 (page size). Windows loader
    // rejects sub-page alignment with STATUS_INVALID_IMAGE_FORMAT.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"sub-page","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 512, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecFormatJsonValidate, MissingSubsystemRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"no-subsystem","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecFormatJsonValidate, MissingStackHeapSizesRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"no-stack","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecFormatJsonValidate, SectionAlignmentLessThanFileAlignmentRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"sect-lt-file","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 8192, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecFormatJsonValidate, VirtualAddressNotMultipleOfSectionAlignmentRejected) {
    // code-reviewer C3: secText.virtualAddress must be a multiple
    // of sectionAlignment per PE/COFF §3.4.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"misaligned-va","kind":"pe"},
      "pe": { "machine": 34404, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4097}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(PeExecWriter, DllArmFailsLoud) {
    // Anchored D-LK2-4: PE .dll arm not yet implemented; walker
    // emits K_NoMatchingObjectFormat rather than silently producing
    // a malformed DLL.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"a-dll","kind":"pe"},
      "pe": { "machine": 34404, "type": "dll" },
      "optionalHeader": { "magic": 523, "imageBase": 6442450944, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "dllCharacteristics": 0, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_TRUE(r.has_value());
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, **target, **r, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(PeExecWriter, EmptyTextFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // bytes intentionally empty
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(PeExecWriter, RelocOffsetPastFunctionBytesFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xC3};   // 1 byte
    Relocation rel;
    rel.offset = 4;       // past end
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3, 0xC3, 0xC3, 0xC3};
    mod.functions.push_back(std::move(f1));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(PeExecWriter, LoadBearingOptionalHeaderFieldsPinnedByteForByte) {
    // PE32+ field-ordering regression catcher. A future refactor
    // that slides any IMAGE_OPTIONAL_HEADER64 u64 by 8 bytes
    // silently shifts every subsequent field — e.g. stackReserve
    // 0x100000 reads as a 4 GiB request → ENOMEM at process load.
    // Pin every load-bearing field at its spec offset.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Per PE/COFF §3.4 (PE32+ optional header layout):
    //   [0xD0] SizeOfImage (u32) — must be ≥ secText.RVA + textVS.
    //                              The shipped JSON ships 1-byte
    //                              .text so SizeOfImage = 0x2000.
    EXPECT_EQ(readU32LE(bytes, 0xD0), 0x2000u);
    //   [0xD4] SizeOfHeaders (u32) — fileAlignment-aligned. Headers
    //                              fit in 0x200 for the shipped JSON.
    EXPECT_EQ(readU32LE(bytes, 0xD4), 0x200u);
    //   [0xDE] DllCharacteristics (u16) — shipped JSON sets
    //                              0x8160 (HIGH_ENTROPY|DYNAMIC_BASE|
    //                              NX_COMPAT|TS_AWARE).
    EXPECT_EQ(readU16LE(bytes, 0xDE), 0x8160u);
    //   [0xE0] SizeOfStackReserve (u64) — 0x100000 (1 MiB).
    EXPECT_EQ(readU64LE(bytes, 0xE0), 0x100000ull);
    //   [0xE8] SizeOfStackCommit (u64) — 0x1000 (4 KiB).
    EXPECT_EQ(readU64LE(bytes, 0xE8), 0x1000ull);
    //   [0xF0] SizeOfHeapReserve (u64) — 0x100000 (1 MiB).
    EXPECT_EQ(readU64LE(bytes, 0xF0), 0x100000ull);
    //   [0xF8] SizeOfHeapCommit (u64) — 0x1000 (4 KiB).
    EXPECT_EQ(readU64LE(bytes, 0xF8), 0x1000ull);
    //   [0x100] LoaderFlags (u32) — reserved, must be 0.
    EXPECT_EQ(readU32LE(bytes, 0x100), 0u);
    //   [0x104] NumberOfRvaAndSizes (u32) — fixed at 16.
    EXPECT_EQ(readU32LE(bytes, 0x104), 16u);
}

TEST(PeExecWriter, DisplacementOverflowFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};
    rel.kind   = RelocationKind{1};
    rel.addend = std::numeric_limits<std::int64_t>::max() / 2;
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── LK6 cycle 2a: FFI extern imports via .idata / IAT ──────────

namespace {
[[nodiscard]] AssembledModule makeModuleWithOneExtern(
    std::vector<std::uint8_t> bytes,
    std::uint32_t             fnSym,
    std::uint32_t             externSym,
    std::int64_t              relOffset) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{fnSym};
    fn.bytes  = std::move(bytes);
    Relocation rel;
    rel.offset = relOffset;
    rel.target = SymbolId{externSym};
    rel.kind   = RelocationKind{1};  // rel32
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    ExternImport imp;
    imp.symbol      = SymbolId{externSym};
    imp.mangledName = "ExitProcess";
    imp.libraryPath = "kernel32.dll";
    mod.externImports.push_back(std::move(imp));
    return mod;
}
} // namespace

TEST(PeExecWriter, ExternImportProducesIDataSectionAndPatchesReloc) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3}, 1, 99, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // NumberOfSections = 2 (.text + .idata)
    EXPECT_EQ(readU16LE(bytes, 0x86), 2u);

    // IMAGE_DATA_DIRECTORY[1] (Import Table) RVA + Size populated.
    std::uint32_t const importDirRva = readU32LE(bytes, 0x110);
    EXPECT_NE(importDirRva, 0u);
    EXPECT_EQ(readU32LE(bytes, 0x114), 40u);  // 1 lib + null = 40 B

    // IMAGE_DATA_DIRECTORY[12] (IAT) RVA populated.
    std::uint32_t const iatDirRva = readU32LE(bytes, 0x108 + 12*8);
    EXPECT_NE(iatDirRva, 0u);

    // ImageImportDescriptor[0].FirstThunk @ +16 == iatDirRva.
    constexpr std::size_t kSecondSecHdr = 0x188 + 40;
    std::uint32_t const idataFileOff =
        readU32LE(bytes, kSecondSecHdr + 20);
    ASSERT_NE(idataFileOff, 0u);
    EXPECT_EQ(readU32LE(bytes, idataFileOff + 16), iatDirRva);

    // Terminator descriptor — next 20 bytes all zero.
    for (std::size_t i = 0; i < 20; ++i) {
        EXPECT_EQ(bytes[idataFileOff + 20 + i], 0u);
    }

    // REL32 patched in .text: value = iatDirRva - 0x1001 - 4.
    constexpr std::size_t kFirstSecHdr = 0x188;
    std::uint32_t const textFileOff =
        readU32LE(bytes, kFirstSecHdr + 20);
    std::int32_t const expected =
        static_cast<std::int32_t>(iatDirRva) - 0x1001 - 4;
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1),
              static_cast<std::uint32_t>(expected));
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);
}

TEST(PeExecWriter, IDataContainsExitProcessNameAndKernel32DllName) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0, 0, 0, 0, 0xC3}, 1, 99, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    constexpr std::size_t kSecondSecHdr = 0x188 + 40;
    std::uint32_t const idataFileOff =
        readU32LE(bytes, kSecondSecHdr + 20);
    std::uint32_t const idataSize =
        readU32LE(bytes, kSecondSecHdr + 16);
    std::string_view hay{
        reinterpret_cast<char const*>(bytes.data() + idataFileOff),
        idataSize};
    EXPECT_NE(hay.find("ExitProcess"), std::string_view::npos);
    EXPECT_NE(hay.find("kernel32.dll"), std::string_view::npos);
}

TEST(PeExecWriter, NoExternImportsEmitsSingleSection) {
    // Regression: when externImports is empty, the walker emits
    // ONLY .text (no .idata) and IMAGE_DATA_DIRECTORY[1] stays 0.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU16LE(bytes, 0x86), 1u);
    EXPECT_EQ(readU32LE(bytes, 0x110), 0u);
    EXPECT_EQ(readU32LE(bytes, 0x114), 0u);
}

// ── Post-audit folds (test-analyzer + code-reviewer convergences) ──

TEST(PeExecWriter, IltAndIatHoldIdenticalThunksAtFileImageTime) {
    // PE/COFF §6.4: ILT[i] and IAT[i] hold the same HINT/NAME RVA
    // at file image time; the loader patches IAT in-place at load.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0, 0, 0, 0, 0xC3}, 1, 99, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Read ImageImportDescriptor[0] @ idata + 0:
    //   OriginalFirstThunk (ILT RVA) @ +0
    //   FirstThunk         (IAT RVA) @ +16
    constexpr std::size_t kSecondSecHdr = 0x188 + 40;
    std::uint32_t const idataFileOff =
        readU32LE(bytes, kSecondSecHdr + 20);
    std::uint32_t const idataVa =
        readU32LE(bytes, kSecondSecHdr + 12);
    std::uint32_t const iltRva = readU32LE(bytes, idataFileOff + 0);
    std::uint32_t const iatRva = readU32LE(bytes, idataFileOff + 16);
    std::size_t const iltFileOff = idataFileOff + (iltRva - idataVa);
    std::size_t const iatFileOff = idataFileOff + (iatRva - idataVa);
    // Both 8-byte thunks must be byte-identical (point to same
    // HINT/NAME entry).
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(bytes[iltFileOff + i], bytes[iatFileOff + i]);
    }
    // ILT/IAT terminators (next u64 after the single thunk) are 0.
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(bytes[iltFileOff + 8 + i], 0u);
        EXPECT_EQ(bytes[iatFileOff + 8 + i], 0u);
    }
}

TEST(PeExecWriter, MultipleExternsInOneLibraryProduceDistinctIatSlots) {
    // test-analyzer #1: with 2 externs in one library, slotIdx
    // must advance by kThunkSize (=8). A bug that uses
    // descriptor-size 20 instead of thunk-size 8 would land
    // ExitProcess at the same IAT slot as GetStdHandle.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xC3};
    Relocation r1; r1.offset = 1; r1.target = SymbolId{10};
    r1.kind = RelocationKind{1};
    Relocation r2; r2.offset = 6; r2.target = SymbolId{11};
    r2.kind = RelocationKind{1};
    fn.relocations.push_back(r1);
    fn.relocations.push_back(r2);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{10}, "ExitProcess",  "kernel32.dll"});
    mod.externImports.push_back(
        ExternImport{SymbolId{11}, "GetStdHandle", "kernel32.dll"});
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU16LE(bytes, 0x86), 2u);
    // 1 lib + null terminator descriptor = 2 × 20 = 40
    EXPECT_EQ(readU32LE(bytes, 0x114), 40u);
    // IAT directory size = 2 externs + 1 terminator slot = 3 * 8.
    EXPECT_EQ(readU32LE(bytes, 0x108 + 12*8 + 4), 24u);
    // Two distinct HINT/NAME entries → ExitProcess and GetStdHandle.
    constexpr std::size_t kSecondSecHdr = 0x188 + 40;
    std::uint32_t const idataFileOff =
        readU32LE(bytes, kSecondSecHdr + 20);
    std::uint32_t const idataSize =
        readU32LE(bytes, kSecondSecHdr + 16);
    std::string_view hay{
        reinterpret_cast<char const*>(bytes.data() + idataFileOff),
        idataSize};
    EXPECT_NE(hay.find("ExitProcess"),  std::string_view::npos);
    EXPECT_NE(hay.find("GetStdHandle"), std::string_view::npos);
}

TEST(PeExecWriter, MultipleLibrariesEachWithOneExtern) {
    // test-analyzer #2: with 2 libraries, descriptorBlockSize =
    // 3 × 20 = 60 — NOT 8-aligned. code-reviewer #1: walker pads
    // up to 8 before ILT/IAT (idata buffer zero-initialised, pad
    // stays 0). Verify the IAT-block stride works and both DLL
    // names land in the table.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xC3};
    Relocation r1; r1.offset = 1; r1.target = SymbolId{10};
    r1.kind = RelocationKind{1};
    Relocation r2; r2.offset = 6; r2.target = SymbolId{11};
    r2.kind = RelocationKind{1};
    fn.relocations.push_back(r1);
    fn.relocations.push_back(r2);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{10}, "ExitProcess", "kernel32.dll"});
    mod.externImports.push_back(
        ExternImport{SymbolId{11}, "printf",      "msvcrt.dll"});
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // 2 libs + null terminator = 3 × 20 = 60
    EXPECT_EQ(readU32LE(bytes, 0x114), 60u);
    // IAT total: 2 libs × (1 extern + 1 terminator) × 8 = 32
    EXPECT_EQ(readU32LE(bytes, 0x108 + 12*8 + 4), 32u);
    constexpr std::size_t kSecondSecHdr = 0x188 + 40;
    std::uint32_t const idataFileOff =
        readU32LE(bytes, kSecondSecHdr + 20);
    std::uint32_t const idataSize =
        readU32LE(bytes, kSecondSecHdr + 16);
    std::string_view hay{
        reinterpret_cast<char const*>(bytes.data() + idataFileOff),
        idataSize};
    EXPECT_NE(hay.find("kernel32.dll"), std::string_view::npos);
    EXPECT_NE(hay.find("msvcrt.dll"),   std::string_view::npos);
    EXPECT_NE(hay.find("ExitProcess"),  std::string_view::npos);
    EXPECT_NE(hay.find("printf"),       std::string_view::npos);
}

TEST(PeExecWriter, MixedExternAndIntraModuleCallInOneFunction) {
    // test-analyzer #3: one function with TWO relocations — one
    // to an intra-module function (resolves to .text VA) and one
    // to an extern (resolves to .idata IAT slot VA). The shared
    // symbolVa kernel must dispatch both correctly.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    // call intra ; call extern ; ret
    f0.bytes  = {0xE8,0,0,0,0, 0xE8,0,0,0,0, 0xC3};
    Relocation rIntra;
    rIntra.offset = 1;
    rIntra.target = SymbolId{2};       // → fn[1]
    rIntra.kind   = RelocationKind{1};
    Relocation rExtern;
    rExtern.offset = 6;
    rExtern.target = SymbolId{99};     // → extern
    rExtern.kind   = RelocationKind{1};
    f0.relocations.push_back(rIntra);
    f0.relocations.push_back(rExtern);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "ExitProcess", "kernel32.dll"});
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Intra-call: from text @ +1 to fn[1] @ +11 (fn[0] is 11 bytes).
    // value = 11 - 1 - 4 = 6.
    constexpr std::size_t kFirstSecHdr = 0x188;
    std::uint32_t const textFileOff =
        readU32LE(bytes, kFirstSecHdr + 20);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 6u);
    // Extern call: patches to iatDirRva - 0x1006 - 4.
    std::uint32_t const iatDirRva = readU32LE(bytes, 0x108 + 12*8);
    std::int32_t const expectedExtern =
        static_cast<std::int32_t>(iatDirRva) - 0x1006 - 4;
    EXPECT_EQ(readU32LE(bytes, textFileOff + 6),
              static_cast<std::uint32_t>(expectedExtern));
}

TEST(LinkerExternResolution, NeitherDefinedNorExternStillFailsLoud) {
    // test-analyzer #4: a reloc target present in NEITHER
    // functions NOR externImports still fires K_SymbolUndefined.
    // The new diagnostic message mentions ExternImport explicitly.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8,0,0,0,0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{42};         // not in any table
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    // externImports is empty AND fn's reloc target isn't fn.symbol.
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.bytes.empty());
    bool sawCode = false;
    bool sawExternMention = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) {
            sawCode = true;
            if (d.actual.find("ExternImport") != std::string::npos)
                sawExternMention = true;
        }
    }
    EXPECT_TRUE(sawCode);
    EXPECT_TRUE(sawExternMention);
}

TEST(LinkerExternResolution, EmptyMangledNameRejected) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "", "kernel32.dll"});
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(LinkerExternResolution, EmptyLibraryPathRejected) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "ExitProcess", ""});
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(LinkerExternResolution, DuplicateSymbolIdAcrossFunctionsAndExternsRejected) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, /*fnSym=*/1);
    mod.externImports.push_back(
        ExternImport{SymbolId{1}, "ExitProcess", "kernel32.dll"});
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.bytes.empty());
    bool sawAmbig = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
         && d.actual.find("ambiguous resolution") != std::string::npos) {
            sawAmbig = true;
        }
    }
    EXPECT_TRUE(sawAmbig);
}

TEST(LinkerExternResolution, OkFalseWhenWalkerFailsLoud) {
    // architect O1: post-walker errorCount gate resets
    // resolvedFuncCount so LinkedImage.ok() doesn't return true
    // with empty bytes when the walker fail-loud'd. Both ELF +
    // Mach-O dynamic walkers now succeed for externs (cycles
    // 2b.2 + 2c). To exercise the fail-loud gate we use an ELF
    // schema with `bindNow=false` (lazy binding anchored at
    // D-LK6-11 — still fails loud).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"elf-lazy-gate","kind":"elf"},
      "entryPoint": "",
      "elf": {
        "class":"elf64","data":"lsb","machine":62,"type":"exec",
        "pageAlign":4096,
        "interpreter":"/lib64/ld-linux-x86-64.so.2",
        "bindNow": false
      },
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ],
      "relocations":[
        {"name":"R_X86_64_PC32","kind":1,"nativeId":2},
        {"name":"R_X86_64_64","kind":2,"nativeId":1},
        {"name":"R_X86_64_32","kind":3,"nativeId":10}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "printf", "libc.so.6"});
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, **target, **fmt, rep);
    EXPECT_TRUE(img.bytes.empty());
    EXPECT_FALSE(img.ok());
    EXPECT_EQ(img.resolvedFuncCount, 0u);
}

// ── D-LK2-RODATA: PE walker emits .rdata from AssembledData ────
//
// The PE walker emits a `.rdata` section between `.text` and
// `.idata` whenever `module.dataItems` carries any item with
// `section == DataSectionKind::Rodata`. Layout discipline:
//   * Per-item Alignment padding within the section.
//   * Section header characteristics 0x40000040 from the format
//     schema's Rodata row (IMAGE_SCN_CNT_INITIALIZED_DATA |
//     IMAGE_SCN_MEM_READ).
//   * Section ordering: [.text, .rdata?, .idata?].
//   * SizeOfInitializedData sums rdataRawSize + idataRawSize.
//   * Linker's F1 capability gate lowers for PE only.

TEST(PeExecWriter, RodataSectionEmittedWhenDataItemsNonEmpty) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Rodata;
    d.bytes   = {'h', 'e', 'l', 'l', 'o', '\n', '\0'};
    d.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // NumberOfSections at IMAGE_FILE_HEADER offset 0x86: 2 sections
    // (.text + .rdata; no .idata since externImports empty).
    EXPECT_EQ(readU16LE(bytes, 0x86), 2u)
        << ".text + .rdata = 2 sections (idata absent)";
    // Section header table starts at 0x188 (DOS + stub + PE sig +
    // file header + optional header64). .rdata is sectionHeaders[1]
    // (after .text). Each section header is 40 bytes.
    constexpr std::size_t kSecHdrTable = 0x188;
    constexpr std::size_t kSecHdrSize  = 40;
    constexpr std::size_t kRdataHdr    = kSecHdrTable + kSecHdrSize;
    // Section header name: ".rdata\0\0" (8 bytes).
    EXPECT_EQ(bytes[kRdataHdr + 0], '.');
    EXPECT_EQ(bytes[kRdataHdr + 1], 'r');
    EXPECT_EQ(bytes[kRdataHdr + 2], 'd');
    EXPECT_EQ(bytes[kRdataHdr + 3], 'a');
    EXPECT_EQ(bytes[kRdataHdr + 4], 't');
    EXPECT_EQ(bytes[kRdataHdr + 5], 'a');
    EXPECT_EQ(bytes[kRdataHdr + 6], 0u);
    EXPECT_EQ(bytes[kRdataHdr + 7], 0u);
    // VirtualSize @ +8 = 7 (the item's byte count).
    EXPECT_EQ(readU32LE(bytes, kRdataHdr + 8), 7u);
    // Characteristics @ +36 = 0x40000040.
    EXPECT_EQ(readU32LE(bytes, kRdataHdr + 36), 0x40000040u);
    // Bytes at the rdata file pointer should match "hello\n\0".
    std::uint32_t const rdataFileOff =
        readU32LE(bytes, kRdataHdr + 20);
    ASSERT_GE(bytes.size(), rdataFileOff + 7u);
    EXPECT_EQ(bytes[rdataFileOff + 0], 'h');
    EXPECT_EQ(bytes[rdataFileOff + 1], 'e');
    EXPECT_EQ(bytes[rdataFileOff + 2], 'l');
    EXPECT_EQ(bytes[rdataFileOff + 3], 'l');
    EXPECT_EQ(bytes[rdataFileOff + 4], 'o');
    EXPECT_EQ(bytes[rdataFileOff + 5], '\n');
    EXPECT_EQ(bytes[rdataFileOff + 6], '\0');
}

TEST(PeExecWriter, RodataAndIdataCoexistInCorrectOrder) {
    // When BOTH dataItems and externImports are non-empty, the
    // walker emits 3 sections: .text, .rdata, .idata in that VA
    // order. The .idata RVA shifts to (rdataRva + rdataVirtualSize).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0, 0, 0, 0, 0xC3}, 1, 99, 1);
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Rodata;
    d.bytes   = {'x'};
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // 3 sections.
    EXPECT_EQ(readU16LE(bytes, 0x86), 3u);
    constexpr std::size_t kSecHdrTable = 0x188;
    constexpr std::size_t kSecHdrSize  = 40;
    // .text @ [0], .rdata @ [1], .idata @ [2].
    auto const hdrAt = [&](std::size_t i) {
        return kSecHdrTable + i * kSecHdrSize;
    };
    std::uint32_t const textVa  = readU32LE(bytes, hdrAt(0) + 12);
    std::uint32_t const rdataVa = readU32LE(bytes, hdrAt(1) + 12);
    std::uint32_t const idataVa = readU32LE(bytes, hdrAt(2) + 12);
    // VAs in ascending order — .rdata between .text and .idata.
    EXPECT_LT(textVa, rdataVa);
    EXPECT_LT(rdataVa, idataVa);
    // .idata name pinned (regression: section ordering swap would
    // place .rdata at [2] and .idata at [1]).
    EXPECT_EQ(bytes[hdrAt(2) + 0], '.');
    EXPECT_EQ(bytes[hdrAt(2) + 1], 'i');
    EXPECT_EQ(bytes[hdrAt(2) + 2], 'd');
    EXPECT_EQ(bytes[hdrAt(2) + 3], 'a');
    EXPECT_EQ(bytes[hdrAt(2) + 4], 't');
    EXPECT_EQ(bytes[hdrAt(2) + 5], 'a');
}

TEST(PeExecWriter, MultipleRodataItemsLayWithAlignmentPadding) {
    // Per-item Alignment padding within the section. Item 0 is 3
    // bytes byte-aligned; item 1 is 1 byte but 8-byte aligned →
    // item 1 lands at offset 8 (3 bytes padded up to 8).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData a;
    a.symbol  = SymbolId{42};
    a.section = DataSectionKind::Rodata;
    a.bytes   = {0x11, 0x22, 0x33};
    a.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(a));
    AssembledData b;
    b.symbol  = SymbolId{43};
    b.section = DataSectionKind::Rodata;
    b.bytes   = {0xFF};
    b.alignment = Alignment::of<8>();
    mod.dataItems.push_back(std::move(b));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    constexpr std::size_t kSecHdrTable = 0x188;
    constexpr std::size_t kSecHdrSize  = 40;
    constexpr std::size_t kRdataHdr    = kSecHdrTable + kSecHdrSize;
    // VirtualSize = 3 (item 0) + 5 padding + 1 (item 1) = 9.
    EXPECT_EQ(readU32LE(bytes, kRdataHdr + 8), 9u);
    std::uint32_t const rdataFileOff =
        readU32LE(bytes, kRdataHdr + 20);
    // Item 0 bytes at offset 0..3.
    EXPECT_EQ(bytes[rdataFileOff + 0], 0x11);
    EXPECT_EQ(bytes[rdataFileOff + 1], 0x22);
    EXPECT_EQ(bytes[rdataFileOff + 2], 0x33);
    // Padding bytes 3..7 are zero.
    for (std::size_t i = 3; i < 8; ++i) {
        EXPECT_EQ(bytes[rdataFileOff + i], 0u)
            << "padding byte " << i << " must be zero";
    }
    // Item 1's byte at offset 8 (aligned).
    EXPECT_EQ(bytes[rdataFileOff + 8], 0xFFu);
}

TEST(PeExecWriter, NoRodataItemsEmitsNoRdataSection) {
    // Regression: empty dataItems must NOT introduce a .rdata
    // section (preserves the pre-D-LK2-RODATA single-section
    // shape for modules without rodata).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Single section (.text only).
    EXPECT_EQ(readU16LE(bytes, 0x86), 1u);
}

namespace {
// Helper that builds a one-function module + one Rodata data item
// and runs the linker against the given (target, format), returning
// the reporter for inspection. Used by the per-format capability-
// gate tests below.
[[nodiscard]] LinkedImage runLinkerWithRodataItem(
    TargetSchema const& target,
    ObjectFormatSchema const& fmt,
    DiagnosticReporter& rep,
    DataSectionKind kind = DataSectionKind::Rodata) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = kind;
    if (kind != DataSectionKind::Bss) {
        d.bytes = {'x'};
    }
    mod.dataItems.push_back(std::move(d));
    return linker::link(mod, target, fmt, rep);
}
} // namespace

TEST(LinkerEndToEnd, ElfAcceptsRodataDataItemsAfterD_LK1_ELF_EXEC_DATA_SECTIONS) {
    // FLIP-MARKER: D-LK1-ELF-EXEC-DATA-SECTIONS CLOSED. The ELF exec
    // format JSON now advertises `supportedDataSections: ["rodata"]`,
    // so the schema-declared capability gate (linker.cpp) lets a
    // Rodata dataItem through to the ELF walker, which emits a loadable
    // `.rodata` section. A one-function module + one rodata item now
    // links cleanly (was K_NoMatchingObjectFormat before the arm
    // closed). When this fails, the format JSON's supportedDataSections
    // row was removed without re-anchoring the walker arm.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped(
        "elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    DiagnosticReporter rep;
    LinkedImage img =
        runLinkerWithRodataItem(**target, **fmt, rep);
    EXPECT_TRUE(img.ok())
        << "ELF exec must now accept a Rodata dataItem end-to-end "
           "(D-LK1-ELF-EXEC-DATA-SECTIONS)";
    EXPECT_EQ(rep.errorCount(), 0u);
    // resolvedFuncCount == 2: the user `ret` function PLUS the
    // synthetic `_start` trampoline the linker prepends for exec
    // images (D-LK10-ENTRY Slice C). A clean link (vs the pre-close
    // rejection at resolvedFuncCount==0) is the marker.
    EXPECT_EQ(img.resolvedFuncCount, 2u);
    EXPECT_TRUE((**fmt).acceptsDataSection(
        DataSectionKind::Rodata))
        << "ELF exec format JSON must advertise rodata now that "
           "D-LK1-ELF-EXEC-DATA-SECTIONS has closed";
}

TEST(LinkerEndToEnd, MachORejectsDataItemsUntilD_LK3_RODATA) {
    // Parallel to the ELF test — Mach-O declares no
    // `supportedDataSections`; D-LK3-RODATA still anchored.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped(
        "macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    DiagnosticReporter rep;
    LinkedImage img =
        runLinkerWithRodataItem(**target, **fmt, rep);
    EXPECT_FALSE(img.ok());
    EXPECT_EQ(::dss::test_support::countCode(rep,
                  DiagnosticCode::K_NoMatchingObjectFormat),
              1u);
    EXPECT_EQ(img.resolvedFuncCount, 0u);
    EXPECT_FALSE((**fmt).acceptsDataSection(
        DataSectionKind::Rodata))
        << "Mach-O exec format JSON must not advertise rodata "
           "until D-LK3-RODATA closes";
}

TEST(PeExecWriter, RejectsDataKindDataItemUntilD_LK4_RODATA_PRODUCER) {
    // PE-Exec walker accepts ONLY Rodata. Data items must fail
    // loud at the walker's pre-loop scan (the schema-declared
    // capability gate stops Bss-only-walker formats earlier; the
    // walker's own pre-loop scan stops Data/Bss items even when
    // the format advertises Rodata).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Data;
    d.bytes   = {0x11};
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    // Bypass the linker (whose schema gate would also reject) to
    // exercise the walker's OWN guard.
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_EQ(::dss::test_support::countCode(rep,
                  DiagnosticCode::K_NoMatchingObjectFormat),
              1u);
}

TEST(PeExecWriter, RejectsBssDataItemViaLinkerSchemaGate) {
    // Bss items are rejected by the linker's schema-declared
    // capability gate (PE-Exec advertises only Rodata) BEFORE
    // reaching the walker. `validateAssembledData` runs FIRST and
    // accepts Bss-with-empty-bytes; the schema gate then fires
    // K_NoMatchingObjectFormat. Ordering is load-bearing — flipping
    // these two blocks would surface the wrong diagnostic class.
    auto loaded = loadShippedExec();
    DiagnosticReporter rep;
    LinkedImage img = runLinkerWithRodataItem(
        *loaded.target, *loaded.format, rep,
        DataSectionKind::Bss);
    EXPECT_FALSE(img.ok());
    EXPECT_EQ(::dss::test_support::countCode(rep,
                  DiagnosticCode::K_NoMatchingObjectFormat),
              1u);
}

TEST(PeExecWriter, RequireSectionRodataFailsLoudWhenSchemaOmitsRow) {
    // Synthesize a PE-Exec format JSON that advertises rodata
    // capability but does NOT declare a `.rdata` sections[] row
    // — the walker's `requireSection(Rodata)` must fail loud
    // (silent emission without a section is forbidden).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    char const* const kJson = R"({
      "$comment": "Synthetic PE-Exec schema for D-LK2-RODATA require-section test — declares rodata capability but omits the section row.",
      "dssObjectFormatVersion": 1,
      "format": {"name": "pe-exec-rodata-no-row", "version": "1.0", "kind": "pe"},
      "entryPoint": "",
      "processExit": {"mechanism": "by-name-import", "importLibraryPath": "kernel32.dll", "importMangledName": "ExitProcess"},
      "entryCallingConvention": "ms_x64",
      "supportedDataSections": ["rodata"],
      "pe": {"machine": 34404, "characteristics": 34, "type": "exec"},
      "optionalHeader": {"magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "majorOperatingSystemVersion": 6, "minorOperatingSystemVersion": 0, "majorSubsystemVersion": 6, "minorSubsystemVersion": 0, "subsystem": 3, "dllCharacteristics": 33120, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096},
      "sections": [
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}
      ],
      "relocations": [
        {"name":"IMAGE_REL_AMD64_REL32","kind":1,"nativeId":4},
        {"name":"IMAGE_REL_AMD64_ADDR64","kind":2,"nativeId":1},
        {"name":"IMAGE_REL_AMD64_ADDR32","kind":3,"nativeId":2}
      ]
    })";
    auto fmt = ObjectFormatSchema::loadFromText(kJson, "synthetic");
    ASSERT_TRUE(fmt.has_value());
    DiagnosticReporter rep;
    LinkedImage img = runLinkerWithRodataItem(
        **target, **fmt, rep);
    EXPECT_FALSE(img.ok());
    EXPECT_EQ(::dss::test_support::countCode(rep,
                  DiagnosticCode::K_NoMatchingObjectFormat),
              1u);
}

TEST(PeExecWriter, SizeOfInitializedDataSumsRdataAndIdata) {
    // PE/COFF §3.4: SizeOfInitializedData = Σ rawSize of sections
    // carrying IMAGE_SCN_CNT_INITIALIZED_DATA. With `.rdata` +
    // `.idata` both present and fileAlignment=512, this is
    // 2 * 512 = 1024 (both single-byte payloads file-pad to 512).
    auto loaded = loadShippedExec();
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0, 0, 0, 0, 0xC3}, 1, 99, 1);
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Rodata;
    d.bytes   = {'x'};
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // OH64 starts at file-offset 0x98 (DOS 64 + stub 64 + PE sig 4
    // + file hdr 20 = 152 = 0x98); SizeOfInitializedData is at +8.
    EXPECT_EQ(readU32LE(bytes, 0x98 + 8), 1024u);
}

TEST(PeExecWriter, SizeOfImageReflectsRdataExtentInRodataOnlyArm) {
    // SizeOfImage = alignUp(highest_section_va_end, sectionAlign).
    // With rodata-only (no imports), the highest VA-extent is
    // rdata->rva + rdata->virtualSize. text_VA = 0x1000, text_VS
    // section-aligned = 0x1000, rdata_VA = 0x2000, rdata_VS
    // section-aligned = 0x1000 → SizeOfImage = 0x3000.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Rodata;
    d.bytes   = {'x'};
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // SizeOfImage field at OH64+56 (per the existing
    // LoadBearingOptionalHeaderFieldsPinnedByteForByte test).
    EXPECT_EQ(readU32LE(bytes, 0x98 + 56), 0x3000u);
}

TEST(PeExecWriter, CertTableFileOffsetShiftsPastRdataAndIdata) {
    // Synthesize a PE-Exec format with non-zero
    // `attributeCertReserveSize` so the walker emits a non-zero
    // cert table file offset at IMAGE_DATA_DIRECTORY[4]. The
    // offset must sit STRICTLY past the LAST section's raw end —
    // a regression that forgot to shift past rdata would silently
    // place the cert table inside rdata bytes, corrupting the
    // signed image. (The shipped exec JSON ships with
    // attributeCertReserveSize=0 today; this test exercises the
    // shift code that triggers when LK7 codesign lands the
    // reservation field.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    char const* const kJson = R"({
      "$comment": "Synthetic PE-Exec schema for D-LK2-RODATA cert-table-shift test — declares non-zero attributeCertReserveSize so the walker computes cert table offset past rdata+idata.",
      "dssObjectFormatVersion": 1,
      "format": {"name": "pe-exec-cert-shift", "version": "1.0", "kind": "pe"},
      "entryPoint": "",
      "processExit": {"mechanism": "by-name-import", "importLibraryPath": "kernel32.dll", "importMangledName": "ExitProcess"},
      "entryCallingConvention": "ms_x64",
      "supportedDataSections": ["rodata"],
      "pe": {"machine": 34404, "characteristics": 34, "type": "exec"},
      "optionalHeader": {"magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "majorOperatingSystemVersion": 6, "minorOperatingSystemVersion": 0, "majorSubsystemVersion": 6, "minorSubsystemVersion": 0, "subsystem": 3, "dllCharacteristics": 33120, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096, "attributeCertReserveSize": 64},
      "sections": [
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096},
        {"kind":"rodata","name":".rdata","type":1073741888,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":0}
      ],
      "relocations": [
        {"name":"IMAGE_REL_AMD64_REL32","kind":1,"nativeId":4},
        {"name":"IMAGE_REL_AMD64_ADDR64","kind":2,"nativeId":1},
        {"name":"IMAGE_REL_AMD64_ADDR32","kind":3,"nativeId":2}
      ]
    })";
    auto fmt = ObjectFormatSchema::loadFromText(kJson, "synthetic");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0, 0, 0, 0, 0xC3}, 1, 99, 1);
    AssembledData d;
    d.symbol  = SymbolId{42};
    d.section = DataSectionKind::Rodata;
    d.bytes   = {'x'};
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Data directory base in OH64: per PE §3.4 the directories
    // sit at offset 112 from OH64 start. Cert is entry 4 → +32.
    // IMAGE_DATA_DIRECTORY[4].VirtualAddress is the (file-offset)
    // base of the cert table.
    constexpr std::size_t kOH64       = 0x98;
    constexpr std::size_t kDirsBase   = kOH64 + 112;
    constexpr std::size_t kCertVaOff  = kDirsBase + 4 * 8;
    std::uint32_t const certFileOff =
        readU32LE(bytes, kCertVaOff);
    EXPECT_NE(certFileOff, 0u)
        << "Cert reservation declared; non-zero offset expected";
    // Cert offset must land STRICTLY past the section raw bytes —
    // a regression that placed the cert table inside rdata or
    // idata would silently corrupt them.
    constexpr std::size_t kSecHdrTable = 0x188;
    constexpr std::size_t kSecHdrSize  = 40;
    // .idata is sectionHeaders[2] here (text=0, rdata=1, idata=2).
    constexpr std::size_t kIdataHdr    = kSecHdrTable + 2 * kSecHdrSize;
    std::uint32_t const idataFileOff   =
        readU32LE(bytes, kIdataHdr + 20);
    std::uint32_t const idataRawSize   =
        readU32LE(bytes, kIdataHdr + 16);
    EXPECT_GE(certFileOff, idataFileOff + idataRawSize)
        << "cert table must sit past the last section's raw end";
}

