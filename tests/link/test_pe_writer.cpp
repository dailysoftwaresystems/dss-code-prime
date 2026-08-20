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
#include "link/image_request.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
#include "diagnostic_count.hpp"
#include "format_reject_support.hpp"   // countAtPath / countWithMessage / rejectSummary

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;
using dss::link_format::test::countAtPath;
using dss::link_format::test::errorCount;
using dss::link_format::test::countWithMessage;
using dss::link_format::test::rejectSummary;

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

// c149 (D-LK-EXTERN-DATA-IMPORT, PE half) sibling: a section's
// Misc.VirtualSize (section header +8) — the UNPADDED byte extent. The
// data-extern tests pin `.text`'s VirtualSize to prove the import-thunk
// block was compacted to the FUNCTION externs (a data extern reserving
// a dead 6-byte `FF 25` thunk would grow it). Returns 0 if absent (the
// caller asserts).
[[nodiscard]] std::uint32_t
findExecSectionVirtualSize(std::vector<std::uint8_t> const& img,
                           std::array<char, 8> const&       name) {
    std::uint16_t const n = readU16LE(img, 0x86);
    for (std::uint16_t i = 0; i < n; ++i) {
        std::size_t const h = 0x188u + static_cast<std::size_t>(i) * 40u;
        bool eq = true;
        for (std::size_t b = 0; b < 8; ++b) {
            if (static_cast<char>(img[h + b]) != name[b]) { eq = false; break; }
        }
        if (eq) return readU32LE(img, h + 8);
    }
    return 0u;
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
    // StorageClass @ +16 = 3 (IMAGE_SYM_CLASS_STATIC) — this function has no
    // ModuleSymbol row, so it is not externally visible → `definedBinding` =
    // Local → STATIC, not EXTERNAL (TF-C54, D-LK-INTERNAL-LINKAGE-FN-EMITTED-
    // GLOBAL-FOREIGN-COLLISION; pre-fix it was hardcoded EXTERNAL = 2).
    EXPECT_EQ(bytes[symPtr + 16], 3u);
    // NumberOfAuxSymbols @ +17 = 0
    EXPECT_EQ(bytes[symPtr + 17], 0u);
}

// ── D-LK-OBJECT-EXTERN-SYMBOL-NAMES: real names in the .obj symtab ──

TEST(PeWriter, ObjectSymtabCarriesRealNameForExternalDefButStaticStaysInternal) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    // Two defined functions: A externally-visible (Global) → real name;
    // B static (Local) → stays internal `sym_11`. Names are ≤ 8 chars so
    // the COFF Name field inlines them (no string-table indirection).
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{10};
    a.bytes  = {0xC3};
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{11};
    b.bytes  = {0xC3};
    mod.functions.push_back(std::move(b));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "realfn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "statfn",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    std::uint32_t const symPtr  = readU32LE(bytes, 8);
    std::uint32_t const numSyms = readU32LE(bytes, 12);
    ASSERT_EQ(numSyms, 2u);

    auto inlineNameAt = [&](std::uint32_t recOff) {
        std::string s;
        for (std::size_t i = 0; i < 8 && bytes[recOff + i] != 0; ++i) {
            s.push_back(static_cast<char>(bytes[recOff + i]));
        }
        return s;
    };

    // Symbol 0 = fn A (externally-visible) → real inlined name.
    EXPECT_EQ(inlineNameAt(symPtr), "realfn")
        << "externally-visible defined function must carry its real name";
    // Symbol 1 = fn B (static) → stays internal `sym_11`.
    EXPECT_EQ(inlineNameAt(symPtr + 18u), "sym_11")
        << "a static (Local-binding) function must stay internal in the .obj";

    // Storage class (COFF symbol record byte @ +16) — TF-C54,
    // D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION. NAME and
    // BINDING are coupled: fn A (Global) → IMAGE_SYM_CLASS_EXTERNAL (2); fn B
    // (static, Local) → IMAGE_SYM_CLASS_STATIC (3), so a sibling `.obj`'s
    // unrelated `sym_<id>` can never collide with it at a FOREIGN multi-TU
    // link. COFF has no local-first ordering (unlike ELF) — order unchanged,
    // only the class flips. RED-ON-DISABLE: pre-fix both were EXTERNAL (2).
    EXPECT_EQ(bytes[symPtr + 16], 2u)
        << "externally-visible fn A is IMAGE_SYM_CLASS_EXTERNAL (2)";
    EXPECT_EQ(bytes[symPtr + 18u + 16], 3u)
        << "static fn B is IMAGE_SYM_CLASS_STATIC (3), not EXTERNAL — the TF-C54 fix";
}

// ── D-LK-OBJECT-WEAK-DEF-RELOCATABLE: a WEAK defined symbol fails loud ──
//
// The relocatable PE/COFF writer cannot faithfully emit a WEAK DEFINED symbol
// (COFF weak-external — IMAGE_SYM_CLASS_WEAK_EXTERNAL + its aux record — is not
// wired). Degrading it to IMAGE_SYM_CLASS_EXTERNAL would SILENTLY lose the
// weak-override semantics, so the writer fails loud instead. RED-ON-DISABLE:
// reverting the fail-loud arm makes the writer emit the weak fn as EXTERNAL (2)
// with 0 errors — this pin (empty bytes + exactly one K_NoMatchingObjectFormat
// citing the anchor) goes red. The ELF sibling (Weak → STB_WEAK, native) +
// the PE/Mach-O EXEC arms (weak forced Global / merge-resolved) are unaffected.
TEST(PeWriter, ObjectWeakDefinedFunctionFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction w;
    w.symbol = SymbolId{10};
    w.bytes  = {0xC3};
    mod.functions.push_back(std::move(w));
    // A WEAK, externally-visible defined function (the `__attribute__((weak))`
    // shape c-subset.lang.json produces). `definedBinding` returns Weak.
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "weakfn",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty())
        << "a weak defined symbol must emit no bytes (loud-fail path), never a "
           "silently-degraded EXTERNAL record";
    EXPECT_EQ(rep.errorCount(), 1u)
        << "exactly one fail-loud diagnostic must fire for the weak def";
    bool found = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat &&
            d.actual.find("D-LK-OBJECT-WEAK-DEF-RELOCATABLE") !=
                std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "the diagnostic must cite D-LK-OBJECT-WEAK-DEF-RELOCATABLE (the "
           "COFF weak-external deferral) for future-grep navigability";
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-pe-va","kind":"pe"},
      "pe": { "machine": 34404 },
      "sections":[{"kind":"text","name":".text","type":1615855648,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: pe.machine is valid, no optionalHeader
    // is declared (so its own "must not declare" rule can't co-fire),
    // and 'segment' is empty, so the non-zero virtualAddress on this
    // (default) PE .obj row is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/virtualAddress"), 1u)
        << rejectSummary(r);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-pe-seg","kind":"pe"},
      "pe": { "machine": 34404 },
      "sections":[{"kind":"text","name":".text","segment":"__TEXT","type":1615855648,"flags":0,"addrAlign":0,"entrySize":0}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: pe.machine is valid and virtualAddress
    // defaults to 0 (satisfying the default PE .obj rule), so the
    // non-empty 'segment' is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/segment"), 1u) << rejectSummary(r);
}

// ── pe.machine = 0 validate rejection ──────────────────────────

TEST(PeFormatJson, ZeroMachineRejectedByValidate) {
    // The validate() rule requires `pe.machine != 0`. Pin the
    // rejection so a regression that drops the rule ships loud.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-pe","kind":"pe"},
      "pe": { "machine": 0 }
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: no optionalHeader/sections declared, so
    // pe.machine==0 is the only diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/pe/machine"), 1u) << rejectSummary(r);
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

// ── D-LK-OBJECT-DATA-SECTION-RELOCATABLE (PE/COFF Obj arm, c148) ──
//
// The .obj writer emits DATA sections — the PE analog of the ELF c142
// ET_REL arm + the Mach-O c147 mirror (and the c145/c147 data-item
// relocation emission). Layout constants for a TWO-section .obj (used
// by the pins below): IMAGE_FILE_HEADER 20 | section header[0] (.text)
// @20 | section header[1] @60 (Name@60, VirtualSize@68, VA@72,
// SizeOfRawData@76, PointerToRawData@80, PointerToRelocations@84,
// NumberOfRelocations@92 u16, Characteristics@96) | raw data @100.

namespace {
// Compare an 8-byte COFF header/symbol Name field with a short name
// (inline names are NUL-padded; buf[8] stays NUL so the string_view
// ctor stops at the pad for names under 8 chars and at index 8 for
// full-width names).
[[nodiscard]] bool coffNameEquals(std::vector<std::uint8_t> const& bytes,
                                   std::size_t off, std::string_view expect) {
    char buf[9] = {};
    for (std::size_t i = 0; i < 8; ++i)
        buf[i] = static_cast<char>(bytes[off + i]);
    return std::string_view{buf} == expect;
}
} // namespace

// (1) A function + one rodata item: NumberOfSections grows to 2, the
// `.rdata` header carries the schema Characteristics OR'd with the
// walker-derived IMAGE_SCN_ALIGN class, and the data symbol is
// EXTERNAL with SectionNumber = the 1-based ordinal and Value = the
// SECTION-RELATIVE offset (the COFF convention — NOT Mach-O's flat
// address). RED-on-disable: revert the data-section emission →
// NumberOfSections stays 1 and NumberOfSymbols stays 1.
TEST(PeWriter, ObjRodataItemEmitsRdataSectionAndDataSymbol) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};                      // ret (1 byte)
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "greet",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {'h', 'i', 0};
    d.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{42}, "msg",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(readU16LE(bytes, 2), 2u);      // NumberOfSections
    // Section header[1] = `.rdata` (rodata).
    EXPECT_TRUE(coffNameEquals(bytes, 60, ".rdata"));
    EXPECT_EQ(readU32LE(bytes, 68), 0u);     // VirtualSize (obj)
    EXPECT_EQ(readU32LE(bytes, 72), 0u);     // VirtualAddress (obj)
    EXPECT_EQ(readU32LE(bytes, 76), 3u);     // SizeOfRawData = "hi\0"
    EXPECT_EQ(readU32LE(bytes, 80), 101u);   // PointerToRawData = 100 + text 1
    EXPECT_EQ(readU32LE(bytes, 84), 0u);     // PointerToRelocations (none)
    EXPECT_EQ(readU16LE(bytes, 92), 0u);     // NumberOfRelocations
    // Characteristics = CNT_INITIALIZED_DATA|MEM_READ (0x40000040, the
    // schema row) | IMAGE_SCN_ALIGN_1BYTES (0x00100000, H1-derived).
    EXPECT_EQ(readU32LE(bytes, 96), 0x40100040u);
    // The item's bytes land at the raw pointer.
    ASSERT_GE(bytes.size(), 104u);
    EXPECT_EQ(bytes[101], 'h');
    EXPECT_EQ(bytes[102], 'i');
    EXPECT_EQ(bytes[103], 0u);

    // Symbols: fn(0) + data(1). PointerToSymbolTable = 100 + 1 + 3.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    EXPECT_EQ(symPtr, 104u);
    ASSERT_EQ(readU32LE(bytes, 12), 2u);     // NumberOfSymbols
    std::size_t const s1 = symPtr + 18u;
    EXPECT_TRUE(coffNameEquals(bytes, s1, "msg"))
        << "externally-visible data global must carry its real name";
    EXPECT_EQ(readU32LE(bytes, s1 + 8), 0u)
        << "COFF Value = SECTION-RELATIVE offset";
    EXPECT_EQ(readI16LE(bytes, s1 + 12), 2)
        << "SectionNumber = the .rdata 1-based ordinal";
    EXPECT_EQ(readU16LE(bytes, s1 + 14), 0u)  // notype (data)
        << "COFF data symbols are notype - DTYPE_FUNCTION is fn-only";
    EXPECT_EQ(bytes[s1 + 16], 2u);            // IMAGE_SYM_CLASS_EXTERNAL
    EXPECT_EQ(bytes[s1 + 17], 0u);            // no aux records
}

// ── D-LK-INTERNAL-LINKAGE-FN-EMITTED-GLOBAL-FOREIGN-COLLISION (TF-C54):
//    the DATA emit site couples name+binding exactly as the function site ──
//
// The static→Local storage-class flip lives at a SECOND, fully-duplicated emit
// site (the data loop), NOT shared with the function loop — the design-audit
// flagged this data twin as unpinned. A static (Local-binding) DATA item must
// carve to `sym_<id>` + IMAGE_SYM_CLASS_STATIC (3); the Global half is pinned
// by ObjRodataItemEmitsRdataSectionAndDataSymbol above (EXTERNAL = 2). This is
// NOT academic — a string-literal / `static const` rodata is exactly a Local
// data item, and pre-fix it collided across TUs as a GLOBAL `sym_<id>` (the
// reader witness uses a `sym_101` rodata static). RED-ON-DISABLE: reverting the
// data-site ternary (pe.cpp) to a hardcoded EXTERNAL makes this read 2 → red.
TEST(PeWriter, ObjectStaticDataItemIsClassStaticNotExternal) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "greet",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {'h', 'i', 0};
    d.alignment = Alignment::of<1>();
    mod.dataItems.push_back(std::move(d));
    // A STATIC (Local-binding) data item — the `static const` / string-literal
    // shape. definedBinding → Local → name carved to `sym_42`, class STATIC.
    mod.symbols.push_back(ModuleSymbol{SymbolId{42}, "msg",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Symbols: fn(0) + data(1). The data symbol record is at symPtr + 18.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    ASSERT_EQ(readU32LE(bytes, 12), 2u);
    std::size_t const s1 = symPtr + 18u;
    EXPECT_TRUE(coffNameEquals(bytes, s1, "sym_42"))
        << "a static (Local) data item stays internal `sym_<id>`, not its real name";
    EXPECT_EQ(bytes[s1 + 16], 3u)   // IMAGE_SYM_CLASS_STATIC — THE FIX
        << "static data emits IMAGE_SYM_CLASS_STATIC (3), not the pre-fix EXTERNAL (2)";

    // ★★ THE DERIVED-TYPE HALF -- D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-
    // BLOCK-LABEL-NOT-ATOM, the DATA twin of the block-symbol pin in
    // ObjJumpTableBlockSymbolIsStaticLocalDefinedNotUndefExtern. A COFF data
    // symbol is `notype` whatever its linkage, and the reader relies on that:
    // class STATIC + type 0 is the shape it must NOT promote to an atom. This
    // also fixes the boundary of the reader fix in place -- stamping
    // DTYPE_FUNCTION here would silently make every static data object an atom
    // boundary in the wrong section kind. Both directions again.
    EXPECT_EQ(readU16LE(bytes, s1 + 14), 0u)
        << "a defined DATA symbol must carry derived type 0 (`notype`) -- "
           "DTYPE_FUNCTION is functions-only, and the COFF reader classifies "
           "atom boundaries on this field";
    EXPECT_EQ(readU16LE(bytes, symPtr + 14), 0x20u)
        << "...while the FUNCTION symbol at index 0 carries "
           "IMAGE_SYM_DTYPE_FUNCTION (0x20) -- the field discriminates";
}

// D-LK-OBJECT-WEAK-DEF-RELOCATABLE: a WEAK defined DATA symbol fails loud — the
// data-site twin of ObjectWeakDefinedFunctionFailsLoud. The data loop has its
// OWN weak guard (pe.cpp); reverting it silently degrades a weak data def to
// EXTERNAL with no diagnostic. RED-ON-DISABLE: revert the data-site guard →
// non-empty bytes + errorCount 0 → this pin goes red.
TEST(PeWriter, ObjectWeakDefinedDataFailsLoud) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;   // a benign anchor fn so only the DATA is weak
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "anchor",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData d;
    d.symbol    = SymbolId{10};
    d.section   = DataSectionKind::Data;
    d.bytes     = {1, 2, 3, 4};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "weakdat",
                                       SymbolBinding::Weak,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty())
        << "a weak defined DATA symbol must emit no bytes (loud-fail path)";
    EXPECT_EQ(rep.errorCount(), 1u)
        << "exactly one fail-loud diagnostic must fire for the weak data def";
    bool found = false;
    for (auto const& dg : rep.all()) {
        if (dg.code == DiagnosticCode::K_NoMatchingObjectFormat &&
            dg.actual.find("D-LK-OBJECT-WEAK-DEF-RELOCATABLE") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "the diagnostic must cite D-LK-OBJECT-WEAK-DEF-RELOCATABLE at the data site";
}

// (2) A RelRoConst item carrying an abs64 reloc to a DEFINED function
// (a const fn-ptr table slot): relro is its OWN section header NAMED
// `.rdata` (COFF has no relro segment; MSVC places reloc-bearing const
// in `.rdata` — link.exe merges the same-name contributions) with its
// OWN IMAGE_RELOCATION table: VirtualAddress = the slot's
// section-relative offset, SymbolTableIndex = the function's index,
// Type = IMAGE_REL_AMD64_ADDR64 (the format JSON's abs64 nativeId).
// The relocation's addend is written INTO the 8-byte slot (the COFF
// in-place-addend convention — IMAGE_RELOCATION has no addend column;
// link.exe computes S + slot). RED-on-disable: drop the data-reloc
// emission → NumberOfRelocations reads 0; drop the in-slot addend →
// the slot reads 0.
TEST(PeWriter, ObjRelRoFnPtrSlotEmitsAddr64RelocAndInSlotAddend) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction f1;
    f1.symbol = SymbolId{1};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));
    AssembledData tab;
    tab.symbol    = SymbolId{7};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes.assign(8, 0);                  // one pointer slot
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{/*offset=*/0u,
                                         /*target=*/SymbolId{1},
                                         /*kind=*/RelocationKind{2},  // abs64
                                         /*addend=*/8});
    mod.dataItems.push_back(std::move(tab));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(readU16LE(bytes, 2), 2u);      // .text + relro
    EXPECT_TRUE(coffNameEquals(bytes, 60, ".rdata"))
        << "relro rides the .rdata NAME as its own section header";
    EXPECT_EQ(readU32LE(bytes, 76), 8u);     // SizeOfRawData
    EXPECT_EQ(readU32LE(bytes, 80), 101u);   // PointerToRawData
    // Characteristics = 0x40000040 | ALIGN_8BYTES (0x00400000) — the
    // exact class cl.exe stamps for an 8-aligned .rdata contribution.
    EXPECT_EQ(readU32LE(bytes, 96), 0x40400040u);
    // Reloc table follows ALL raw data: 100 + 1 + 8.
    std::uint32_t const relocPtr = readU32LE(bytes, 84);
    ASSERT_EQ(readU16LE(bytes, 92), 1u)
        << "the relro section must carry its own IMAGE_RELOCATION table "
           "(the .rela.data.rel.ro analog)";
    EXPECT_EQ(relocPtr, 109u);
    ASSERT_LE(relocPtr + 10u, bytes.size());
    EXPECT_EQ(readU32LE(bytes, relocPtr + 0), 0u)
        << "VirtualAddress = the slot's offset WITHIN its section";
    EXPECT_EQ(readU32LE(bytes, relocPtr + 4), 0u)
        << "SymbolTableIndex = the DEFINED function's symtab entry";
    EXPECT_EQ(readU16LE(bytes, relocPtr + 8), 1u)
        << "Type = IMAGE_REL_AMD64_ADDR64 (nativeId from the format "
           "JSON, never hardcoded in the walker)";
    // In-place addend: the 8-byte slot at file offset 101 carries 8.
    EXPECT_EQ(readU64LE(bytes, 101), 8u)
        << "IMAGE_RELOCATION has no addend column - rel.addend must be "
           "written into the slot bytes (link.exe computes S + slot)";
    // The table's own symbol: Value = 0 (section-relative), sect 2.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    ASSERT_EQ(readU32LE(bytes, 12), 2u);     // fn + table
    EXPECT_EQ(readI16LE(bytes, symPtr + 18 + 12), 2);
    EXPECT_EQ(readU32LE(bytes, symPtr + 18 + 8), 0u);
}

// A jump-table data slot targeting a SYNTHETIC PER-BLOCK symbol (the
// dense-switch / computed-goto shape) must resolve to a DEFINED LOCAL
// IMAGE_SYMBOL — IMAGE_SYM_CLASS_STATIC, SectionNumber=1 (.text),
// Value = funcTextStart + blockByteOffset — never the undefined-extern
// fallback (which would fabricate an undefined `sym_<id>` nothing can
// ever define: link.exe LNK2001, or — worse — a silent wrong-control-
// flow bind against a sibling object's unrelated `sym_<id>` export).
// The ELF STB_LOCAL / Mach-O no-N_EXT block-symbol legs are the
// mirrors (c147 CRITICAL fold, baked into the PE arm from the start).
// RED-ON-DISABLE: drop the block-symbol registration → symbol[1] flips
// to the data symbol and the block lands UNDEF at the tail → the
// StorageClass/SectionNumber/Value assertions fail.
TEST(PeWriter, ObjJumpTableBlockSymbolIsStaticLocalDefinedNotUndefExtern) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{10};
    fn.bytes  = {0xC3, 0x90, 0x90, 0x90,     // block 0
                 0xC3, 0x90, 0x90, 0x90};    // block 1 at offset 4
    fn.blockSymbols.push_back({SymbolId{77}, /*blockByteOffset=*/4u});
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "dispatch",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    AssembledData tab;                        // one jump-table slot
    tab.symbol    = SymbolId{7};
    tab.section   = DataSectionKind::Data;
    tab.bytes.assign(8, 0);
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{/*offset=*/0u,
                                         /*target=*/SymbolId{77},  // BLOCK
                                         /*kind=*/RelocationKind{2},
                                         /*addend=*/0});
    mod.dataItems.push_back(std::move(tab));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a jump-table slot targeting a block symbol must encode";

    // Symbol order: func(0) → BLOCK local(1) → data(2). No undefined
    // extern fabricated for the block.
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    ASSERT_EQ(readU32LE(bytes, 12), 3u)
        << "func + block local + data symbol - and NO fabricated "
           "undefined extern for the block";
    std::size_t const s1 = symPtr + 18u;      // the block symbol
    EXPECT_TRUE(coffNameEquals(bytes, s1, "sym_77"));
    EXPECT_EQ(bytes[s1 + 16], 3u)
        << "block symbol must be IMAGE_SYM_CLASS_STATIC (3, LOCAL) - "
           "EXTERNAL would let a sibling object's unrelated sym_<id> "
           "bind to an interior block address (silent wrong control "
           "flow); UNDEF SectionNumber is the fabricated-extern break "
           "this pin guards";
    EXPECT_EQ(readI16LE(bytes, s1 + 12), 1)   // SectionNumber = .text
        << "block symbol lives in .text";
    EXPECT_EQ(readU32LE(bytes, s1 + 8), 4u)   // Value = text offset
        << "Value = funcTextStart + blockByteOffset (section-relative)";

    // ★★ THE DERIVED-TYPE HALF -- D-LINK-NONEXTERNAL-DEFINED-SYMBOL-READ-AS-
    // BLOCK-LABEL-NOT-ATOM. The storage class above says LOCAL and says nothing
    // more: a file-local FUNCTION is class STATIC too. What separates them is
    // IMAGE_SYMBOL.Type (+14) -- a function declares DTYPE_FUNCTION (0x20), a
    // block label declares 0 -- and the COFF reader now classifies atom
    // boundaries on exactly that field. Until this pin existed the property the
    // reader depends on was UNASSERTED on the writer side: a writer that stamped
    // 0x20 on block symbols would make every interior label an atom boundary and
    // SPLIT the function containing it, and nothing here would have noticed.
    // Both directions are pinned, because "all zero" is not a discriminator.
    EXPECT_EQ(readU16LE(bytes, s1 + 14), 0u)
        << "a synthetic block label must carry derived type 0 -- 0x20 would make "
           "the reader treat an interior label as its own atom and split the "
           "enclosing function";
    EXPECT_EQ(readU16LE(bytes, symPtr + 14), 0x20u)
        << "...while the FUNCTION symbol at index 0 carries "
           "IMAGE_SYM_DTYPE_FUNCTION (0x20) -- the field is a real discriminator, "
           "not uniformly zero";

    // The .data section's reloc resolves SymbolTableIndex to the block
    // local (index 1), and its Type is the JSON abs64 nativeId.
    std::uint32_t const relocPtr = readU32LE(bytes, 84);
    ASSERT_EQ(readU16LE(bytes, 92), 1u);
    EXPECT_EQ(readU32LE(bytes, relocPtr + 4), 1u)
        << "the jump-table slot targets the DEFINED block local, not a "
           "fabricated undefined extern";
    EXPECT_EQ(readU16LE(bytes, relocPtr + 8), 1u);   // ADDR64
}

// (3) A bss item: SizeOfRawData carries the ZERO-FILL span with
// PointerToRawData = 0 and VirtualSize = 0 — the COFF .obj convention
// cl.exe itself emits (dumpbin: `.bss` SizeOfRawData=0x190, no raw
// pointer) and link.exe accepts. NO file bytes — the symtab follows
// the text bytes directly. RED-on-disable: emitting bss file-backed
// shifts the symtab pointer; dropping it flips NumberOfSections.
TEST(PeWriter, ObjBssItemCarriesSizeOfRawDataWithNoFilePointer) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData g;
    g.symbol       = SymbolId{9};
    g.section      = DataSectionKind::Bss;
    g.reservedSize = 4;                      // int g; — no file bytes
    g.alignment    = Alignment::of<4>();
    mod.dataItems.push_back(std::move(g));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    EXPECT_EQ(readU16LE(bytes, 2), 2u);
    EXPECT_TRUE(coffNameEquals(bytes, 60, ".bss"));
    EXPECT_EQ(readU32LE(bytes, 68), 0u);     // VirtualSize = 0
    EXPECT_EQ(readU32LE(bytes, 76), 4u)      // SizeOfRawData = span
        << "COFF .obj bss carries its span in SizeOfRawData";
    EXPECT_EQ(readU32LE(bytes, 80), 0u)      // PointerToRawData = 0
        << "bss stores NO file bytes";
    // 0xC0000080 (schema) | ALIGN_4BYTES (0x00300000) = 0xC0300080 —
    // byte-identical to cl.exe's witnessed .bss Characteristics class.
    EXPECT_EQ(readU32LE(bytes, 96), 0xC0300080u);
    // Symtab directly after the text byte (100 + 1) — bss stored none.
    EXPECT_EQ(readU32LE(bytes, 8), 101u);
    // The bss symbol: SectionNumber=2, Value=0 (section-relative).
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    EXPECT_EQ(readI16LE(bytes, symPtr + 18 + 12), 2);
    EXPECT_EQ(readU32LE(bytes, symPtr + 18 + 8), 0u);
}

// (4) A DATA-FREE module keeps the exact single-section layout (the
// pre-c148 shape) BYTE-IDENTICALLY: every header field and trailing
// structure offset unchanged — the data-section machinery must be a
// total no-op when no data items exist.
TEST(PeWriter, ObjDataFreeModuleKeepsSingleSectionLayoutByteIdentical) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // 20 header + 40 section header + 1 text + 18 symbol + 4 strtab.
    ASSERT_EQ(bytes.size(), 83u);
    EXPECT_EQ(readU16LE(bytes, 2), 1u);      // NumberOfSections
    EXPECT_EQ(readU32LE(bytes, 8), 61u);     // PointerToSymbolTable
    EXPECT_EQ(readU32LE(bytes, 12), 1u);     // NumberOfSymbols
    EXPECT_TRUE(coffNameEquals(bytes, 20, ".text"));
    EXPECT_EQ(readU32LE(bytes, 36), 1u);     // SizeOfRawData
    EXPECT_EQ(readU32LE(bytes, 40), 60u);    // PointerToRawData
    EXPECT_EQ(readU32LE(bytes, 44), 0u);     // PointerToRelocations
    EXPECT_EQ(readU32LE(bytes, 56), 0x60500020u);   // Characteristics
    EXPECT_EQ(bytes[60], 0xC3u);             // the text byte
    EXPECT_EQ(readU32LE(bytes, 79), 4u);     // strtab = size prefix only
}

// (5) The shipped .obj format declares the four data-section rows +
// supportedDataSections (the linker's acceptsDataSection gate) — and
// does NOT declare TLS (tdata/tbss stay gate-rejected). Also pins the
// c148 externCallDispatch declaration (the extern-call route into the
// .obj). RED-on-disable: remove a JSON row/field → the corresponding
// assertion fails.
TEST(PeFormatJson, ObjDeclaresDataSectionRowsAndExternDispatch) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    auto const& fmt = *loaded.format;
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::Rodata));
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::Data));
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::Bss));
    EXPECT_TRUE(fmt.acceptsDataSection(DataSectionKind::RelRoConst));
    EXPECT_FALSE(fmt.acceptsDataSection(DataSectionKind::Tdata))
        << "TLS stays undeclared on the .obj schema (fail-loud gate)";
    EXPECT_FALSE(fmt.acceptsDataSection(DataSectionKind::Tbss));

    ASSERT_TRUE(fmt.externCallDispatch().has_value())
        << "pe64-x86_64-windows (.obj) must declare externCallDispatch "
           "so extern calls lower (D-LK-OBJECT-EXTERN-CALL-RELOCATABLE)";
    EXPECT_EQ(*fmt.externCallDispatch(), ExternCallDispatch::DirectPlt);

    auto const* rodata = fmt.sectionByKind(SectionKind::Rodata);
    ASSERT_NE(rodata, nullptr);
    EXPECT_EQ(rodata->name, ".rdata");
    EXPECT_EQ(rodata->type, 0x40000040u);
    auto const* data = fmt.sectionByKind(SectionKind::Data);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data->name, ".data");
    EXPECT_EQ(data->type, 0xC0000040u);
    auto const* relro = fmt.sectionByKind(SectionKind::RelRoConst);
    ASSERT_NE(relro, nullptr);
    EXPECT_EQ(relro->name, ".rdata")
        << "relro rides the .rdata NAME (COFF has no relro segment; "
           "link.exe merges same-name contributions)";
    EXPECT_EQ(relro->type, 0x40000040u);
    auto const* bss = fmt.sectionByKind(SectionKind::Bss);
    ASSERT_NE(bss, nullptr);
    EXPECT_EQ(bss->name, ".bss");
    EXPECT_EQ(bss->type, 0xC0000080u);
    // The rows deliberately carry NO IMAGE_SCN_ALIGN bits — the walker
    // derives the class from the H1 layout alignment (a preexisting
    // align class would corrupt the OR; the walker fail-louds on it).
    for (auto const* row : {rodata, data, relro, bss}) {
        EXPECT_EQ(row->type & 0x00F00000u, 0u) << row->name;
    }
}

// (6) A reloc-bearing RODATA item stays fail-loud (allow=false — the
// RodataDataItemWithRelocationFailsLoud discipline shared with
// ELF/Mach-O): a relocated slot cannot live in never-written rodata.
TEST(PeWriter, ObjRodataItemWithRelocationFailsLoud) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData d;
    d.symbol    = SymbolId{3};
    d.section   = DataSectionKind::Rodata;
    d.bytes.assign(8, 0);
    d.alignment = Alignment::of<8>();
    d.relocations.push_back(Relocation{0u, SymbolId{1}, RelocationKind{2}, 0});
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

// (7) Thread-local items fail LOUD on the direct walker call (the
// linker's acceptsDataSection gate fires first in the shipped
// pipeline; this is the anti-silent-drop belt behind it, mirroring
// the ELF / Mach-O object-arm rejects).
TEST(PeWriter, ObjThreadLocalItemFailsLoud) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData t;
    t.symbol    = SymbolId{4};
    t.section   = DataSectionKind::Tdata;
    t.bytes     = {7, 0, 0, 0};
    t.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(t));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksThreadLocalSupport)
            saw = true;
    }
    EXPECT_TRUE(saw)
        << "a Tdata item must be rejected loud, never silently dropped";
}

// (8) D-LK-OBJECT-EXTERN-CALL-RELOCATABLE (PE arm, c148): an extern
// call's undefined symbol carries its REAL import name — PE x64 C
// mangling is IDENTITY, so `puts` lands verbatim (no leading
// underscore, unlike Mach-O) — never the internal `sym_<id>` fallback
// the pre-c148 writer emitted (the exact break that made a foreign
// `link.exe main.obj dss.obj` fail with an unresolvable `sym_20`).
TEST(PeWriter, ObjExternCallEmitsUndefinedRealNameNotSymId) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{10};
    caller.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};   // call rel32
    caller.relocations.push_back(Relocation{/*offset=*/1u,
                                            /*target=*/SymbolId{20},
                                            /*kind=*/RelocationKind{1},
                                            /*addend=*/0});
    mod.functions.push_back(std::move(caller));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "greet",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "puts";        // pipeline-mangled (identity on PE)
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a .obj with an extern-call import must encode";

    std::uint32_t const symPtr = readU32LE(bytes, 8);
    ASSERT_EQ(readU32LE(bytes, 12), 2u);     // caller + undefined extern
    std::size_t const s1 = symPtr + 18u;
    EXPECT_TRUE(coffNameEquals(bytes, s1, "puts"))
        << "undefined extern must carry its REAL import name (identity "
           "mangling on PE - no leading underscore), never sym_<id>";
    EXPECT_EQ(readU32LE(bytes, s1 + 8), 0u);         // Value = 0
    EXPECT_EQ(readI16LE(bytes, s1 + 12), 0);         // SectionNumber=UNDEF
    EXPECT_EQ(bytes[s1 + 16], 2u);                   // EXTERNAL
    // The rel32 reloc targets that symtab entry.
    std::uint32_t const relocPtr = readU32LE(bytes, 44);
    ASSERT_EQ(readU16LE(bytes, 52), 1u);
    EXPECT_EQ(readU32LE(bytes, relocPtr + 4), 1u);
    EXPECT_EQ(readU16LE(bytes, relocPtr + 8), 4u);   // REL32
}

// (9) The c145/c147 extern-coverage mirror: a target referenced ONLY
// by a DATA-item relocation (a const table of libc fn pointers) still
// gets its undefined IMAGE_SYMBOL under the REAL import name, and the
// data reloc's SymbolTableIndex points at it. RED-on-disable: scan
// only function relocs → the extern entry is never emitted
// (K_SymbolUndefined aborts the encode).
TEST(PeWriter, ObjDataRelocOnlyExternGetsUndefinedSymbol) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};                      // NO function relocations
    mod.functions.push_back(std::move(fn));
    AssembledData tab;
    tab.symbol    = SymbolId{7};
    tab.section   = DataSectionKind::RelRoConst;
    tab.bytes.assign(8, 0);
    tab.alignment = Alignment::of<8>();
    tab.relocations.push_back(Relocation{0u, SymbolId{20},
                                         RelocationKind{2}, 0});
    mod.dataItems.push_back(std::move(tab));
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "puts";
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a data-reloc-only extern must be covered by the undefined-"
           "extern scan (the ELF c145 / Mach-O c147 mirror)";
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    ASSERT_EQ(readU32LE(bytes, 12), 3u) << "fn + table + undefined puts";
    std::size_t const s2 = symPtr + 2u * 18u;
    EXPECT_TRUE(coffNameEquals(bytes, s2, "puts"));
    EXPECT_EQ(readI16LE(bytes, s2 + 12), 0);         // UNDEF
    // The relro reloc targets the extern's index (2).
    std::uint32_t const relocPtr = readU32LE(bytes, 84);
    ASSERT_EQ(readU16LE(bytes, 92), 1u);
    EXPECT_EQ(readU32LE(bytes, relocPtr + 4), 2u);
}

// (10) A data SymbolId colliding with a function SymbolId is a
// producer-contract breach — fail loud (the K_DuplicateDataSymbol
// mirror of the ELF/Mach-O discipline).
TEST(PeWriter, ObjDuplicateDataSymbolFailsLoud) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    AssembledData d;
    d.symbol    = SymbolId{1};               // collides with the function
    d.section   = DataSectionKind::Rodata;
    d.bytes     = {1, 2, 3, 4};
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& diag : rep.all()) {
        if (diag.code == DiagnosticCode::K_DuplicateDataSymbol) saw = true;
    }
    EXPECT_TRUE(saw);
}

// (11) All four data sections at once: the full section-header order
// (.text → .rdata → .data → .rdata(relro) → .bss LAST), cumulative
// raw-data pointers, and 1-based SectionNumber ordinals across every
// symbol. Pins the multi-section cursor arithmetic a single-section
// module cannot exercise — and that COFF accepts TWO same-name
// `.rdata` headers as distinct sections.
TEST(PeWriter, ObjAllFourDataSectionsOrderAndOrdinals) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    auto addItem = [&](std::uint32_t sym, DataSectionKind k,
                       std::vector<std::uint8_t> b, std::uint64_t reserved) {
        AssembledData d;
        d.symbol       = SymbolId{sym};
        d.section      = k;
        d.bytes        = std::move(b);
        d.reservedSize = reserved;
        d.alignment    = Alignment::of<8>();
        mod.dataItems.push_back(std::move(d));
    };
    addItem(10, DataSectionKind::Rodata, {1, 2, 3, 4, 5, 6, 7, 8}, 0);
    addItem(11, DataSectionKind::Data, {9, 9, 9, 9, 9, 9, 9, 9}, 0);
    addItem(12, DataSectionKind::RelRoConst, {0, 0, 0, 0, 0, 0, 0, 0}, 0);
    addItem(13, DataSectionKind::Bss, {}, 8);

    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // 5 sections; raw base = 20 + 5*40 = 220.
    ASSERT_EQ(readU16LE(bytes, 2), 5u);
    struct Expect {
        char const*   name;
        std::uint32_t sizeOfRawData;
        std::uint32_t pointerToRawData;
        std::uint32_t characteristics;
    };
    Expect const rows[] = {
        {".rdata", 8, 221, 0x40400040u},     // rodata (align 8)
        {".data",  8, 229, 0xC0400040u},
        {".rdata", 8, 237, 0x40400040u},     // relro — 2nd .rdata header
        {".bss",   8, 0,   0xC0400080u},     // no raw pointer
    };
    for (std::size_t i = 0; i < 4; ++i) {
        std::size_t const h = 20 + (i + 1) * 40;
        EXPECT_TRUE(coffNameEquals(bytes, h, rows[i].name)) << i;
        EXPECT_EQ(readU32LE(bytes, h + 16), rows[i].sizeOfRawData) << i;
        EXPECT_EQ(readU32LE(bytes, h + 20), rows[i].pointerToRawData) << i;
        EXPECT_EQ(readU32LE(bytes, h + 36), rows[i].characteristics) << i;
    }
    // Symtab after text(1) + rodata(8) + data(8) + relro(8) = 245; bss
    // stored nothing. Symbols: fn=SECT1, then ordinals 2/3/4/5, each
    // Value = 0 (every item is first in its section).
    std::uint32_t const symPtr = readU32LE(bytes, 8);
    EXPECT_EQ(symPtr, 245u);
    ASSERT_EQ(readU32LE(bytes, 12), 5u);
    std::int16_t const expectSect[] = {1, 2, 3, 4, 5};
    for (std::size_t s = 0; s < 5; ++s) {
        EXPECT_EQ(readI16LE(bytes, symPtr + s * 18 + 12), expectSect[s])
            << s;
        EXPECT_EQ(readU32LE(bytes, symPtr + s * 18 + 8), 0u) << s;
    }
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

// ★ THE ONE DOOR every EXEC-flavored byte-level writer test below uses to
// reach `pe::encode`. D-LK10-ENTRY entry gate (`resolveEntryFnIdx`,
// src/link/format/exec_reloc_apply.hpp): a format declaring `processExit`
// has CONTRACTED that its image entry is the DSS-synthesized `_start`
// trampoline, and ONLY `linker::link` injects one (entry_trampoline.cpp
// sets `imageEntryOverride = 0` on every injection). Reaching the walker
// with neither is now K_FormatLacksProcessExit instead of a silent
// functions[0] default.
//
// These tests drive the walker DIRECTLY on purpose — they pin emitted
// BYTES, not the trampoline. Stamping the override says so out loud;
// semantically it is a no-op (index 0 is exactly what the pre-gate default
// produced implicitly), it just makes the choice DECLARED rather than
// assumed. Funnelled through one helper because ~40 call sites are 40
// chances to miss one. The DLL arm and the .obj arm deliberately do NOT
// come through here: a dll module carrying an override is itself a
// fail-loud case (pe.cpp's `isDll` branch), and .obj resolves no entry.
[[nodiscard]] std::vector<std::uint8_t>
encodeUntrampolined(AssembledModule           mod,  // by value: stamped copy
                    TargetSchema const&       target,
                    ObjectFormatSchema const& fmt,
                    DiagnosticReporter&       reporter,
                    ImageRequest const&       request = {}) {
    mod.imageEntryOverride = std::size_t{0};
    return pe::encode(mod, target, fmt, reporter, request);
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

TEST(PeExecFormatJson, ShippedPeExecIsDirectPlt) {
    // D-FFI-PE-IMPORT-THUNK regression pin: pe64-x86_64-windows-exec was
    // flipped indirect-slot→direct-plt this cycle. PE now points each
    // extern symbol's VA at a synthesized `jmp *[IAT slot]` import THUNK
    // (code, a .text address), so an indirect-slot call site (`FF 15`
    // deref of the thunk's CODE bytes as a pointer) would be a latent
    // crash. The shipped exec format MUST declare direct-plt; pin it so a
    // revert to indirect-slot is caught on EVERY leg — the sqlite
    // aSyscall[] address-taken-import crash had no non-Windows runtime to
    // expose it. Mirrors MachOArm64Exit.ShippedX86DarwinExecIsDirectPlt.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.format->externCallDispatch().has_value());
    EXPECT_TRUE(*loaded.format->externCallDispatch()
                == ExternCallDispatch::DirectPlt);
}

TEST(PeExecFormatJson, ShippedPeExecDeclaresGotIndirectDataImportBinding) {
    // c149 (D-LK-EXTERN-DATA-IMPORT, the PE half — the LAST missing
    // image binding model): the shipped pe64 exec format declares
    // `dataImportBinding: "got-indirect"` — PE `__imp_<name>` semantics:
    // the loader fills a data import's IAT slot with the imported
    // OBJECT's address, so the slot IS a pointer (the Mach-O __got
    // non-lazy-pointer model, same enum value). The linker's pre-walker
    // gate keys on this field; MIR->LIR selects the lea-of-slot + deref
    // GlobalAddr lowering from it. RED-on-disable: revert the JSON
    // declaration -> this pin fails AND every data-import link rejects
    // K_FormatLacksImportSupport again (the pre-c149 wall). Mirrors the
    // macho64-arm64-darwin-exec c117 declaration pin.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.format->dataImportBinding().has_value())
        << "pe64-x86_64-windows-exec must declare dataImportBinding "
           "(D-LK-EXTERN-DATA-IMPORT, c149)";
    EXPECT_TRUE(*loaded.format->dataImportBinding()
                == DataImportBinding::GotIndirect)
        << "PE data imports bind IAT-slot-indirect (__imp_ semantics) -- the "
           "got-indirect model, which is now the ONLY member: ELF's "
           "copy-relocation alternative was deleted from the vocabulary "
           "(D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET)";
}

TEST(PeExecWriter, AddressTakenImportResolvesToTextThunkNotIdataSlot) {
    // D-FFI-PE-IMPORT-THUNK host-independent structural pin. The PE exec
    // walker synthesizes one `FF 25 disp32` import thunk per extern at
    // the tail of .text (the ELF-PLT / Mach-O-__stubs analog), and an
    // extern reference resolves to the THUNK (a .text code address), NOT
    // the .idata IAT data slot. RED-on-disable: drop the pe.cpp thunk
    // emission (or flip the format back to indirect-slot) → the reference
    // resolves to .idata and no FF 25 thunk exists → these assertions
    // fail on EVERY leg (the sqlite 0xC0000005 had no host-independent
    // guard). Recomputes the two-hop call→thunk→IAT from the emitted
    // section VAs, so an encoder divergence is loud.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    // main: `E8 <rel32> C3` = call the extern, then ret. The rel32 patch
    // site is at function offset 1; the reloc targets the extern symbol.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        Relocation{/*offset=*/1, SymbolId{99}, RelocationKind{1},
                   /*addend=*/0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "puts", "msvcrt.dll"});

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(img.empty());

    auto const [textRva, textPtr] =
        findExecSection(img, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const [idataRva, idataPtr] =
        findExecSection(img, {'.', 'i', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(textRva, 0u);
    ASSERT_NE(idataRva, 0u);
    (void)idataPtr;

    // (1) The thunk sits at the tail of .text, right after the 6-byte
    //     function body: `FF 25 disp32`.
    std::size_t const thunkFileOff = static_cast<std::size_t>(textPtr) + 6u;
    std::uint32_t const thunkRva   = textRva + 6u;
    ASSERT_LT(thunkFileOff + 6u, img.size());
    EXPECT_EQ(img[thunkFileOff + 0], 0xFFu) << "import thunk opcode byte 0";
    EXPECT_EQ(img[thunkFileOff + 1], 0x25u)
        << "import thunk opcode byte 1 (jmp [rip+disp32])";

    // (2) The thunk jumps THROUGH the extern's IAT slot: its rip-relative
    //     disp32 target lands in .idata (the loader-patched FirstThunk).
    auto const thunkDisp =
        static_cast<std::int32_t>(readU32LE(img, thunkFileOff + 2u));
    std::int64_t const iatTargetRva =
        static_cast<std::int64_t>(thunkRva) + 6 + thunkDisp;
    EXPECT_GE(iatTargetRva, static_cast<std::int64_t>(idataRva))
        << "import thunk must jump through the .idata IAT slot "
           "(thunkRva=0x" << std::hex << thunkRva
        << " disp=" << std::dec << thunkDisp << ")";

    // (3) THE FIX: the extern CALL resolves to the .text THUNK, NOT the
    //     .idata data slot. `E8 disp32` at function offset 0; target =
    //     (funcRva + 5) + disp32 must equal the thunk RVA (in .text). A
    //     revert points it at the .idata IAT slot → `call` into data →
    //     the sqlite 0xC0000005.
    auto const callDisp =
        static_cast<std::int32_t>(readU32LE(img, textPtr + 1u));
    std::int64_t const callTargetRva =
        static_cast<std::int64_t>(textRva) + 5 + callDisp;
    EXPECT_EQ(callTargetRva, static_cast<std::int64_t>(thunkRva))
        << "extern call must resolve to the .text import thunk (0x"
        << std::hex << thunkRva << "), not the .idata IAT slot";
    EXPECT_LT(callTargetRva, static_cast<std::int64_t>(idataRva))
        << "extern call target must be in .text (< .idata RVA)";
}

TEST(PeExecWriter, DataExternBindsToIatSlotInIdataNotTextThunk) {
    // c149 (D-LK-EXTERN-DATA-IMPORT, the PE half) — the data twin of
    // AddressTakenImportResolvesToTextThunkNotIdataSlot. A DATA extern
    // (isData — an msvcrt `_fmode`-class data export) binds to its
    // `.idata` IAT SLOT VA (`__imp_` semantics: the loader fills the
    // slot with the imported OBJECT's address — the Mach-O __got
    // non-lazy-pointer model, got-indirect) and gets NO `FF 25` text
    // thunk (a data object is not callable; a thunk-VA binding would
    // read jump-stub bytes as the object's value).
    //
    // RED-on-disable both ways: (1) revert the JSON declaration -> the
    // walker's binding assertion (and the linker's pre-walker gate)
    // reject with K_FormatLacksImportSupport — no clean encode; (2)
    // bind the data extern to a thunk VA (the function path) -> the
    // resolved reference lands in .text, failing BOTH the exact-slot
    // equality and the .idata range assertion, and .text VirtualSize
    // grows by the dead 6-byte thunk.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    // main: `48 8B 05 <disp32>` (mov rax, [rip+disp32]) + `C3` = read
    // the imported object, then ret — 8 bytes. The disp32 patch site is
    // at function offset 3 (REL32, resolved against the end of the
    // 7-byte mov); the reloc targets the DATA extern.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        Relocation{/*offset=*/3, SymbolId{99}, RelocationKind{1},
                   /*addend=*/0});
    mod.functions.push_back(std::move(fn));
    ExternImport dataExt{SymbolId{99}, "_fmode", "msvcrt.dll"};
    dataExt.isData = true;
    mod.externImports.push_back(std::move(dataExt));

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u)
        << "the data-import gate must admit the declared got-indirect "
           "binding (c149) -- no more K_FormatLacksImportSupport wall";
    ASSERT_FALSE(img.empty());

    auto const [textRva, textPtr] =
        findExecSection(img, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const [idataRva, idataPtr] =
        findExecSection(img, {'.', 'i', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(textRva, 0u);
    ASSERT_NE(idataRva, 0u);
    (void)idataPtr;

    // (1) NO thunk was emitted for the data extern: `.text` holds
    //     exactly the 8 function bytes — the thunk block is COMPACTED
    //     to the function externs (here: none). A dead data thunk
    //     would grow VirtualSize to 14.
    EXPECT_EQ(findExecSectionVirtualSize(
                  img, {'.', 't', 'e', 'x', 't', 0, 0, 0}),
              8u)
        << "a DATA extern must NOT reserve/emit an FF 25 import thunk "
           "-- thunk bytes are per-FUNCTION-extern only";

    // (2) `.idata` layout for 1 library x 1 extern: descriptors
    //     (1+1)*20 = 40 (8-aligned) -> ILT @40 (extern + terminator =
    //     16 B) -> IAT @56. The IAT data directory must point at it.
    std::uint32_t const iatDirRva  = readU32LE(img, 0x108 + 12 * 8);
    std::uint32_t const iatDirSize = readU32LE(img, 0x108 + 12 * 8 + 4);
    EXPECT_EQ(iatDirRva, idataRva + 56u);
    EXPECT_EQ(iatDirSize, 16u);  // 1 slot + u64 terminator

    // (3) THE BINDING: the mov's disp32 resolves the data extern to
    //     its IAT SLOT VA — inside .idata, at exactly the slot the
    //     import machinery lays out — NOT a .text thunk.
    auto const movDisp =
        static_cast<std::int32_t>(readU32LE(img, textPtr + 3u));
    std::int64_t const targetRva =
        static_cast<std::int64_t>(textRva) + 7 + movDisp;
    std::uint32_t const idataVSize = findExecSectionVirtualSize(
        img, {'.', 'i', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(idataVSize, 0u);
    EXPECT_GE(targetRva, static_cast<std::int64_t>(idataRva))
        << "data-extern reference must resolve into .idata (the IAT "
           "slot), not .text";
    EXPECT_LT(targetRva,
              static_cast<std::int64_t>(idataRva) + idataVSize)
        << "data-extern reference must land inside .idata's extent";
    EXPECT_EQ(targetRva, static_cast<std::int64_t>(idataRva) + 56)
        << "data-extern symbolVa must be the IAT SLOT VA (idata+56 for "
           "1 lib x 1 extern), the loader-filled __imp_ pointer";

    // (4) The import machinery resolves a data import's NAME exactly
    //     like a function's: HINT/NAME carries `_fmode`, the
    //     descriptor names msvcrt.dll.
    std::uint32_t const idataFileOff = readU32LE(
        img, 0x188 + 40 + 20);  // section[1] (.idata) PointerToRawData
    std::string_view const hay{
        reinterpret_cast<char const*>(img.data() + idataFileOff),
        idataVSize};
    EXPECT_NE(hay.find("_fmode"), std::string_view::npos);
    EXPECT_NE(hay.find("msvcrt.dll"), std::string_view::npos);
}

TEST(PeExecWriter, MixedFunctionAndDataExternsThunkForFunctionSlotForData) {
    // c149 (D-LK-EXTERN-DATA-IMPORT, PE half), the MIXED-module pin:
    // one FUNCTION extern (called) + one DATA extern (read) in ONE
    // library. The function extern keeps its c112 `FF 25` thunk +
    // thunk-VA binding; the data extern binds to its IAT slot VA; the
    // IAT carries BOTH entries (+ terminator); ONE import descriptor.
    // Mirrors macho.cpp's c119 DataExternGetsGotSlotButNoStub
    // compaction pin (funcExternIdxs — thunk index j != extern index i).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    // main: `E8 <rel32>` call puts (offset 0, patch @1)
    //       `48 8B 05 <disp32>` mov rax,[rip+_fmode] (offset 5, patch @8)
    //       `C3` ret                                  -> 13 bytes total.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0,
                 0x48, 0x8B, 0x05, 0, 0, 0, 0,
                 0xC3};
    fn.relocations.push_back(
        Relocation{/*offset=*/1, SymbolId{99}, RelocationKind{1}, 0});
    fn.relocations.push_back(
        Relocation{/*offset=*/8, SymbolId{98}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "puts", "msvcrt.dll"});
    ExternImport dataExt{SymbolId{98}, "_fmode", "msvcrt.dll"};
    dataExt.isData = true;
    mod.externImports.push_back(std::move(dataExt));

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(img.empty());

    auto const [textRva, textPtr] =
        findExecSection(img, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const [idataRva, idataPtr] =
        findExecSection(img, {'.', 'i', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(textRva, 0u);
    ASSERT_NE(idataRva, 0u);
    (void)idataPtr;

    // (1) Thunk count = FUNCTION externs only: 13 fn bytes + ONE
    //     6-byte thunk = 19. A per-extern (lockstep) reservation would
    //     give 25.
    EXPECT_EQ(findExecSectionVirtualSize(
                  img, {'.', 't', 'e', 'x', 't', 0, 0, 0}),
              19u)
        << "exactly one FF 25 thunk (the function extern's) -- the data "
           "extern must not reserve thunk bytes";

    // (2) The single thunk sits at .text offset 13 and jumps through
    //     the FUNCTION extern's IAT slot. `.idata` layout for 1 lib x
    //     2 externs: descriptors 40 -> ILT @40 (2 externs + term = 24)
    //     -> IAT @64; extern[0] (puts) slot @64, extern[1] (_fmode)
    //     slot @72.
    std::size_t const thunkFileOff = static_cast<std::size_t>(textPtr) + 13u;
    std::uint32_t const thunkRva   = textRva + 13u;
    ASSERT_LT(thunkFileOff + 6u, img.size());
    EXPECT_EQ(img[thunkFileOff + 0], 0xFFu);
    EXPECT_EQ(img[thunkFileOff + 1], 0x25u);
    auto const thunkDisp =
        static_cast<std::int32_t>(readU32LE(img, thunkFileOff + 2u));
    EXPECT_EQ(static_cast<std::int64_t>(thunkRva) + 6 + thunkDisp,
              static_cast<std::int64_t>(idataRva) + 64)
        << "the function thunk must jump through ITS OWN IAT slot "
           "(idata+64), lockstep with the import layout";

    // (3) The CALL resolves to the thunk (c112 function path,
    //     unchanged by c149).
    auto const callDisp =
        static_cast<std::int32_t>(readU32LE(img, textPtr + 1u));
    EXPECT_EQ(static_cast<std::int64_t>(textRva) + 5 + callDisp,
              static_cast<std::int64_t>(thunkRva))
        << "function extern still binds to its thunk VA";

    // (4) The DATA reference resolves to the data extern's IAT slot
    //     (idata+72) — in .idata, distinct from the function's slot.
    auto const movDisp =
        static_cast<std::int32_t>(readU32LE(img, textPtr + 8u));
    std::int64_t const dataTargetRva =
        static_cast<std::int64_t>(textRva) + 12 + movDisp;
    EXPECT_EQ(dataTargetRva, static_cast<std::int64_t>(idataRva) + 72)
        << "data extern must bind to ITS IAT slot VA (idata+72)";
    EXPECT_GE(dataTargetRva, static_cast<std::int64_t>(idataRva))
        << "data extern must never bind into .text";

    // (5) Import accounting: ONE descriptor (1 lib + terminator = 40),
    //     IAT block = 2 externs + terminator = 24 bytes; both names in
    //     the HINT/NAME table.
    EXPECT_EQ(readU32LE(img, 0x114), 40u);
    EXPECT_EQ(readU32LE(img, 0x108 + 12 * 8), idataRva + 64u);
    EXPECT_EQ(readU32LE(img, 0x108 + 12 * 8 + 4), 24u);
    std::uint32_t const idataFileOff = readU32LE(img, 0x188 + 40 + 20);
    std::uint32_t const idataVSize = findExecSectionVirtualSize(
        img, {'.', 'i', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(idataVSize, 0u);
    std::string_view const hay{
        reinterpret_cast<char const*>(img.data() + idataFileOff),
        idataVSize};
    EXPECT_NE(hay.find("puts"), std::string_view::npos);
    EXPECT_NE(hay.find("_fmode"), std::string_view::npos);
    EXPECT_NE(hay.find("msvcrt.dll"), std::string_view::npos);
}

TEST(PeExecWriter, DataExternUnderUndeclaredDataImportBindingFailsLoud) {
    // c149 walker-side binding assertion (the SAME assertion elf.cpp and
    // macho.cpp carry): the PE exec walker implements exactly
    // `got-indirect`, so a pe-exec schema that does NOT declare that
    // model must be rejected LOUD rather than silently handed an
    // IAT-slot binding it never declared.
    //
    // ⓘ THE FIXTURE USED TO DECLARE A FOREIGN MODEL — `"copy-relocation"`,
    // a valid enum value with the wrong walker. That value no longer
    // EXISTS (D-LK-ELF-COPY-RELOC-CLAIMS-ONE-NAME-OF-AN-ALIAS-SET deleted
    // it), and a fixture spelling it would now be refused AT LOAD, which
    // would test the loader instead of this walker. So the fixture omits
    // the key entirely: that is the only remaining way to reach this
    // walker without the model it implements, and it is reachable because
    // this test drives the walker DIRECTLY — the linker's pre-walker gate,
    // which would also refuse a nullopt binding, is bypassed here on
    // purpose so the walker's own belt is what gets measured.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LLP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name":"pe-exec-undeclared-data-binding","kind":"pe"},
      "externCallDispatch": "direct-plt",
      "$entryClusterComment": "D-LK10-ENTRY: validate() now REJECTS an exec-flavored format that declares no processExit, so a synthetic pe-exec schema must carry the pair. Values copied verbatim from the shipped pe64-x86_64-windows-exec.format.json; inert here (the walker is driven directly, no trampoline) but required to load.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}],
      "relocations":[{"name":"IMAGE_REL_AMD64_REL32","kind":1,"nativeId":4}]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        Relocation{3u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    ExternImport dataExt{SymbolId{99}, "_fmode", "msvcrt.dll"};
    dataExt.isData = true;
    mod.externImports.push_back(std::move(dataExt));
    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, **target, **fmt, rep);
    EXPECT_TRUE(img.empty());
    EXPECT_GE(::dss::test_support::countCode(
                  rep, DiagnosticCode::K_FormatLacksImportSupport),
              1u)
        << "the walker must reject a data-import binding model it does "
           "not implement -- never a silent IAT-slot bind";
}

TEST(LinkerEndToEnd, PeExecAcceptsAndBindsDataExternImport) {
    // c149 end-to-end acceptance through linker::link with the SHIPPED
    // pe64-x86_64-windows-exec schema: the pre-walker data-import gate
    // (linker.cpp K_FormatLacksImportSupport) now ADMITS a surviving
    // extern DATA import because the format declares got-indirect —
    // the exact call path that rejected before c149 (see
    // Linker.ImageWithNoDataBindingStillRejectsReferencedDataExtern
    // for the reverted-JSON red pin). The shipped format also injects
    // the ExitProcess entry trampoline, so this pins gate+walker+
    // trampoline coexistence rather than exact byte layout (that is
    // DataExternBindsToIatSlotInIdataNotTextThunk's job).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        Relocation{3u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    ExternImport dataExt{SymbolId{99}, "_fmode", "msvcrt.dll"};
    dataExt.isData = true;
    mod.externImports.push_back(std::move(dataExt));
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    EXPECT_EQ(rep.errorCount(), 0u)
        << "the linker's data-import gate must pass under the declared "
           "got-indirect binding (c149)";
    EXPECT_TRUE(img.ok());
    ASSERT_FALSE(img.bytes.empty());
    // The import surfaced: the emitted image's import-name list carries
    // the data extern (the walker resolved its name like any import).
    EXPECT_EQ(std::count(img.externImportNames.begin(),
                         img.externImportNames.end(),
                         std::string{"_fmode"}),
              1);
}

TEST(PeExecWriter, FunctionUnwindInfoEmitsPdataXdataAndExceptionDataDir) {
    // D-WIN64-PDATA-XDATA-UNWIND host-independent structural pin. A pe64
    // function carrying call-frame information (frame alloc + callee-saves) gets
    // a .pdata RUNTIME_FUNCTION + a .xdata UNWIND_INFO, and the EXCEPTION
    // data directory (index 3) points at .pdata. Also pins the c114 FPR
    // decision: a saved FPR (MS-x64 xmm6..15, spilled low-64 via MOVSD) is
    // OMITTED from the unwind codes (no matching UWOP; RSP-irrelevant) while
    // its 8-byte store STILL advances the following GPR saves' CodeOffsets.
    //
    // Frame 0x20 (ALLOC_SMALL slots=4) + prologue-order saves
    // [xmm6@0, rbx@8, rbp@16]; the `mov` cursor starts at allocLen=7 (sub
    // rsp,imm32) then +8 per store: xmm6→15 (omitted), rbx→23, rbp→31.
    // RED-on-disable: reverting the FPR-omit to the old fail-loud makes
    // encode() report an error; mis-passing the DSS ordinal (30) for xmm6
    // instead of its hwEncoding (6) makes its width 9 → rbx CodeOffset 24,
    // not 23; dropping the emission entirely removes .pdata/.xdata. Runs on
    // every leg (pure byte inspection, no execution).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // sub rsp, 0x20 (48 81 EC 20 00 00 00) ; ret (C3) — first byte 0x48
    // satisfies the prologue-shape guard; the rest is opaque to the builder.
    fn.bytes = {0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00, 0xC3};
    // The CFI states each rule's PC DIRECTLY -- these are the byte offsets the
    // assembler measures for this prologue, not lengths the writer re-derives
    // from an assumed encoding. Frame 0x20 on a convention whose CALL pushes 8,
    // so the CFA is 8+0x20 = 40 once the sub retires, and each save's
    // CFA-relative offset is its RSP slot minus 40. xmm6 is physical ordinal 22
    // (the FPR file starts at 16); its HARDWARE encoding is 6.
    CfiFunction cfi;
    cfi.codeLength    = 8;
    cfi.initial       = CfiInitialState{/*cfaRegister=*/4, /*cfaOffset=*/8,
                                        /*returnAddressAtCfaOffset=*/-8,
                                        /*returnAddressRegister=*/std::nullopt};
    cfi.prologueEndPc = 31;
    cfi.ops = {
        CfiOp{7,  CfiOpKind::DefCfaOffset,   CfiRegRef{},             CfiRegRef{},  40},
        CfiOp{15, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(22), CfiRegRef{}, -40},
        CfiOp{23, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(3),  CfiRegRef{}, -32},
        CfiOp{31, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(5),  CfiRegRef{}, -24},
    };
    fn.cfi = std::move(cfi);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);   // the FPR save no longer fails loud
    ASSERT_FALSE(img.empty());

    auto const [textRva, textPtr] =
        findExecSection(img, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const [pdataRva, pdataPtr] =
        findExecSection(img, {'.', 'p', 'd', 'a', 't', 'a', 0, 0});
    auto const [xdataRva, xdataPtr] =
        findExecSection(img, {'.', 'x', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(textRva, 0u);
    ASSERT_NE(pdataRva, 0u) << ".pdata section must exist";
    ASSERT_NE(xdataRva, 0u) << ".xdata section must exist";

    // (1) The EXCEPTION data directory (index 3 → file 0x108 + 3*8 = 0x120)
    //     points at .pdata, size = one 12-byte RUNTIME_FUNCTION.
    EXPECT_EQ(readU32LE(img, 0x120), pdataRva) << "EXCEPTION dir RVA -> .pdata";
    EXPECT_EQ(readU32LE(img, 0x124), 12u)      << "EXCEPTION dir size = 1 entry";

    // (2) The RUNTIME_FUNCTION covers the function and points at its .xdata.
    EXPECT_EQ(readU32LE(img, pdataPtr + 0u), textRva)      << "BeginAddress";
    EXPECT_EQ(readU32LE(img, pdataPtr + 4u), textRva + 8u) << "EndAddress";
    EXPECT_EQ(readU32LE(img, pdataPtr + 8u), xdataRva)     << "UnwindInfoAddress";

    // (3) The UNWIND_INFO, byte-for-byte. Header: Ver=1/Flags=0, SizeOfProlog
    //     = 31, CountOfCodes = 5 (2 GPR saves ×2 nodes + 1 ALLOC_SMALL) — NOT
    //     7 (the xmm6 save contributes NO code), FrameReg/Off = 0. Codes are
    //     DESCENDING by CodeOffset: rbp(31), rbx(23), then ALLOC(7).
    std::size_t const u = xdataPtr;
    EXPECT_EQ(img[u + 0], 0x01u) << "Version=1, Flags=0";
    EXPECT_EQ(img[u + 1], 31u)   << "SizeOfProlog";
    EXPECT_EQ(img[u + 2], 5u)    << "CountOfCodes: xmm6 omitted (else 7)";
    EXPECT_EQ(img[u + 3], 0x00u) << "FrameRegister=0, FrameOffset=0";
    // rbp: UWOP_SAVE_NONVOL(4) | reg 5<<4 = 0x54, CodeOffset 31, node 16/8=2.
    EXPECT_EQ(img[u + 4], 31u)   << "rbp CodeOffset";
    EXPECT_EQ(img[u + 5], 0x54u) << "rbp SAVE_NONVOL | reg=5";
    EXPECT_EQ(readU16LE(img, u + 6), 2u) << "rbp scaled offset 16/8";
    // rbx: 0x34, CodeOffset 23 -- the PC the CFI STATED, carried through
    // untouched. The builder no longer computes it from an assumed store width,
    // which is exactly the defect that left every FPR-saving pe64 function's
    // SizeOfProlog one byte short PER SAVE
    // (D-WIN64-UNWIND-XMM-SPILL-WIDTH-ASSUMED-NINE-MEASURED-TEN).
    EXPECT_EQ(img[u + 8], 23u)   << "rbx CodeOffset (xmm6 width was 8)";
    EXPECT_EQ(img[u + 9], 0x34u) << "rbx SAVE_NONVOL | reg=3";
    EXPECT_EQ(readU16LE(img, u + 10), 1u) << "rbx scaled offset 8/8";
    // ALLOC_SMALL(2) | (slots-1=3)<<4 = 0x32, CodeOffset 7 (end of sub rsp).
    EXPECT_EQ(img[u + 12], 7u)    << "ALLOC CodeOffset";
    EXPECT_EQ(img[u + 13], 0x32u) << "ALLOC_SMALL | (slots-1)=3";
}

TEST(PeExecWriter, FunctionUnwindInfoStackProbePrologueUsesFixedAllocLen) {
    // D-WIN64-PDATA-XDATA-UNWIND + D-WIN64-LARGE-FRAME-STACK-PROBE. A pe64
    // function whose frame exceeds one guard page emits the inline page-probe
    // prologue (mov r11d,frame / <28-B runtime loop> / sub rsp,r11), which is a
    // FIXED 37 bytes — the loop ITERATES at runtime, it is NOT unrolled per
    // page. The unwind builder's `allocLen` for the probe path must be that
    // fixed 37 (kStackProbeLoopBytes), so SizeOfProlog + every CodeOffset stay
    // pinned to the real instruction boundaries.
    //
    // RED-on-disable: the c114 audit caught a drifted `9 + 28*pages + 3`
    // formula here → for this 8192-B frame (pages=2) it computed allocLen 68,
    // putting SizeOfProlog at 84 and the saves' CodeOffsets at 76/84 — 31 bytes
    // past the real prologue → a silently-wrong table RtlVirtualUnwind would
    // mis-read. This test fails on that formula and passes only on the fixed 37.
    // The prior (small-frame) unwind test exercises only the `sub rsp,imm32`
    // (allocLen 7) path, so this is the path that was untested.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // The probe prologue's first byte is 0x41 (mov r11d,imm) — the builder's
    // prologue-shape guard checks exactly that; the rest of the body is opaque.
    fn.bytes = {0x41, 0xBB, 0x00, 0x20, 0x00, 0x00, 0xC3, 0x90};
    // Probe path: the CFI states the MEASURED end of the 37-byte inline
    // page-probe loop (37) and of each 8-byte GPR store (45, 53). The writer no
    // longer keeps a private copy of `kStackProbeLoopBytes` to reach these.
    CfiFunction cfi;
    cfi.codeLength    = 8;
    cfi.initial       = CfiInitialState{/*cfaRegister=*/4, /*cfaOffset=*/8,
                                        /*returnAddressAtCfaOffset=*/-8,
                                        /*returnAddressRegister=*/std::nullopt};
    cfi.prologueEndPc = 53;
    cfi.ops = {
        CfiOp{37, CfiOpKind::DefCfaOffset,   CfiRegRef{},            CfiRegRef{},  0x2008},
        CfiOp{45, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(3), CfiRegRef{}, -0x2008},
        CfiOp{53, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(6), CfiRegRef{}, -0x2000},
    };
    fn.cfi = std::move(cfi);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);

    auto const [xdataRva, xdataPtr] =
        findExecSection(img, {'.', 'x', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(xdataRva, 0u);

    // allocLen = 37 (0x25). Saves ride from there: rbx CodeOffset 37+8=45,
    // rsi 45+8=53; SizeOfProlog = 53. ALLOC_LARGE (frame>128, slots=1024) →
    // opinfo 0, one u16 node = 1024. CountOfCodes = 2+2+2 = 6.
    std::size_t const w = xdataPtr;
    EXPECT_EQ(img[w + 0], 0x01u) << "Version=1, Flags=0";
    EXPECT_EQ(img[w + 1], 53u)   << "SizeOfProlog = 37 (fixed probe) + 8 + 8";
    EXPECT_EQ(img[w + 2], 6u)    << "CountOfCodes: 2 saves*2 + ALLOC_LARGE 2";
    EXPECT_EQ(img[w + 3], 0x00u) << "FrameRegister=0";
    // rsi (highest CodeOffset first): SAVE_NONVOL(4) | reg 6<<4 = 0x64, off 53.
    EXPECT_EQ(img[w + 4], 53u)   << "rsi CodeOffset (probe was 37, not 68)";
    EXPECT_EQ(img[w + 5], 0x64u) << "rsi SAVE_NONVOL | reg=6";
    EXPECT_EQ(readU16LE(img, w + 6), 1u);
    // rbx: 0x34, CodeOffset 45 = 37 + 8.
    EXPECT_EQ(img[w + 8], 45u)   << "rbx CodeOffset = 37 + 8";
    EXPECT_EQ(img[w + 9], 0x34u) << "rbx SAVE_NONVOL | reg=3";
    EXPECT_EQ(readU16LE(img, w + 10), 0u);
    // ALLOC_LARGE(1) | opinfo 0 = 0x01, CodeOffset 37 (0x25), u16 node = 1024.
    EXPECT_EQ(img[w + 12], 37u)   << "ALLOC CodeOffset = fixed probe length";
    EXPECT_EQ(img[w + 13], 0x01u) << "ALLOC_LARGE | opinfo 0";
    EXPECT_EQ(readU16LE(img, w + 14), 1024u) << "frame/8 = 8192/8";
}

TEST(PeExecWriter, SehScopeTableEmitsEhandlerFlagAndScopeRecord) {
    // c116 (D-WIN64-SEH-FUNCLETS) host-independent
    // structural pin. A function carrying call-frame information with a non-empty
    // `sehScopes` gets:
    //   (1) UNW_FLAG_EHANDLER in its UNWIND_INFO byte0 (0x01 → 0x09);
    //   (2) after the DWORD-aligned unwind codes: a u32 handler RVA (the
    //       __C_specific_handler THUNK RVA) + a SCOPE_TABLE {u32 Count;
    //       Record[]{Begin, End, Handler(=filter funclet RVA), JumpTarget}}.
    // The scope's Begin/End/JumpTarget are the parent function's RVA + the
    // SehScopeEntry byte offsets; Handler is the filter funclet FUNCTION's RVA;
    // the handler field is the personality import's thunk RVA (a .text address).
    //
    // RED-on-disable: reverting the EHANDLER byte leaves byte0 at 0x01; dropping
    // the scope-table emission removes the trailing handler RVA + records (the
    // .xdata blob is then just the codes). A funclet whose symbol has no function
    // RVA fails loud.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    // Function 0: the PARENT — sub rsp,0x20 (frame) then ret. One SEH scope over
    // a guarded body at [begin=0x08, end=0x10) with a handler block at 0x18. The
    // filter is function 1; the personality is the __C_specific_handler extern.
    AssembledFunction parent;
    parent.symbol = SymbolId{1};
    parent.bytes  = {0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00, 0xC3,   // 0..7
                     0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,   // 8..15
                     0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,   // 16..23
                     0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};  // 24..31
    {
        CfiFunction cfi;
        cfi.codeLength    = 32;
        cfi.initial       = CfiInitialState{4, 8, -8, std::nullopt};
        cfi.prologueEndPc = 7;
        cfi.ops = { CfiOp{7, CfiOpKind::DefCfaOffset, CfiRegRef{},
                          CfiRegRef{}, 0x28} };
        parent.cfi = std::move(cfi);
        SehScopeEntry sc;
        sc.beginByteOffset      = 0x08;
        sc.endByteOffset        = 0x10;
        sc.jumpTargetByteOffset = 0x18;
        sc.filterFuncletSymbol  = SymbolId{2};   // function 1
        sc.personalitySymbol    = SymbolId{3};   // the extern
        // ★ SEH scopes are now a SIBLING of the frame description, not a member
        //   of it: an exception scope is a source-language construct, not a
        //   statement about where a register was spilled.
        parent.sehScopes.push_back(sc);
    }
    mod.functions.push_back(std::move(parent));
    // Function 1: the FILTER FUNCLET — a trivial `xor eax,eax; ret`. It carries a
    // frame so it gets its own (EHANDLER-less) UNWIND_INFO; the scope table's
    // Handler points at its function RVA.
    AssembledFunction funclet;
    funclet.symbol = SymbolId{2};
    funclet.bytes  = {0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00, 0xC3};
    {
        CfiFunction cfi;
        cfi.codeLength    = 8;
        cfi.initial       = CfiInitialState{4, 8, -8, std::nullopt};
        cfi.prologueEndPc = 7;
        cfi.ops = { CfiOp{7, CfiOpKind::DefCfaOffset, CfiRegRef{},
                          CfiRegRef{}, 0x28} };
        funclet.cfi = std::move(cfi);
    }
    mod.functions.push_back(std::move(funclet));
    // The __C_specific_handler personality import (SEH-gated — synthesized on
    // demand, exactly as the SEH pass does). A real msvcrt.dll export.
    mod.externImports.push_back(
        ExternImport{SymbolId{3}, "__C_specific_handler", "msvcrt.dll",
                     /*isData=*/false});

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(img.empty());

    auto const [textRva, textPtr] =
        findExecSection(img, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const [xdataRva, xdataPtr] =
        findExecSection(img, {'.', 'x', 'd', 'a', 't', 'a', 0, 0});
    ASSERT_NE(textRva, 0u);
    ASSERT_NE(xdataRva, 0u) << ".xdata section must exist";

    // The parent (function 0) is first in .text, so its RVA == textRva; its
    // UNWIND_INFO is the first .xdata blob (xdataPtr). The funclet (function 1)
    // follows the parent in .text: parent is 32 bytes → funclet RVA = textRva+32.
    std::uint32_t const parentRva  = textRva;
    std::uint32_t const funcletRva = textRva + 32u;

    // (1) UNW_FLAG_EHANDLER: byte0 = 0x09 (Version 1 | Flags(1)<<3), NOT 0x01.
    std::size_t const u = xdataPtr;
    EXPECT_EQ(img[u + 0], 0x09u) << "UNW_FLAG_EHANDLER set (else 0x01)";
    EXPECT_EQ(img[u + 1], 7u)    << "SizeOfProlog (sub rsp,imm32)";
    EXPECT_EQ(img[u + 2], 1u)    << "CountOfCodes: 1 ALLOC_SMALL node";
    // (2) After the header (4B) + 1 code node (2B), DWORD-aligned → 8 bytes. Then:
    //     [8]  u32 handler RVA (the __C_specific_handler thunk — a .text address)
    //     [12] u32 scope Count = 1
    //     [16] Begin, [20] End, [24] Handler, [28] JumpTarget
    std::uint32_t const handlerThunkRva = readU32LE(img, u + 8);
    EXPECT_GE(handlerThunkRva, textRva) << "handler field is a .text thunk RVA";
    EXPECT_EQ(readU32LE(img, u + 12), 1u) << "scope Count = 1";
    EXPECT_EQ(readU32LE(img, u + 16), parentRva + 0x08u) << "Begin = parent+0x08";
    EXPECT_EQ(readU32LE(img, u + 20), parentRva + 0x10u) << "End = parent+0x10";
    EXPECT_EQ(readU32LE(img, u + 24), funcletRva)        << "Handler = funclet RVA";
    EXPECT_EQ(readU32LE(img, u + 28), parentRva + 0x18u) << "JumpTarget = parent+0x18";
}

TEST(PeExecWriter, SehGuardingFunctionSavingNonVolatileXmmFailsLoud) {
    // c116 (D-WIN64-XMM-UNWIND-RESTORE, the H5 invariant): a __try-guarding
    // function (non-empty sehScopes) that spills a NON-VOLATILE xmm must FAIL
    // LOUD, not silently emit an unwind table that omits UWOP_SAVE_XMM128 — the
    // __except handler resumes in the parent frame and could read an unrestored
    // xmm. sqlite's WAL SEH functions spill zero non-volatile xmm (the H5 proof),
    // so this never fires for the shipped corpus; the guard converts that proof
    // into an ENFORCED invariant. RED-on-disable: drop the guardsSeh arm in
    // pe.cpp buildFunctionUnwindInfo → the FPR save is silently omitted (as it
    // legitimately is for a NON-SEH function) → this function encodes cleanly and
    // ships a broken unwind table. The paired negative (a NON-SEH function with
    // the same xmm save encodes fine) is the FunctionUnwindInfoEmitsPdata... test
    // above (xmm6 omitted, no error).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    ASSERT_TRUE(loaded.target);

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    // Same parent shape as the scope-table test, but its prologue ALSO saves a
    // non-volatile xmm (xmm6). sub rsp,0x20 (7B) then movsd [rsp],xmm6 — the
    // bytes past [0] are opaque to the unwind builder (it reads the CFI).
    AssembledFunction parent;
    parent.symbol = SymbolId{1};
    parent.bytes  = {0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00,          // sub rsp,0x20
                     0xF2, 0x0F, 0x11, 0x34, 0x24,                      // movsd [rsp],xmm6
                     0x90, 0x90, 0x90, 0xC3};
    {
        CfiFunction cfi;
        cfi.codeLength    = 16;
        cfi.initial       = CfiInitialState{4, 8, -8, std::nullopt};
        cfi.prologueEndPc = 17;
        cfi.ops = {
            CfiOp{7,  CfiOpKind::DefCfaOffset,   CfiRegRef{},             CfiRegRef{}, 0x28},
            // xmm6 = physical ordinal 22 (non-volatile under ms_x64).
            CfiOp{17, CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(22), CfiRegRef{}, -0x28},
        };
        parent.cfi = std::move(cfi);
        SehScopeEntry sc;
        sc.beginByteOffset      = 0x0C;
        sc.endByteOffset        = 0x0F;
        sc.jumpTargetByteOffset = 0x0F;
        sc.filterFuncletSymbol  = SymbolId{2};
        sc.personalitySymbol    = SymbolId{3};
        parent.sehScopes.push_back(sc);
    }
    mod.functions.push_back(std::move(parent));
    AssembledFunction funclet;
    funclet.symbol = SymbolId{2};
    funclet.bytes  = {0x48, 0x81, 0xEC, 0x20, 0x00, 0x00, 0x00, 0xC3};
    {
        CfiFunction cfi;
        cfi.codeLength    = 8;
        cfi.initial       = CfiInitialState{4, 8, -8, std::nullopt};
        cfi.prologueEndPc = 7;
        cfi.ops = { CfiOp{7, CfiOpKind::DefCfaOffset, CfiRegRef{},
                          CfiRegRef{}, 0x28} };
        funclet.cfi = std::move(cfi);
    }
    mod.functions.push_back(std::move(funclet));
    mod.externImports.push_back(
        ExternImport{SymbolId{3}, "__C_specific_handler", "msvcrt.dll",
                     /*isData=*/false});

    DiagnosticReporter rep;
    auto img = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_GT(rep.errorCount(), 0u)
        << "a SEH-guarding function saving a non-volatile xmm must fail loud "
           "(D-WIN64-XMM-UNWIND-RESTORE), not silently omit its restore";
}

namespace {
// D-WIN64-PDATA-ARM64-SILENT-SKIP fixture. A SYNTHETIC pe-exec schema
// whose only meaningful divergence from the shipped
// pe64-x86_64-windows-exec is `pe.machine` = 0xAA64 (IMAGE_FILE_MACHINE_
// ARM64, 43620) instead of 0x8664. No arm64-PE format SHIPS — that is
// precisely why the walker's unwind gap had no leg to expose it, and why
// the fixture has to be synthesized here. Everything else (convention,
// relocation vocabulary) is spelled arm64-honestly so the fixture never
// reads as "x64 with one number changed".
[[nodiscard]] char const* arm64PeExecSchemaJson() {
    return R"({
      "$comment": "SYNTHETIC arm64 PE-exec schema for D-WIN64-PDATA-ARM64-SILENT-SKIP. No arm64-PE format ships; this exists only to drive pe::encode with a machine the unwind path has no emitter for.",
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
      "dataModel": "LLP64",
      "headerNameMatching": "case-sensitive",
      "format": {"name": "pe-exec-arm64-synthetic", "version": "1.0", "kind": "pe"},
      "entryPoint": "",
      "externCallDispatch": "direct-plt",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "aapcs64",
      "pe": {"machine": 43620, "characteristics": 34, "type": "exec"},
      "optionalHeader": {"magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "majorOperatingSystemVersion": 6, "minorOperatingSystemVersion": 0, "majorSubsystemVersion": 6, "minorSubsystemVersion": 0, "subsystem": 3, "dllCharacteristics": 33120, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096},
      "sections": [
        {"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}
      ],
      "relocations": [
        {"name":"IMAGE_REL_ARM64_BRANCH26","kind":1,"nativeId":3},
        {"name":"IMAGE_REL_ARM64_ADDR64","kind":2,"nativeId":14},
        {"name":"IMAGE_REL_ARM64_ADDR32","kind":3,"nativeId":1}
      ]
    })";
}

// ONE module shape, two arms. `withCfi` is the ONLY difference between
// the refusal case and the complementary encodes-fine case, so the pin
// measures the presence of unwind information and nothing else.
[[nodiscard]] AssembledModule makeArm64PeModule(bool withCfi) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // `sub sp, sp, #0x20` ; `ret` (a64, little-endian). Opaque to the
    // walker — it never decodes instruction bytes on this path.
    fn.bytes = {0xFF, 0x83, 0x00, 0xD1, 0xC0, 0x03, 0x5F, 0xD6};
    if (withCfi) {
        // The MINIMAL honest "this function carries unwind information":
        // a prologue end plus one CFA-offset rule. Deliberately free of
        // register references — the refusal must fire before anything
        // decodes a register ordinal, so the fixture cannot accidentally
        // pass/fail on an x86_64-vs-arm64 ordinal mismatch.
        CfiFunction cfi;
        cfi.codeLength    = 8;
        cfi.initial       = CfiInitialState{/*cfaRegister=*/31, /*cfaOffset=*/0,
                                            /*returnAddressAtCfaOffset=*/-8,
                                            /*returnAddressRegister=*/std::nullopt};
        cfi.prologueEndPc = 4;
        cfi.ops = { CfiOp{4, CfiOpKind::DefCfaOffset, CfiRegRef{},
                          CfiRegRef{}, 0x20} };
        fn.cfi = std::move(cfi);
    }
    mod.functions.push_back(std::move(fn));
    return mod;
}
} // namespace

TEST(PeExecWriter, NonX64MachineCarryingUnwindInfoFailsLoudButCfiFreeStillEncodes) {
    // D-WIN64-PDATA-ARM64-SILENT-SKIP. The `.pdata`/`.xdata` builder is
    // gated on `machine == 0x8664` and, before this cycle, had NO `else`:
    // an arm64 PE image was emitted happily with every function's MEASURED
    // call-frame information discarded, the EXCEPTION data directory left
    // 0/0, and not one diagnostic. Unwind info accepted and then dropped
    // yields a binary that RUNS correctly and cannot be unwound — no
    // debugger backtrace, no profiler stack, an exception thrown through
    // the frame terminates — and none of that surfaces until a core dump.
    // Building ARM64 `.pdata` v2 packed unwind is a genuinely different
    // encoding and is NOT what this pin asks for; it asks for a refusal.
    //
    // TWO ARMS, one module shape:
    //   (1) machine 0xAA64 + a function carrying `cfi` → encode FAILS with
    //       K_UnwindRuleUnrepresentable naming the machine, the sections,
    //       and the count.
    //   (2) the SAME module with NO `cfi` → encodes CLEANLY. This is the
    //       half that keeps the guard honest: the trigger is INFORMATION
    //       LOSS, not the architecture. A guard keyed on the machine alone
    //       would break a complete non-x64 encode and still pass arm (1).
    //
    // RED-on-disable (MEASURED, not asserted): deleting the `else` arm in
    // pe.cpp restores the silent skip → arm (1)'s `errorCount()` goes to 0,
    // `img` is non-empty, and the witness substring never appears.
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(arm64PeExecSchemaJson(),
                                                "synthetic-arm64-pe");
    ASSERT_TRUE(fmt.has_value());

    // ── (1) unwind information present ⇒ loud refusal ──────────────
    {
        DiagnosticReporter rep;
        auto img = encodeUntrampolined(makeArm64PeModule(/*withCfi=*/true),
                                       **target, **fmt, rep);
        EXPECT_TRUE(img.empty())
            << "an image whose unwind tables cannot be encoded must emit NO "
               "bytes -- never a silently unwind-less binary";
        EXPECT_GT(rep.errorCount(), 0u)
            << "dropping .pdata/.xdata must be LOUD (D-WIN64-PDATA-ARM64-"
               "SILENT-SKIP)";
        EXPECT_EQ(::dss::test_support::countCode(
                      rep, DiagnosticCode::K_UnwindRuleUnrepresentable),
                  1u);
        // Assert on the MESSAGE, not merely on a failure boolean: a
        // refusal that does not say WHICH machine, WHAT is missing, HOW
        // MUCH is lost and WHAT would close it sends the reader nowhere.
        bool named = false;
        for (auto const& d : rep.all()) {
            if (d.code != DiagnosticCode::K_UnwindRuleUnrepresentable) continue;
            if (d.actual.find("no unwind-table emitter for PE machine")
                    == std::string::npos) continue;
            named = true;
            EXPECT_NE(d.actual.find("0xaa64"), std::string::npos)
                << "must name the machine in hex: " << d.actual;
            EXPECT_NE(d.actual.find("1 function(s)"), std::string::npos)
                << "must say how many functions lose unwind info: " << d.actual;
            EXPECT_NE(d.actual.find("`.pdata`"), std::string::npos)
                << "must name .pdata as what goes missing: " << d.actual;
            EXPECT_NE(d.actual.find("`.xdata`"), std::string::npos)
                << "must name .xdata as what goes missing: " << d.actual;
            EXPECT_NE(d.actual.find("per-machine unwind"), std::string::npos)
                << "must state what would close it: " << d.actual;
            EXPECT_NE(d.actual.find("D-WIN64-PDATA-ARM64-SILENT-SKIP"),
                      std::string::npos)
                << "must cite the anchor for future-grep navigability: "
                << d.actual;
            break;
        }
        EXPECT_TRUE(named)
            << "the refusal's witness substring is the pin -- a bare failure "
               "boolean would stay green over a refusal for an unrelated "
               "reason";
    }

    // ── (2) SAME module, no unwind information ⇒ encodes cleanly ────
    {
        DiagnosticReporter rep;
        auto img = encodeUntrampolined(makeArm64PeModule(/*withCfi=*/false),
                                       **target, **fmt, rep);
        for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
        EXPECT_EQ(rep.errorCount(), 0u)
            << "a non-x64 PE image that carries NO call-frame information "
               "loses nothing here -- refusing it would be a gratuitous break";
        EXPECT_FALSE(img.empty());
        EXPECT_EQ(::dss::test_support::countCode(
                      rep, DiagnosticCode::K_UnwindRuleUnrepresentable),
                  0u);
        // And it really did take the no-tables path: no .pdata/.xdata, and
        // the EXCEPTION data directory (index 3) stays 0/0.
        EXPECT_EQ(findExecSection(img, {'.','p','d','a','t','a',0,0}).first, 0u);
        EXPECT_EQ(findExecSection(img, {'.','x','d','a','t','a',0,0}).first, 0u);
        EXPECT_EQ(readU32LE(img, 0x120), 0u) << "EXCEPTION dir RVA";
        EXPECT_EQ(readU32LE(img, 0x124), 0u) << "EXCEPTION dir size";
    }
}

TEST(PeExecWriter, DosHeaderMzSignature) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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

// c145 (D-LK-RELRO-CONST-DATA-RELOCATABLE): a CONST symbol-address global (relro)
// is FOLDED into read-only `.rdata` in the PE32+ image; because the exec keeps
// ASLR the walker writes the slot's PREFERRED VA AND emits an IMAGE_REL_BASED_DIR64
// `.reloc` entry (PE base-relocates `.rdata` before sealing it read-only). Same
// shape as the F5 rodata slot above — proving relro rides `.rdata` with no
// fail-loud. RED-on-disable: without the relro→`.rdata` merge the item is dropped
// or rejected and no slot / DIR64 entry appears.
TEST(PeExecWriter, RelRoConstSlotFoldsIntoRdataAndEmitsDir64BaseReloc) {
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};                       // ret — first .text fn at RVA 0x1000
    mod.functions.push_back(std::move(fn));
    AssembledData slot;
    slot.symbol  = SymbolId{2};
    slot.section = DataSectionKind::RelRoConst;   // the c145 routing
    slot.bytes.assign(8, std::uint8_t{0});
    Relocation slotRel;
    slotRel.offset = 0;
    slotRel.target = SymbolId{1};
    slotRel.kind   = RelocationKind{2};       // abs64
    slotRel.addend = 0;
    slot.relocations.push_back(slotRel);
    mod.dataItems.push_back(std::move(slot));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a relro item must NOT fail loud in a PE exec image";

    constexpr std::uint64_t kImageBase = 0x140000000ull;
    auto const text  = findExecSection(bytes, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    auto const rdata = findExecSection(bytes, {'.', 'r', 'd', 'a', 't', 'a', 0, 0});
    auto const reloc = findExecSection(bytes, {'.', 'r', 'e', 'l', 'o', 'c', 0, 0});
    ASSERT_NE(rdata.first, 0u) << "the relro slot must ride `.rdata`";
    ASSERT_NE(reloc.first, 0u) << ".reloc must carry the DIR64 for the relro slot";
    EXPECT_EQ(readU64LE(bytes, rdata.second), kImageBase + text.first)
        << "the relro slot holds the def's PREFERRED VA (imageBase + targetRVA)";
    std::uint32_t const slotRva = rdata.first;
    EXPECT_EQ(readU16LE(bytes, reloc.second + 8),
              static_cast<std::uint16_t>((10u << 12) | (slotRva & 0x0FFFu)))
        << "IMAGE_REL_BASED_DIR64 entry emitted for the relro slot in .rdata";
}

// ── D-CSUBSET-THREAD-LOCAL (TLS C3): the PE writer's .tls / directory /
// _tls_index / secrel / backstop structural pins. Host-independent (pure
// in-memory encode + parse), so Linux CI catches a regression the Windows-
// only runtime example cannot. RED-on-disable: reverting the pe.cpp TLS arm
// makes the .tls section + IMAGE_TLS_DIRECTORY64 + DataDirectory[9] vanish —
// EVERY assertion below turns red, while a single-thread runtime example
// (which a static alias also passes) would still exit 42. Structure, not
// behavior, is the PE discriminator (Windows has no pthread — see
// thread_local_win_threads for the CreateThread 2-thread runtime witness).
namespace {
[[nodiscard]] AssembledData makeTdataItem(std::uint32_t sym,
                                          std::vector<std::uint8_t> bytes,
                                          std::uint32_t align) {
    AssembledData d;
    d.symbol    = SymbolId{sym};
    d.section   = DataSectionKind::Tdata;
    d.bytes     = std::move(bytes);
    d.alignment = Alignment::ofRuntimePow2(align);
    return d;
}
[[nodiscard]] AssembledData makeTbssItem(std::uint32_t sym,
                                         std::uint64_t size,
                                         std::uint32_t align) {
    AssembledData d;
    d.symbol       = SymbolId{sym};
    d.section      = DataSectionKind::Tbss;
    d.reservedSize = size;
    d.alignment    = Alignment::ofRuntimePow2(align);
    return d;
}
} // namespace

TEST(PeExecTls, ThreadLocalEmitsTlsDirectorySectionAndIndex) {
    // g {07 00 00 00} tdata align 4 -> offset 0; counter tbss 4 bytes
    // align 4 -> tbssBlockBase alignUp(4,4)=4 -> blockMemsz 8. The WHOLE
    // block (tdata + the tbss zero region) is EMBEDDED in the .tls
    // template (SizeOfZeroFill=0) — the Windows loader does not reliably
    // zero SizeOfZeroFill for the main thread's early static TLS.
    //   .tls bytes: [ 8-byte block | pad→8=0 | dir(40) | index(4) ]
    //   dirOffset = alignUp(8,8) = 8 ; indexOffset = 48 ; fileSize = 52
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};                    // ret — first .text fn @ RVA 0x1000
    mod.functions.push_back(std::move(fn));
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    mod.dataItems.push_back(makeTbssItem(43, 4, 4));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    constexpr std::uint64_t kImageBase = 0x140000000ull;
    auto const tls   = findExecSection(bytes, {'.', 't', 'l', 's', 0, 0, 0, 0});
    auto const reloc = findExecSection(bytes, {'.', 'r', 'e', 'l', 'o', 'c', 0, 0});
    ASSERT_NE(tls.first, 0u)   << ".tls section must exist";
    ASSERT_NE(reloc.first, 0u) << ".reloc must exist (3 directory VA fixups)";
    std::uint32_t const tlsRva = tls.first;
    std::size_t const   tlsOff = tls.second;

    // DataDirectory[9] (IMAGE_DIRECTORY_ENTRY_TLS): RVA @ 0x150, Size @ 0x154
    // (optional header at 0x98; DataDirectory begins at 0x108; entry 9 = +0x48).
    EXPECT_EQ(readU32LE(bytes, 0x150), tlsRva + 8u)
        << "DataDir[9] RVA == the directory's RVA (.tls + dirOffset 8)";
    EXPECT_EQ(readU32LE(bytes, 0x154), 40u)
        << "DataDir[9] size == sizeof(IMAGE_TLS_DIRECTORY64)";

    // IMAGE_TLS_DIRECTORY64 (40 bytes) at .tls file offset + dirOffset(8):
    std::size_t const d = tlsOff + 8;
    EXPECT_EQ(readU64LE(bytes, d + 0), kImageBase + tlsRva + 0u)   // Start
        << "StartAddressOfRawData = .tls base VA";
    EXPECT_EQ(readU64LE(bytes, d + 8), kImageBase + tlsRva + 8u)   // End
        << "EndAddressOfRawData = Start + full block (tdata 4 + tbss 4)";
    EXPECT_EQ(readU64LE(bytes, d + 16), kImageBase + tlsRva + 48u) // Index
        << "AddressOfIndex = the _tls_index slot VA (.tls + 48)";
    EXPECT_EQ(readU64LE(bytes, d + 24), 0u) << "AddressOfCallBacks = 0";
    EXPECT_EQ(readU32LE(bytes, d + 32), 0u)
        << "SizeOfZeroFill = 0 (the tbss zero region is EMBEDDED in the template)";
    EXPECT_EQ(readU32LE(bytes, d + 36), 0u) << "Characteristics = 0";

    // The template: g == 7, then the tbss counter's 4 zero bytes.
    EXPECT_EQ(readU32LE(bytes, tlsOff + 0), 7u) << "g's template value";
    EXPECT_EQ(readU32LE(bytes, tlsOff + 4), 0u) << "counter's embedded zero";
    // The _tls_index slot: 4 writable zero bytes for the loader to fill.
    EXPECT_EQ(readU32LE(bytes, tlsOff + 48), 0u) << "_tls_index starts 0";

    // ★ EXACTLY 3 IMAGE_REL_BASED_DIR64 entries (Start/End/AddressOfIndex) —
    // NOT AddressOfCallBacks (a reloc on 0 → slide → non-null garbage). The
    // .tls template pointer count is 0 (g is a plain int), so 3 total.
    std::size_t o = reloc.second;
    int dir64 = 0;
    std::vector<std::uint32_t> siteRvas;
    // Walk the base-reloc blocks in the section's virtual size.
    std::uint32_t const relocSize = readU32LE(bytes, 0x134);  // DataDir[5] size
    std::size_t const end = reloc.second + relocSize;
    while (o + 8 <= end) {
        std::uint32_t const page = readU32LE(bytes, o + 0);
        std::uint32_t const bsz  = readU32LE(bytes, o + 4);
        if (bsz < 8) break;
        std::size_t const entries = (bsz - 8) / 2;
        for (std::size_t k = 0; k < entries; ++k) {
            std::uint16_t const e = readU16LE(bytes, o + 8 + k * 2);
            if ((e >> 12) == 10u) {   // IMAGE_REL_BASED_DIR64
                ++dir64;
                siteRvas.push_back(page + (e & 0x0FFFu));
            }
        }
        o += bsz;
    }
    EXPECT_EQ(dir64, 3) << "exactly 3 DIR64 base relocs (Start/End/Index)";
    // The 3 sites are the directory's 3 VA fields (dir RVA + {0,8,16}).
    std::vector<std::uint32_t> expect{tlsRva + 8u, tlsRva + 16u, tlsRva + 24u};
    std::sort(siteRvas.begin(), siteRvas.end());
    std::sort(expect.begin(), expect.end());
    EXPECT_EQ(siteRvas, expect)
        << "the DIR64 sites are the directory Start/End/AddressOfIndex VAs";
}

TEST(PeExecTls, SecrelPatchedIntoFunctionRelocIsPositiveTemplateOffset) {
    // A function `lea rax, [rax + secrel32(counter)]` carrying a kind-4
    // (tls) reloc against the tbss `counter` gets its disp32 patched to
    // counter's POSITIVE template offset (tbssBlockBase = 4) — NOT a VA and
    // NOT the ELF negative tpoff. `[block + 4]` is the per-thread address.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    // REX.W 8D 80 <disp32> = lea rax, [rax + disp32]; patch site at offset 3.
    fn.bytes  = {0x48, 0x8D, 0x80, 0, 0, 0, 0, 0xC3};
    Relocation r; r.offset = 3; r.target = SymbolId{43};
    r.kind = RelocationKind{4}; r.addend = 0;   // tls-tpoff32 (PE: SECREL)
    fn.relocations.push_back(r);
    mod.functions.push_back(std::move(fn));
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4)); // g @ 0
    mod.dataItems.push_back(makeTbssItem(43, 4, 4));             // counter @ 4

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const text = findExecSection(bytes, {'.', 't', 'e', 'x', 't', 0, 0, 0});
    ASSERT_NE(text.first, 0u);
    // disp32 at .text file offset + 3 == counter's template offset (4).
    EXPECT_EQ(readU32LE(bytes, text.second + 3), 4u)
        << "the SECREL disp32 is counter's POSITIVE template offset "
           "(tbssBlockBase), not a VA / not the ELF negative tpoff";
}

TEST(PeExecTls, DataItemRelocTargetingTlsSymbolFailsLoud) {
    // ★ Seam-(i) / CRIT-2 backstop: a DATA item carrying a relocation that
    // targets a THREAD-LOCAL symbol must FAIL LOUD — a thread-local object
    // has no link-time address (C11 6.6p9; the semantic tier rejects
    // `&tls` as 0xE048). Without the backstop the writer would embed the
    // section-relative template offset as a garbage abs64 pointer.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));
    // A .data pointer whose abs64 reloc targets the thread-local g (#42).
    AssembledData p;
    p.symbol = SymbolId{50}; p.section = DataSectionKind::Data;
    p.bytes.assign(8, 0); p.alignment = Alignment::ofRuntimePow2(8);
    Relocation r; r.offset = 0; r.target = SymbolId{42};
    r.kind = RelocationKind{2}; r.addend = 0;    // abs64 (non-tls) vs TLS target
    p.relocations.push_back(r);
    mod.dataItems.push_back(std::move(p));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& diag : rep.all())
        if (diag.code == DiagnosticCode::K_RelocationKindMismatch) saw = true;
    EXPECT_TRUE(saw) << "a data reloc targeting a TLS symbol must fail loud";
}

TEST(PeExecTls, FunctionTlsKindRelocTargetingNonTlsSymbolFailsLoud) {
    // ★ CRIT-2 backstop, the OTHER XOR arm: a tls-flagged (kind-4)
    // relocation against a NON-thread-local symbol must fail loud — it
    // would embed a VA where a section-relative offset belongs.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x8D, 0x80, 0, 0, 0, 0, 0xC3};
    Relocation r; r.offset = 3; r.target = SymbolId{60};  // a plain .rdata item
    r.kind = RelocationKind{4}; r.addend = 0;             // tls kind vs non-tls
    fn.relocations.push_back(r);
    mod.functions.push_back(std::move(fn));
    AssembledData rd;
    rd.symbol = SymbolId{60}; rd.section = DataSectionKind::Rodata;
    rd.bytes = {1, 2, 3, 4}; rd.alignment = Alignment::ofRuntimePow2(4);
    mod.dataItems.push_back(std::move(rd));
    // Also carry a real TLS item so tlsSymbols is non-empty (the writer
    // runs the tls arm) but #60 is NOT in it.
    mod.dataItems.push_back(makeTdataItem(42, {7, 0, 0, 0}, 4));

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& diag : rep.all())
        if (diag.code == DiagnosticCode::K_RelocationKindMismatch) saw = true;
    EXPECT_TRUE(saw) << "a tls-kind reloc against a non-tls symbol fails loud";
}

TEST(PeExecTls, OveralignedThreadLocalFailsLoud) {
    // ★ D-CSUBSET-THREAD-LOCAL-PE-OVERALIGN (audit FOLD-1), RED-on-disable:
    // an `_Alignas(32) thread_local` var can't be honored on PE/x64 — the
    // Windows loader guarantees only 16-byte (MEMORY_ALLOCATION_ALIGNMENT)
    // static-TLS block-base alignment and IMAGE_TLS_DIRECTORY64 has no field
    // to request more. The writer MUST fail loud
    // (K_ThreadLocalOveralignedForFormat 0x8016) rather than silently
    // under-align every thread's copy. Disable the pe.cpp gate → this
    // compiles clean = the silent miscompile (the red).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.dataItems.push_back(makeTdataItem(42, {1, 0, 0, 0}, 32));  // _Alignas(32)

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool saw = false;
    for (auto const& diag : rep.all())
        if (diag.code == DiagnosticCode::K_ThreadLocalOveralignedForFormat)
            saw = true;
    EXPECT_TRUE(saw)
        << "an over-aligned (>16) thread-local must fail loud on pe64 — "
           "the loader cannot guarantee the per-thread block alignment";
}

TEST(PeExecTls, SixteenByteAlignedThreadLocalCompilesClean) {
    // Boundary pin: alignment EXACTLY 16 (== the loader's guarantee) is the
    // largest that passes — the gate is `> 16`, not `>= 16`. Every normal
    // scalar / pointer / small aggregate thread_local (align <= 16) stays
    // green; only explicit over-alignment bites.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    mod.dataItems.push_back(makeTdataItem(42, {1, 0, 0, 0}, 16));  // _Alignas(16)

    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "16-byte alignment == the PE x64 TLS block-base guarantee — clean";
    // FATAL, not EXPECT: `findExecSection` below indexes the buffer at 0x86
    // through an unchecked `std::span::operator[]`, so a non-fatal guard let
    // an empty `bytes` run straight into UB — MEASURED as a SEGV that killed
    // the binary after 83 of 97 tests and hid the 14 that follow.
    ASSERT_FALSE(bytes.empty());
    auto const tls = findExecSection(bytes, {'.', 't', 'l', 's', 0, 0, 0, 0});
    EXPECT_NE(tls.first, 0u) << ".tls still emitted for the 16-aligned var";
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── PE32+ exec negative-load pins ───────────────────────────────
//
// Each fixture below carries exactly ONE deliberate defect and asserts
// THAT diagnostic — never a bare `ASSERT_FALSE(r.has_value())`.
//
// Two properties make the bare form unsafe here. `validate()`
// ACCUMULATES diagnostics (no early return before its `return
// problems;`), and `loadFromText` rejects when ANY accumulated
// diagnostic is an error — so the moment a NEW load rule starts
// rejecting a fixture, the bare assertion keeps passing while the rule
// the test exists to pin can be deleted outright. That already happened
// once: D-LK10-ENTRY 2.13 ("an exec-flavored format MUST declare
// `processExit`") emptied this entire family in place, silently, with
// the suite still green.
//
// So each fixture declares the entry cluster (`processExit` +
// `entryCallingConvention`) AND `pe.characteristics` = 34, all copied
// VERBATIM from the shipped pe64-x86_64-windows-exec.format.json —
// closing the two rejection reasons that are NOT the defect under test.
// And each asserts `countAtPath` on the rule's own JSON pointer plus
// `countAtPath(r, "/processExit") == 0`, the anti-subsumption half that
// goes red if the entry-cluster reason ever comes back.
TEST(PeExecFormatJsonValidate, MissingImageBaseRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"bad-pe-exec","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/optionalHeader/imageBase"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(PeExecFormatJsonValidate, ObjWithOptionalHeaderRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"obj-with-opt-hdr","kind":"pe"},
      "pe": { "machine": 34404, "type": "obj" },
      "optionalHeader": { "magic": 523 }
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: pe.machine is valid and no sections are
    // declared, so the optionalHeader-on-.obj rule is the only
    // diagnostic.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/optionalHeader"), 1u) << rejectSummary(r);
}

TEST(PeExecFormatJsonValidate, NonPow2SectionAlignmentRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"odd-align","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 3000, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 3 reasons, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    // THREE, all consequences of the one value: 3000 is not a power
    // of two, is below the 4096 page floor, and does not divide the
    // 0x1000 .text RVA. Nothing unrelated to sectionAlignment fires.
    EXPECT_EQ(errorCount(r), 3u) << rejectSummary(r);
    // TWO of those three land at this pointer (power-of-two + page
    // floor), so `GE` here; the message pin names the one this test
    // exists for.
    EXPECT_GE(countAtPath(r, "/optionalHeader/sectionAlignment"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "must be a positive power-of-two"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

// ── New tests folded from 7-agent review of LK2 cycle 2 ────────

TEST(PeExecFormatJsonValidate, SectionAlignmentBelowPageSizeRejected) {
    // PE/COFF §3.4 + code-reviewer C3 + silent-failure C3: PE32+
    // sectionAlignment must be >= 4096 (page size). Windows loader
    // rejects sub-page alignment with STATUS_INVALID_IMAGE_FORMAT.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"sub-page","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 512, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    // 512 IS a power of two -- the sole defect is the sub-page value, so
    // exactly one diagnostic lands here. The message pin separates it
    // from the power-of-two rule that shares this pointer.
    EXPECT_EQ(countAtPath(r, "/optionalHeader/sectionAlignment"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "must be >= 4096 (page size)"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(PeExecFormatJsonValidate, MissingSubsystemRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"no-subsystem","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/optionalHeader/subsystem"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(PeExecFormatJsonValidate, MissingStackHeapSizesRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"no-stack","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    // This rule reports at the BLOCK pointer `/optionalHeader` (one
    // diagnostic naming all four fields), so the message pin is what
    // distinguishes it from every `/optionalHeader/<field>` sibling that
    // `countAtPath`'s substring match would also count.
    EXPECT_EQ(countAtPath(r, "/optionalHeader"), 1u) << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "requires non-zero sizeOfStackReserve"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(PeExecFormatJsonValidate, SectionAlignmentLessThanFileAlignmentRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"sect-lt-file","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 8192, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/optionalHeader/sectionAlignment"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countWithMessage(
                  r, "sectionAlignment (4096) >= fileAlignment (8192)"),
              1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(PeExecFormatJsonValidate, VirtualAddressNotMultipleOfSectionAlignmentRejected) {
    // code-reviewer C3: secText.virtualAddress must be a multiple
    // of sectionAlignment per PE/COFF §3.4.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"misaligned-va","kind":"pe"},
      "$entryClusterComment": "Entry cluster + pe.characteristics: verbatim from the shipped pe64-x86_64-windows-exec.format.json. Present so this fixture is rejected ONLY for the defect it pins -- see the block comment above these tests.",
      "runtimeLibraries": [{"role":"cLibrary","image":"ucrtbase.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "ms_x64",
      "pe": { "machine": 34404, "characteristics": 34, "type": "exec" },
      "optionalHeader": { "magic": 523, "imageBase": 5368709120, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 3, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4097}]
    })");
    ASSERT_FALSE(r.has_value());
    // MEASURED sole-reason pin: this fixture is rejected for EXACTLY
    // 1 reason, and `errorCount` is the machine check that keeps it
    // that way -- a comment claiming isolation rots, this line goes red
    // the day an unrelated rule starts rejecting the fixture too.
    EXPECT_EQ(errorCount(r), 1u) << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/sections/0/virtualAddress"), 1u)
        << rejectSummary(r);
    EXPECT_EQ(countWithMessage(r, "must be a multiple of 'sectionAlignment'"),
              1u)
        << rejectSummary(r);
    EXPECT_EQ(countAtPath(r, "/processExit"), 0u) << rejectSummary(r);
}

TEST(PeExecWriter, DllArmEncodesEntrylessImage) {
    // c152 (D-LK2-4): the Dll arm SHIPPED — this replaces the retired
    // `DllArmFailsLoud` red pin (which pinned the pre-c152
    // not-implemented reject). The dll routes through the same
    // encodeExec substrate; the deep pins (IMAGE_FILE_DLL, entry 0,
    // .edata sort invariant, DIR64, fail-loud belts) live in
    // tests/link/test_pe_dll_writer.cpp. Here: a hand-declared dll
    // schema (validate now REQUIRES characteristics 0x2022-shaped
    // bits — EXECUTABLE_IMAGE + IMAGE_FILE_DLL) loads and encodes a
    // non-empty image with AddressOfEntryPoint == 0.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"a-dll","kind":"pe"},
      "pe": { "machine": 34404, "characteristics": 8226, "type": "dll" },
      "optionalHeader": { "magic": 523, "imageBase": 6442450944, "sectionAlignment": 4096, "fileAlignment": 512, "subsystem": 2, "dllCharacteristics": 352, "sizeOfStackReserve": 1048576, "sizeOfStackCommit": 4096, "sizeOfHeapReserve": 1048576, "sizeOfHeapCommit": 4096 },
      "sections":[{"kind":"text","name":".text","type":1616904224,"flags":0,"addrAlign":0,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_TRUE(r.has_value());
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = pe::encode(mod, **target, **r, rep);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // IMAGE_FILE_HEADER.Characteristics @ 0x96 carries IMAGE_FILE_DLL.
    EXPECT_EQ(readU16LE(bytes, 0x96) & 0x2000u, 0x2000u);
    // AddressOfEntryPoint (optional header +16 = 0x98+16) == 0.
    EXPECT_EQ(readU32LE(bytes, 0x98 + 16), 0u);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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

    // Under direct-plt (D-FFI-PE-IMPORT-THUNK) the extern call resolves
    // to the .text import THUNK (right after the 6-byte function body),
    // NOT the IAT slot directly. `E8 disp32` at text RVA 0x1000; target
    // = (textRva + 5) + disp32 == the thunk RVA (textRva + 6 = function
    // end). The thunk (`FF 25 disp32`) then jumps through the IAT slot
    // (iatDirRva) — the two-hop that makes an address-taken import
    // callable and retires the sqlite aSyscall[] crash.
    constexpr std::size_t kFirstSecHdr = 0x188;
    std::uint32_t const textRva =
        readU32LE(bytes, kFirstSecHdr + 12);
    std::uint32_t const textFileOff =
        readU32LE(bytes, kFirstSecHdr + 20);
    std::uint32_t const thunkRva = textRva + 6u;
    auto const callDisp =
        static_cast<std::int32_t>(readU32LE(bytes, textFileOff + 1));
    EXPECT_EQ(static_cast<std::int64_t>(textRva) + 5 + callDisp,
              static_cast<std::int64_t>(thunkRva))
        << "extern call must resolve to the .text import thunk, not the "
           "IAT slot";
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);
    // The thunk `FF 25 disp32` jumps through the IAT slot.
    EXPECT_EQ(bytes[textFileOff + 6], 0xFFu);
    EXPECT_EQ(bytes[textFileOff + 7], 0x25u);
    auto const thunkDisp =
        static_cast<std::int32_t>(readU32LE(bytes, textFileOff + 8));
    EXPECT_EQ(static_cast<std::int64_t>(thunkRva) + 6 + thunkDisp,
              static_cast<std::int64_t>(iatDirRva))
        << "import thunk must jump through the IAT slot";
}

TEST(PeExecWriter, IDataContainsExitProcessNameAndKernel32DllName) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeModuleWithOneExtern(
        {0xE8, 0, 0, 0, 0, 0xC3}, 1, 99, 1);
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    // test-analyzer #3: one function with TWO relocations — one to an
    // intra-module function (resolves to its .text VA) and one to an
    // extern (resolves to the extern's .text import THUNK under direct-
    // plt, D-FFI-PE-IMPORT-THUNK — the thunk then jumps through the
    // .idata IAT slot). The shared symbolVa kernel dispatches both.
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Intra-call: from text @ +1 to fn[1] @ +11 (fn[0] is 11 bytes).
    // value = 11 - 1 - 4 = 6.
    constexpr std::size_t kFirstSecHdr = 0x188;
    std::uint32_t const textFileOff =
        readU32LE(bytes, kFirstSecHdr + 20);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 6u);
    // Extern call (direct-plt, D-FFI-PE-IMPORT-THUNK): resolves to the
    // .text import THUNK — the single extern's thunk sits after BOTH
    // function bodies (f0 11B + f1 1B = text offset 12). `E8 disp32` at
    // f0 offset 5; target = (textRva + 10) + disp == thunkRva (textRva +
    // 12), NOT the IAT slot (a `call` into .idata data → the sqlite
    // crash). The thunk then jumps through the IAT slot.
    std::uint32_t const textRva = readU32LE(bytes, kFirstSecHdr + 12);
    std::uint32_t const thunkRva = textRva + 12u;
    auto const externDisp =
        static_cast<std::int32_t>(readU32LE(bytes, textFileOff + 6));
    EXPECT_EQ(static_cast<std::int64_t>(textRva) + 10 + externDisp,
              static_cast<std::int64_t>(thunkRva))
        << "extern call must resolve to the .text import thunk";
    std::uint32_t const iatDirRva = readU32LE(bytes, 0x108 + 12*8);
    EXPECT_EQ(bytes[textFileOff + 12], 0xFFu);
    EXPECT_EQ(bytes[textFileOff + 13], 0x25u);
    auto const thunkDisp =
        static_cast<std::int32_t>(readU32LE(bytes, textFileOff + 14));
    EXPECT_EQ(static_cast<std::int64_t>(thunkRva) + 6 + thunkDisp,
              static_cast<std::int64_t>(iatDirRva))
        << "import thunk must jump through the IAT slot";
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

// c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS) UPDATED CONTRACT: an empty
// libraryPath is no longer an unconditional structural reject — it is the
// bare-prototype cross-TU reference (no library ON PURPOSE), and the linker
// applies ld's rule on the post-merge module:
//   * REFERENCED (a reloc targets it) + unbound ⇒ K_SymbolUndefined NAMING
//     the symbol, no bytes emitted (the first arm below);
//   * UNREFERENCED + unbound ⇒ the row is DROPPED and the link proceeds
//     (the second arm) — a declared-but-never-called prototype is dead
//     surface, and a leaked row would emit a broken empty-named
//     IMAGE_IMPORT_DESCRIPTOR.
// (Pre-c86 both arms hard-failed with an internal "empty libraryPath"
// wording that never named the symbol.)
TEST(LinkerExternResolution, EmptyLibraryPathReferencedIsUndefinedSymbol) {
    auto loaded = loadShippedExec();
    // call rel32 through the extern's SymbolId — the extern IS referenced.
    AssembledModule mod = makeTrivialModule({0xE8,0,0,0,0, 0xC3}, 1);
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    mod.functions[0].relocations.push_back(rel);
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "ExitProcess", ""});
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(img.bytes.empty());
    bool sawNamedUndefined = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
            && d.actual.find("undefined symbol 'ExitProcess'") != std::string::npos) {
            sawNamedUndefined = true;
        }
    }
    EXPECT_TRUE(sawNamedUndefined)
        << "a referenced unbound extern must reject LOUD, naming the symbol";
}

TEST(LinkerExternResolution, EmptyLibraryPathUnreferencedIsDropped) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);  // no relocs — unreferenced
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "ExitProcess", ""});
    DiagnosticReporter rep;
    LinkedImage img = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "an unreferenced unbound extern must not fail the link";
    EXPECT_FALSE(img.bytes.empty()) << "the image must still emit";
    EXPECT_TRUE(img.ok());
    EXPECT_EQ(std::count(img.externImportNames.begin(), img.externImportNames.end(),
                         std::string{"ExitProcess"}), 0)
        << "the dropped row must not reach the emitted import table";
}

TEST(LinkerExternResolution, DuplicateSymbolIdAcrossFunctionsAndExternsRejected) {
    auto loaded = loadShippedExec();
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: the colliding extern must be REFERENCED
    // to survive the reference gate and reach the compound-index duplicate check —
    // an unreferenced non-eager import is now dropped (which also removes the
    // collision). A relocation to SymbolId{1} keeps the extern live so the
    // ambiguous-resolution reject (fn #1 vs extern #1) still fires.
    AssembledModule mod = makeTrivialModule({0xE8,0,0,0,0, 0xC3}, /*fnSym=*/1);
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};          // references SymbolId{1} → keeps the extern live
    rel.kind   = RelocationKind{1};
    mod.functions[0].relocations.push_back(rel);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name":"elf-lazy-gate","kind":"elf"},
      "entryPoint": "",
      "$entryClusterComment": "D-LK10-ENTRY: an ELF ET_EXEC schema is exec-flavored, and validate() now REJECTS an exec-flavored format declaring no processExit -- without this pair the schema no longer LOADS and this test dies at its ASSERT_TRUE(fmt.has_value()) before it can exercise the bindNow=false walker gate at all. Values copied verbatim from the shipped elf64-x86_64-linux-exec.format.json. The link path DOES build a trampoline from them; the walker still fails loud on bindNow=false, which is what this test pins.",
      "runtimeLibraries": [{"role":"cLibrary","image":"libc.so.6"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "exit" },
      "entryCallingConvention": "sysv_amd64",
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
    // D-LINK-EXTERN-IMPORT-REFERENCE-GATE: the extern must be REFERENCED to
    // survive the reference gate and REACH the walker whose lazy-binding fail-loud
    // this test exercises — an unreferenced non-eager import is now dropped (which
    // would leave no extern for the walker to reject). CALL printf so it stays live.
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};   // call rel32 printf; ret
    fn.relocations.push_back(
        Relocation{1u, SymbolId{99}, RelocationKind{1}, 0});   // references printf
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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

// FLIPPED 2026-08-05 — was `MachORejectsDataItemsUntilD_LK3_RODATA`,
// the negative twin of the ELF test above, whose whole content was
// "Mach-O declares no `supportedDataSections`; D-LK3-RODATA still
// anchored". D-LK3-RODATA IS NOW CLOSED FOR BOTH DARWIN EXEC FORMATS:
// `macho64-arm64-darwin-exec` closed it (runtime-witnessed on Apple
// Silicon) and `macho64-x86_64-darwin-exec` joined it on 2026-08-05
// with the rest of its runnable-CLI key set. The walker arm is the
// SAME shared `exec_data_section.hpp` substrate the ELF arm uses.
//
// So this becomes the POSITIVE pin its ELF twin already is, asserted
// just as strictly and in the same shape — not deleted, not softened:
// every assertion below is the exact mirror of
// `ElfAcceptsRodataDataItemsAfterD_LK1_ELF_EXEC_DATA_SECTIONS`, and
// each one goes red if the format JSON's `supportedDataSections` row
// or its `__TEXT,__const` `sections[]` row is removed without
// re-anchoring the walker arm. The sweep in
// `AsmSubstrate.ShippedExecFormatsRodataSectionPerWalkerArm` covers
// the CONFIG half across every shipped exec format; this covers the
// END-TO-END half for Mach-O — that a rodata item survives the
// linker's gate and reaches a walker that links it cleanly.
TEST(LinkerEndToEnd, MachOAcceptsRodataDataItemsAfterD_LK3_RODATA) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped(
        "macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    DiagnosticReporter rep;
    LinkedImage img =
        runLinkerWithRodataItem(**target, **fmt, rep);
    EXPECT_TRUE(img.ok())
        << "Mach-O exec must now accept a Rodata dataItem end-to-end "
           "(D-LK3-RODATA)";
    EXPECT_EQ(rep.errorCount(), 0u);
    // resolvedFuncCount == 2: the user `ret` function PLUS the
    // synthetic `_start` trampoline the linker prepends for exec
    // images (D-LK10-ENTRY Slice C) — the same count the ELF twin
    // pins, and the marker of a clean link vs the pre-close
    // rejection at 0.
    EXPECT_EQ(img.resolvedFuncCount, 2u);
    EXPECT_TRUE((**fmt).acceptsDataSection(
        DataSectionKind::Rodata))
        << "Mach-O exec format JSON must advertise rodata now that "
           "D-LK3-RODATA has closed";
}

// D-LK4-DATA-PRODUCER writer pin (was RejectsDataKindDataItem… — flipped when
// the writable-data-sections cycle CLOSED the former fail-loud). A Data item now
// lands in a `.data` section whose Characteristics carry IMAGE_SCN_MEM_WRITE —
// the bit that makes a runtime store legal (a `.rdata` store faults). RED if the
// PE writer reverts to rejecting Data items, or emits `.data` without MEM_WRITE.
TEST(PeExecWriter, DataSectionEmittedWritable) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Data;
    d.bytes     = {0x07, 0x00, 0x00, 0x00};   // int = 7
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "PE writer must ACCEPT a Data item now that D-LK4-DATA-PRODUCER "
           "closed (it was fail-loud before)";
    ASSERT_FALSE(bytes.empty());
    // 2 sections: .text + .data (no .idata — no externs).
    EXPECT_EQ(readU16LE(bytes, 0x86), 2u) << ".text + .data";
    constexpr std::size_t kSecHdrTable = 0x188;
    constexpr std::size_t kSecHdrSize  = 40;
    constexpr std::size_t kDataHdr     = kSecHdrTable + kSecHdrSize;  // [1]
    // Section name ".data\0\0\0".
    EXPECT_EQ(bytes[kDataHdr + 0], '.');
    EXPECT_EQ(bytes[kDataHdr + 1], 'd');
    EXPECT_EQ(bytes[kDataHdr + 2], 'a');
    EXPECT_EQ(bytes[kDataHdr + 3], 't');
    EXPECT_EQ(bytes[kDataHdr + 4], 'a');
    // Characteristics @ +36 carry IMAGE_SCN_MEM_WRITE (0x80000000). THE pin:
    // a mutable global's section MUST be writable.
    constexpr std::uint32_t kImageScnMemWrite = 0x80000000u;
    std::uint32_t const chars = readU32LE(bytes, kDataHdr + 36);
    EXPECT_NE(chars & kImageScnMemWrite, 0u)
        << ".data Characteristics must carry IMAGE_SCN_MEM_WRITE so a runtime "
           "store does not fault (D-LK4-DATA-PRODUCER); got 0x"
        << std::hex << chars;
    // The 4 data bytes are present on disk at PointerToRawData.
    std::uint32_t const dataFileOff = readU32LE(bytes, kDataHdr + 20);
    ASSERT_GE(bytes.size(), dataFileOff + 4u);
    EXPECT_EQ(bytes[dataFileOff + 0], 0x07u);
}

// D-LK4-DATA-PRODUCER writer pin: a Bss (zero-init) item lands in a `.bss`
// section that is WRITABLE, NOBITS (SizeOfRawData == 0, no file footprint), and
// carries a non-zero VirtualSize (the zero-fill memory extent). RED if the
// writer reverts to rejecting Bss, or writes file bytes for it.
TEST(PeExecWriter, BssSectionEmittedNobitsWritable) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol       = SymbolId{43};
    d.section      = DataSectionKind::Bss;
    d.reservedSize = 4;                       // int g; → 4 zero-fill bytes
    d.alignment    = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    DiagnosticReporter rep;
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "PE writer must ACCEPT a Bss item now that D-LK4-DATA-PRODUCER closed";
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(readU16LE(bytes, 0x86), 2u) << ".text + .bss";
    constexpr std::size_t kSecHdrTable = 0x188;
    constexpr std::size_t kSecHdrSize  = 40;
    constexpr std::size_t kBssHdr      = kSecHdrTable + kSecHdrSize;  // [1]
    EXPECT_EQ(bytes[kBssHdr + 0], '.');
    EXPECT_EQ(bytes[kBssHdr + 1], 'b');
    EXPECT_EQ(bytes[kBssHdr + 2], 's');
    EXPECT_EQ(bytes[kBssHdr + 3], 's');
    // VirtualSize @ +8 = 4 (the zero-fill extent — nonzero).
    EXPECT_EQ(readU32LE(bytes, kBssHdr + 8), 4u);
    // SizeOfRawData @ +16 = 0; PointerToRawData @ +20 = 0 (NOBITS — no file
    // footprint).
    EXPECT_EQ(readU32LE(bytes, kBssHdr + 16), 0u)
        << ".bss must have SizeOfRawData == 0 (no file bytes)";
    EXPECT_EQ(readU32LE(bytes, kBssHdr + 20), 0u)
        << ".bss must have PointerToRawData == 0 (no file footprint)";
    // Characteristics @ +36 carry MEM_WRITE.
    EXPECT_NE(readU32LE(bytes, kBssHdr + 36) & 0x80000000u, 0u)
        << ".bss Characteristics must carry IMAGE_SCN_MEM_WRITE";
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name": "pe-exec-rodata-no-row", "version": "1.0", "kind": "pe"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"kernel32.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "ExitProcess" },
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
    auto bytes = encodeUntrampolined(mod, *loaded.target, *loaded.format, rep);
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
      "cSymbolDecoration": { "scheme": "none" },
  "dataModel": "LP64",
  "headerNameMatching": "case-sensitive",
      "format": {"name": "pe-exec-cert-shift", "version": "1.0", "kind": "pe"},
      "entryPoint": "",
      "runtimeLibraries": [{"role":"cLibrary","image":"kernel32.dll"}],
      "entryVerbs": ["none","argc-argv"],
      "processExit": { "mechanism": "by-name-import", "role": "cLibrary", "importMangledName": "ExitProcess" },
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
    auto bytes = encodeUntrampolined(mod, **target, **fmt, rep);
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


// ═════════════════════════════════════════════════════════════════
// D-CSUBSET-THREAD-LOCAL (TLS C1, audit LOW-b): the PE walker's
// anti-static-alias belt. The shipped pe64 JSONs do not advertise
// tdata/tbss, so the LINKER's acceptsDataSection gate fires first on
// the real pipeline (K_NoMatchingObjectFormat — pinned in
// test_linker.cpp). THIS pin calls pe::encode DIRECTLY — bypassing
// the linker — so the in-walker 0x8015 belt is what fires: the guard
// a future format JSON opting in BEFORE the TLS C3 walker arm would
// hit instead of silently emitting a process-shared alias.
// ═════════════════════════════════════════════════════════════════
// (PeWriter.TdataItemRejectsLoudUntilTlsC3 removed — TLS C3 LANDED: a pe64
// thread-local item now EMITS the .tls section + IMAGE_TLS_DIRECTORY64 +
// _tls_index. The landed behavior is pinned by the PeExecTls.* suite above.)


// ═════════════════════════════════════════════════════════════════
// D-SQLITE-PE64-FULL-TIER-STACK-DEPTH: the per-PROGRAM stack-reserve
// request (`ImageRequest::stackReserveBytes`), pinned END-TO-END
// through the REAL pipeline — `linker::link` → the capability gate →
// `pe::encode` → IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve.
//
// The failure mode every pin below exists to close is the KNOB THAT
// LIES: a build that reports SUCCESS while the emitted image's stack
// is not the number the build asked for. So each test asserts the
// NON-DEFAULT byte value at its spec offset (a test that still passed
// when the override never reached the header would be worthless), an
// EXACT diagnostic code + count on every refusal, and — for the
// unused-feature path — BYTE-IDENTITY with the pre-feature image.
//
// Layout (already pinned by LoadBearingOptionalHeaderFieldsPinned-
// ByteForByte): the PE32+ optional header starts at file offset 0x98,
// so SizeOfStackReserve (PE/COFF §3.4, optional-header +72) is the u64
// at 0xE0 and SizeOfStackCommit (+80) the u64 at 0xE8.
// ═════════════════════════════════════════════════════════════════

namespace {

// IMAGE_OPTIONAL_HEADER64 file offsets (OH64 base 0x98 + spec offset).
constexpr std::size_t kOhStackReserveOff = 0xE0;  // +72, u64
constexpr std::size_t kOhStackCommitOff  = 0xE8;  // +80, u64
constexpr std::size_t kOhHeapReserveOff  = 0xF0;  // +88, u64
constexpr std::size_t kOhSubsystemOff    = 0xDC;  // +68, u16
constexpr std::size_t kOhDllCharacterOff = 0xDE;  // +70, u16

// What the SHIPPED pe64-x86_64-windows-exec schema declares. Kept as
// named constants so a descriptor edit that moves a bound shows up as a
// deliberate test edit rather than a silently-still-passing assertion.
// (The bounds themselves are pinned field-by-field in
// tests/link/test_object_format_schema.cpp.)
constexpr std::uint64_t kShippedDefaultReserve = 0x100000ull;      // 1 MiB
constexpr std::uint64_t kShippedMinReserve     = 65536ull;         // 64 KiB
constexpr std::uint64_t kShippedMaxReserve     = 4294967296ull;    // 4 GiB
// A NON-DEFAULT, in-range, granularity-aligned request. 4 MiB is the
// value the sqlite full-source testfixture actually needs.
constexpr std::uint64_t kOverrideReserve       = 4194304ull;       // 4 MiB

// One trivial `ret` function linked through the REAL pipeline against
// the SHIPPED exec schema. `linker::link` — never a hand-built header —
// is the whole point: the gate and the walker both sit on this path.
[[nodiscard]] LinkedImage
linkTrivialExec(Loaded const&       loaded,
                DiagnosticReporter& rep,
                ImageRequest const& request) {
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    return linker::link(mod, *loaded.target, *loaded.format, rep, request);
}

} // namespace

TEST(PeExecWriter, StackReserveOverrideReachesHeaderAndSlidesNoOtherByte) {
    // THE knob-that-lies guard. A 4 MiB request must be what the Windows
    // loader reads out of SizeOfStackReserve — not the schema's declared
    // 1 MiB default. RED-on-disable: drop the `request.stackReserveBytes
    // .value_or(...)` in pe.cpp's encodeExec (or the ImageRequest
    // thread-through anywhere along linker::link → pe::encode →
    // encodeExec) and 0xE0 reads 0x100000 while the link still reports
    // success — exactly the silent no-op this capability exists to
    // eliminate.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target);
    ASSERT_TRUE(loaded.format);

    DiagnosticReporter repDef;
    LinkedImage const def = linkTrivialExec(loaded, repDef, ImageRequest{});
    for (auto const& d : repDef.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(repDef.errorCount(), 0u);
    ASSERT_TRUE(def.ok());
    ASSERT_GT(def.bytes.size(), kOhStackReserveOff + 8u);

    DiagnosticReporter repReq;
    LinkedImage const got = linkTrivialExec(
        loaded, repReq, ImageRequest{.stackReserveBytes = kOverrideReserve});
    for (auto const& d : repReq.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(repReq.errorCount(), 0u);
    ASSERT_TRUE(got.ok());
    ASSERT_GT(got.bytes.size(), kOhStackReserveOff + 8u);

    // (1) The requested value — NOT the default — is in the header.
    EXPECT_EQ(readU64LE(got.bytes, kOhStackReserveOff), kOverrideReserve)
        << "the per-PROGRAM request must reach "
           "IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve";
    EXPECT_NE(readU64LE(got.bytes, kOhStackReserveOff), kShippedDefaultReserve)
        << "a header still carrying the schema default means the override "
           "was silently dropped";

    // (2) The override wrote the RIGHT field: the neighbours on either
    //     side of it keep their shipped values (a u64 written one slot
    //     early/late would show up here even if 0xE0 happened to match).
    EXPECT_EQ(readU16LE(got.bytes, kOhSubsystemOff), 3u);        // WINDOWS_CUI
    EXPECT_EQ(readU16LE(got.bytes, kOhDllCharacterOff), 0x8160u);
    EXPECT_EQ(readU64LE(got.bytes, kOhStackCommitOff), 0x1000ull)
        << "SizeOfStackCommit must be untouched by a RESERVE request";
    EXPECT_EQ(readU64LE(got.bytes, kOhHeapReserveOff), 0x100000ull)
        << "SizeOfHeapReserve is a different knob entirely";

    // (3) The strongest layout pin available: the override image and the
    //     default image are the same SIZE, and EVERY byte that differs
    //     between them lies inside the 8-byte SizeOfStackReserve field.
    //     Nothing slid, no second field was quietly co-written, no
    //     size/checksum/alignment recompute leaked out. (The differing
    //     COUNT is not 8 — 0x100000 and 0x400000 happen to share seven of
    //     their eight bytes — so the subset relation, plus the exact u64
    //     equality above, is what states the invariant honestly.)
    ASSERT_EQ(got.bytes.size(), def.bytes.size())
        << "a stack-reserve request must not change the image SIZE";
    std::vector<std::size_t> differing;
    for (std::size_t i = 0; i < got.bytes.size(); ++i) {
        if (got.bytes[i] != def.bytes[i]) differing.push_back(i);
    }
    EXPECT_FALSE(differing.empty())
        << "the override produced a byte-identical image -- it never "
           "reached the header at all";
    for (std::size_t const off : differing) {
        EXPECT_TRUE(off >= kOhStackReserveOff
                    && off < kOhStackReserveOff + 8u)
            << "byte at file offset 0x" << std::hex << off << std::dec
            << " changed, but only SizeOfStackReserve [0xE0,0xE8) may";
    }
}

TEST(PeExecWriter, AbsentStackReserveRequestIsByteIdenticalToTheDefaultPath) {
    // The feature must be provably INERT when unused: every pre-existing
    // build is byte-for-byte what it was before the knob existed. Three
    // call shapes are compared — the pre-feature 4-argument call, an
    // explicitly EMPTY ImageRequest, and an EXPLICIT request that happens
    // to equal the schema default (which proves the override path writes
    // that field and nothing else).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target);
    ASSERT_TRUE(loaded.format);

    // The pre-feature call shape: NO request argument at all.
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter repNoArg;
    LinkedImage const noArg =
        linker::link(mod, *loaded.target, *loaded.format, repNoArg);
    for (auto const& d : repNoArg.all()) ADD_FAILURE() << d.actual;
    ASSERT_EQ(repNoArg.errorCount(), 0u);
    ASSERT_TRUE(noArg.ok());
    ASSERT_GT(noArg.bytes.size(), kOhStackReserveOff + 8u);
    EXPECT_EQ(readU64LE(noArg.bytes, kOhStackReserveOff),
              kShippedDefaultReserve)
        << "an unrequested reserve must stay the schema's declared 1 MiB";

    DiagnosticReporter repEmpty;
    LinkedImage const empty = linkTrivialExec(loaded, repEmpty, ImageRequest{});
    ASSERT_EQ(repEmpty.errorCount(), 0u);
    EXPECT_EQ(empty.bytes, noArg.bytes)
        << "an EMPTY ImageRequest must produce a byte-identical image";

    // An explicit request equal to the declared default is legal (in
    // range, 4096-aligned) and must land on the same bytes.
    DiagnosticReporter repSame;
    LinkedImage const same = linkTrivialExec(
        loaded, repSame,
        ImageRequest{.stackReserveBytes = kShippedDefaultReserve});
    ASSERT_EQ(repSame.errorCount(), 0u);
    EXPECT_EQ(same.bytes, noArg.bytes)
        << "requesting exactly the default must write exactly the default "
           "-- the override touches SizeOfStackReserve and nothing else";
}

TEST(PeExecWriter, OutOfRangeOrMisalignedStackReserveRequestFailsLoud) {
    // The request is REFUSED, never clamped and never rounded: an image
    // whose stack silently differs from the number the build asked for is
    // undiagnosable at runtime. Each row must produce a FAILED link with
    // EXACTLY ONE K_InvalidStackReserveRequest — asserting the code, not
    // merely that "an error happened" (a K_SymbolUndefined would satisfy
    // the weaker form while the range check was gone).
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target);
    ASSERT_TRUE(loaded.format);

    struct Row { std::uint64_t want; char const* why; };
    for (Row const row : {
             // Below minimumBytes (65536). Page-aligned + under the cap,
             // so ONLY the minimum check can reject it.
             Row{4096ull,
                 "4096 is below the declared minimumBytes (65536)"},
             // Above maximumBytes (4 GiB). Page-aligned + over the
             // minimum, so ONLY the maximum check can reject it.
             Row{8589934592ull,
                 "8 GiB is above the declared maximumBytes (4 GiB)"},
             // In range on BOTH bounds but off the 4096-byte
             // granularity by one byte — the silent-round trap.
             Row{4194305ull,
                 "4 MiB + 1 is in range but off the 4096-byte granularity"}}) {
        AssembledModule mod = makeTrivialModule({0xC3}, 1);
        DiagnosticReporter rep;
        LinkedImage const img =
            linker::link(mod, *loaded.target, *loaded.format, rep,
                         ImageRequest{.stackReserveBytes = row.want});
        EXPECT_FALSE(img.ok()) << row.why;
        EXPECT_TRUE(img.bytes.empty())
            << row.why << " -- a refused request must emit NO image";
        EXPECT_EQ(::dss::test_support::countCode(
                      rep, DiagnosticCode::K_InvalidStackReserveRequest),
                  1u)
            << row.why;
        EXPECT_EQ(rep.errorCount(), 1u)
            << row.why << " -- the gate runs FIRST and returns, so the "
                          "refusal is the only diagnostic";
    }
}

TEST(PeExecWriter, DllAndObjSchemasRefuseAStackReserveRequest) {
    // CAPABILITY, not KIND, is what gates. All three shipped pe64
    // schemas are `kind: pe` and the dll even carries the very same
    // IMAGE_OPTIONAL_HEADER64.SizeOfStackReserve field — but only the
    // EXEC schema declares `stackReserveControl`, because the Windows
    // loader ignores a DLL's copy (the .exe's value governs the main
    // thread) and a relocatable .obj has no optional header at all.
    // An `if (kind == pe)` engine branch would happily write a field
    // nothing reads; these pins are what forbid one.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto dll = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-dll");
    ASSERT_TRUE(dll.has_value());
    auto obj = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(obj.has_value());

    // The asymmetry, stated: same kind, opposite answers.
    auto exec = loadShippedExec();
    ASSERT_TRUE(exec.format);
    EXPECT_EQ(exec.format->kind(), ObjectFormatKind::Pe);
    EXPECT_TRUE(exec.format->stackReserveControl().has_value());

    for (ObjectFormatSchema const* fmt : {&**dll, &**obj}) {
        EXPECT_EQ(fmt->kind(), ObjectFormatKind::Pe) << fmt->name();
        ASSERT_FALSE(fmt->stackReserveControl().has_value())
            << fmt->name() << " must declare NO stackReserveControl";

        // (a) The linker gate — the single chokepoint every image
        //     emission passes through.
        AssembledModule mod = makeTrivialModule({0xC3}, 1);
        DiagnosticReporter rep;
        LinkedImage const img =
            linker::link(mod, **target, *fmt, rep,
                         ImageRequest{.stackReserveBytes = kOverrideReserve});
        EXPECT_FALSE(img.ok()) << fmt->name();
        EXPECT_TRUE(img.bytes.empty()) << fmt->name();
        EXPECT_EQ(::dss::test_support::countCode(
                      rep, DiagnosticCode::K_FormatLacksStackReserveControl),
                  1u)
            << fmt->name() << " must refuse the request, not drop it";
        EXPECT_EQ(rep.errorCount(), 1u) << fmt->name();

        // (b) The WALKER's own re-check. `pe::encode` is a PUBLIC entry
        //     point, so a caller that bypasses linker::link must still be
        //     refused — the same walker serves exec/dll/obj and only the
        //     declared capability tells them apart.
        DiagnosticReporter repW;
        auto const bytes =
            pe::encode(mod, **target, *fmt, repW,
                       ImageRequest{.stackReserveBytes = kOverrideReserve});
        EXPECT_TRUE(bytes.empty())
            << fmt->name() << " -- pe::encode must refuse a request it "
                              "cannot honour, even off the linker path";
        EXPECT_EQ(::dss::test_support::countCode(
                      repW, DiagnosticCode::K_FormatLacksStackReserveControl),
                  1u)
            << fmt->name();
    }
}

TEST(PeExecWriter, StackReserveBoundsAreInclusiveAtBothEnds) {
    // minimumBytes and maximumBytes are INCLUSIVE: the gate compares
    // `want < min` / `want > max`, never `<=` / `>=`. RED-on-disable:
    // flip either comparison to its non-strict form and the matching
    // boundary request is refused, failing here. Both values also land
    // in the header, so an "accepted but dropped" regression is caught
    // by the same test.
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.target);
    ASSERT_TRUE(loaded.format);

    for (std::uint64_t const want : {kShippedMinReserve, kShippedMaxReserve}) {
        AssembledModule mod = makeTrivialModule({0xC3}, 1);
        DiagnosticReporter rep;
        LinkedImage const img =
            linker::link(mod, *loaded.target, *loaded.format, rep,
                         ImageRequest{.stackReserveBytes = want});
        for (auto const& d : rep.all()) ADD_FAILURE() << d.actual << " (want=" << want << ")";
        EXPECT_EQ(rep.errorCount(), 0u)
            << want << " sits exactly ON a declared bound and must be "
                       "ACCEPTED";
        ASSERT_TRUE(img.ok()) << want;
        ASSERT_GT(img.bytes.size(), kOhStackReserveOff + 8u) << want;
        EXPECT_EQ(readU64LE(img.bytes, kOhStackReserveOff), want)
            << "the boundary value must reach the header verbatim";
        EXPECT_NE(readU64LE(img.bytes, kOhStackReserveOff),
                  kShippedDefaultReserve)
            << want << " must not silently fall back to the default";
    }
}
