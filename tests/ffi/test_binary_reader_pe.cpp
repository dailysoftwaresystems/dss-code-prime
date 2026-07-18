// Plan 11 FF1-PE tests — `dss::ffi::readImportsFromBytes` on PE binaries.
//
// Pins:
//   * PE32+ export-directory round-trip from a synthesized PE binary.
//   * No-export DLL returns empty surface (success, not error).
//   * Failure modes: truncated DOS header, bad PE signature, malformed
//     optional header magic, export RVA outside any section, names
//     table runs past EOF.
//   * c159 HARDENING (D-FF1-PE-READER; closes D-FF1-PE-OBJECT-EXPORTS):
//     the reader walks the Export Address Table + name-ordinal table and
//     CLASSIFIES each export by its EAT-RVA section membership —
//     Function (executable section), Object (non-executable data
//     section = a PE data export), or Forwarder (EAT RVA inside the
//     export-directory span, pointing at a "DLL.Symbol" forward string).
//     Visibility/linkage stay Default/External (the PE export table
//     carries no STT_/STV_/STB_ granularity). New bounds/truncation pins
//     for the EAT + ordinal tables + the forwarder-string read.
//
// Test strategy: synthesize minimal PE32+ binaries directly in C++ via
// the byte-emit helpers + read them back. Avoids shipping real DLLs
// in tests (CI hermeticity). The fixture emits `.text` (executable),
// `.data` (non-executable), and `.edata` sections so a per-export EAT
// RVA lands in the section matching its intended kind.

#include "core/types/diagnostic_reporter.hpp"
#include "ffi/binary_reader.hpp"
#include "byte_emit.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::appU16;
using dss::test_support::appU32;
using dss::test_support::appU64;
using dss::test_support::putU32;

namespace {

// One export the fixture emits into a synthesized PE. `kind` decides
// which section the export's EAT RVA lands in (so the reader classifies
// it): Function → `.text`, Data → `.data`, Forwarder → the
// export-directory span (the RVA points at `forwardTarget`).
enum class ExpKind { Function, Data, Forwarder };
struct ExportSpec {
    std::string name;
    ExpKind     kind = ExpKind::Function;
    std::string forwardTarget;   // used only when kind == Forwarder
};

// The built image plus the RVAs (== file offsets, see below) tests poke.
struct BuiltPe {
    std::vector<std::uint8_t> bytes;
    std::uint32_t exportDirRva  = 0;   // = data-directory[0] RVA (also file off)
    std::uint32_t exportDirSize = 0;
    std::uint32_t eatRva        = 0;   // AddressOfFunctions
    std::uint32_t namesTableRva = 0;   // AddressOfNames
    std::uint32_t ordTableRva   = 0;   // AddressOfNameOrdinals
    std::uint32_t textRva       = 0;
    std::uint32_t dataRva       = 0;
    std::uint32_t edataRva      = 0;
};

// Build a minimal PE32+ binary with THREE section headers:
//   [.text ] executable, virtual-only (SizeOfRawData = 0)  — function RVAs
//   [.data ] non-executable data, virtual-only             — data RVAs
//   [.edata] file-backed: the export tables + name/forwarder strings
//
// The `.edata` section keeps RVA == file offset (its VirtualAddress ==
// PointerToRawData), so `namesTableRva` / `eatRva` / `ordTableRva` are
// direct file offsets the corruption pins poke. `.text` / `.data` are
// virtual-only at high RVAs (0x100000 / 0x200000) that never overlap the
// export-directory span — so a function/data EAT RVA is never misread as
// a forwarder. Layout inside `.edata`:
//   [0]            IMAGE_EXPORT_DIRECTORY (40 B)
//   [40]           Export Address Table   — u32 RVA × N
//   [40+4N]        Name Pointer Table      — u32 RVA × N (INPUT order)
//   [40+8N]        Ordinal Table           — u16 × N (identity: ord[i]=i)
//   [40+10N]       name strings, then forwarder-target strings, then (when
//                  requested) the DllName string (packed)
//
// D-FF1-READER-SONAME (c171): a non-empty `dllName` is packed into `.edata`
// and its RVA is written to the export directory's Name field (offset +12).
// Empty (the default) leaves NameRva == 0, so the reader reports an empty
// soname (the pre-c171 behavior every existing PE test relies on).
[[nodiscard]] BuiltPe buildPeExports(std::vector<ExportSpec> const& exports,
                                     std::string const& dllName = {}) {
    constexpr std::uint32_t kPeOffset      = 128;
    constexpr std::uint32_t kNumSections   = 3;
    constexpr std::uint32_t kSectionHdrOff = kPeOffset + 4 + 20 + 240;      // 392
    constexpr std::uint32_t kEdataRawOff   = kSectionHdrOff + kNumSections * 40;  // 512
    constexpr std::uint16_t kPe32PlusMagic = 0x020B;
    // Section Characteristics (PE/COFF §4.1). The reader classifies on
    // IMAGE_SCN_MEM_EXECUTE (0x20000000) alone.
    constexpr std::uint32_t kTextChars  = 0x60000020u;  // CNT_CODE|MEM_EXECUTE|MEM_READ
    constexpr std::uint32_t kDataChars  = 0xC0000040u;  // CNT_INIT_DATA|MEM_READ|MEM_WRITE
    constexpr std::uint32_t kEdataChars = 0x40000040u;  // CNT_INIT_DATA|MEM_READ
    constexpr std::uint32_t kExportDirSize = 40;

    std::uint32_t const n       = static_cast<std::uint32_t>(exports.size());
    std::uint32_t const edataRva = kEdataRawOff;   // RVA == file offset
    std::uint32_t const textRva  = 0x100000u;
    std::uint32_t const dataRva  = 0x200000u;
    std::uint32_t const secVSize = n + 16u;        // headroom over the export count

    // ── .edata payload offsets (relative to the section start) ──
    std::uint32_t const eatOff   = kExportDirSize;
    std::uint32_t const namesOff = eatOff   + 4u * n;
    std::uint32_t const ordOff   = namesOff + 4u * n;
    std::uint32_t const strOff   = ordOff   + 2u * n;

    // Pack name strings, then (for forwarders only) forward-target
    // strings; remember each string's section-relative offset.
    std::vector<std::uint32_t> nameStrRel(n, 0);
    std::vector<std::uint32_t> fwdStrRel(n, 0);
    std::vector<std::uint8_t>  strBlob;
    for (std::uint32_t i = 0; i < n; ++i) {
        nameStrRel[i] = strOff + static_cast<std::uint32_t>(strBlob.size());
        for (char c : exports[i].name)
            strBlob.push_back(static_cast<std::uint8_t>(c));
        strBlob.push_back(0);
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (exports[i].kind != ExpKind::Forwarder) continue;
        fwdStrRel[i] = strOff + static_cast<std::uint32_t>(strBlob.size());
        for (char c : exports[i].forwardTarget)
            strBlob.push_back(static_cast<std::uint8_t>(c));
        strBlob.push_back(0);
    }
    // The DLL's own name string (packed last); its section-relative offset
    // feeds the export directory Name field (edata +12). Empty -> NameRva 0.
    std::uint32_t dllNameRel = 0;
    if (!dllName.empty()) {
        dllNameRel = strOff + static_cast<std::uint32_t>(strBlob.size());
        for (char c : dllName)
            strBlob.push_back(static_cast<std::uint8_t>(c));
        strBlob.push_back(0);
    }
    std::uint32_t const edataSize =
        strOff + static_cast<std::uint32_t>(strBlob.size());

    // ── Header prefix ──
    std::vector<std::uint8_t> b(kEdataRawOff, 0);
    b[0] = 'M'; b[1] = 'Z';
    putU32(b, 0x3C, kPeOffset);
    b[kPeOffset + 0] = 'P'; b[kPeOffset + 1] = 'E';
    b[kPeOffset + 2] = 0;   b[kPeOffset + 3] = 0;
    std::size_t const coffOff = kPeOffset + 4;
    b[coffOff + 0] = 0x64; b[coffOff + 1] = 0x86;                 // Machine AMD64
    b[coffOff + 2] = static_cast<std::uint8_t>(kNumSections & 0xFF);
    b[coffOff + 3] = static_cast<std::uint8_t>(kNumSections >> 8); // NumberOfSections
    b[coffOff + 16] = 0xF0; b[coffOff + 17] = 0x00;               // SizeOfOptionalHeader = 240
    std::size_t const optHeaderOff = coffOff + 20;
    b[optHeaderOff + 0] = static_cast<std::uint8_t>(kPe32PlusMagic & 0xFF);
    b[optHeaderOff + 1] = static_cast<std::uint8_t>(kPe32PlusMagic >> 8);
    std::size_t const dataDirsOff = optHeaderOff + 112;
    putU32(b, dataDirsOff + 0, edataRva);    // DataDirectories[0].RVA
    putU32(b, dataDirsOff + 4, edataSize);   // DataDirectories[0].Size (covers forwarder strings)

    // ── Section headers ──
    auto writeSectionHdr = [&](std::uint32_t idx, char const* name,
                               std::uint32_t vsize, std::uint32_t vaddr,
                               std::uint32_t rawSize, std::uint32_t rawPtr,
                               std::uint32_t chars) {
        std::size_t const h = kSectionHdrOff + idx * 40u;
        for (std::size_t k = 0; k < 8 && name[k] != '\0'; ++k)
            b[h + k] = static_cast<std::uint8_t>(name[k]);
        putU32(b, h +  8, vsize);
        putU32(b, h + 12, vaddr);
        putU32(b, h + 16, rawSize);
        putU32(b, h + 20, rawPtr);
        putU32(b, h + 36, chars);   // Characteristics
    };
    writeSectionHdr(0, ".text",  secVSize,  textRva,  0,         0,           kTextChars);
    writeSectionHdr(1, ".data",  secVSize,  dataRva,  0,         0,           kDataChars);
    writeSectionHdr(2, ".edata", edataSize, edataRva, edataSize, kEdataRawOff, kEdataChars);

    // ── .edata content ──
    std::vector<std::uint8_t> ed(edataSize, 0);
    auto edU32 = [&](std::size_t off, std::uint32_t v) {
        for (int k = 0; k < 4; ++k)
            ed[off + k] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFF);
    };
    auto edU16 = [&](std::size_t off, std::uint16_t v) {
        ed[off + 0] = static_cast<std::uint8_t>(v & 0xFF);
        ed[off + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    };
    if (!dllName.empty())
        edU32(12, edataRva + dllNameRel);  // NameRva (export-directory DllName)
    edU32(16, 1u);                    // OrdinalBase
    edU32(20, n);                     // AddressTableEntries
    edU32(24, n);                     // NumberOfNamePointers
    edU32(28, edataRva + eatOff);     // AddressOfFunctions
    edU32(32, edataRva + namesOff);   // AddressOfNames
    edU32(36, edataRva + ordOff);     // AddressOfNameOrdinals
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t eat = 0;
        switch (exports[i].kind) {
            case ExpKind::Function:  eat = textRva + i;             break;
            case ExpKind::Data:      eat = dataRva + i;             break;
            case ExpKind::Forwarder: eat = edataRva + fwdStrRel[i]; break;
        }
        edU32(eatOff   + 4u * i, eat);
        edU32(namesOff + 4u * i, edataRva + nameStrRel[i]);
        edU16(ordOff   + 2u * i, static_cast<std::uint16_t>(i));   // identity ordinal
    }
    for (std::size_t k = 0; k < strBlob.size(); ++k)
        ed[strOff + k] = strBlob[k];

    b.insert(b.end(), ed.begin(), ed.end());

    BuiltPe out;
    out.bytes         = std::move(b);
    out.exportDirRva  = edataRva;
    out.exportDirSize = edataSize;
    out.eatRva        = edataRva + eatOff;
    out.namesTableRva = edataRva + namesOff;
    out.ordTableRva   = edataRva + ordOff;
    out.textRva       = textRva;
    out.dataRva       = dataRva;
    out.edataRva      = edataRva;
    return out;
}

// Back-compat shim: all-Function exports, returns just the image bytes.
// Keeps the v1 tests (which assert every export is Function/Default/
// External) reading exactly as before.
[[nodiscard]] std::vector<std::uint8_t>
buildMinimalPe32Plus(std::vector<std::string> const& exportNames) {
    std::vector<ExportSpec> specs;
    specs.reserve(exportNames.size());
    for (auto const& nm : exportNames)
        specs.push_back(ExportSpec{nm, ExpKind::Function, {}});
    return buildPeExports(specs).bytes;
}

} // namespace

// ── Happy path ───────────────────────────────────────────────────

TEST(BinaryReaderPe, ReadsPe32PlusExportTableRoundTrip) {
    auto const bytes = buildMinimalPe32Plus({"printf", "malloc", "free"});
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "msvcrt.dll", rep);
    ASSERT_TRUE(r.has_value())
        << "PE reader rejected synthetic PE: "
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 3u);
    EXPECT_EQ((*r)[0].mangledName, "printf");
    EXPECT_EQ((*r)[0].libraryPath, "msvcrt.dll");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    EXPECT_EQ((*r)[0].visibility, SymbolVisibility::Default);
    EXPECT_EQ((*r)[0].linkage, SymbolLinkage::External);
    EXPECT_EQ((*r)[1].mangledName, "malloc");
    EXPECT_EQ((*r)[2].mangledName, "free");
}

// ── D-FF1-READER-SONAME (c171): export-directory DllName extraction ──

// STRICT: the export directory's Name field (DllName) surfaces on EVERY
// row's `soname`, verbatim ("widget.dll").
TEST(BinaryReaderPe, ExtractsDllNameFromExportDirectory) {
    auto built = buildPeExports({{"printf", ExpKind::Function, {}},
                                 {"malloc", ExpKind::Function, {}}},
                                /*dllName=*/"widget.dll");
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "C:/build/out/widget-9a3f.dll", rep);
    ASSERT_TRUE(r.has_value())
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 2u);
    for (auto const& row : *r) {
        EXPECT_EQ(row.soname, "widget.dll");
    }
    // The path label stays on libraryPath — soname is the SEPARATE
    // embedded DllName, NOT the on-disk basename.
    EXPECT_EQ((*r)[0].libraryPath, "C:/build/out/widget-9a3f.dll");
    EXPECT_EQ(rep.errorCount(), 0u);
}

// RED-ON-DISABLE: the EXISTING synthesizer never sets the Name field, so
// NameRva stays 0 and every row's soname MUST be empty. Fails if the
// extractor ever fabricated a DllName.
TEST(BinaryReaderPe, ZeroNameRvaLeavesSonameEmpty) {
    auto const bytes = buildMinimalPe32Plus({"printf", "malloc"});  // NameRva == 0
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "widget.dll", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    for (auto const& row : *r) {
        EXPECT_TRUE(row.soname.empty())
            << "NameRva==0 must leave soname empty; got '" << row.soname << "'";
    }
}

TEST(BinaryReaderPe, NoNamedExportsReturnsEmptySurface) {
    // A DLL with no named exports is valid — `.edata` may carry
    // ordinal-only exports. v1 only reads names, so empty is the
    // honest answer (not an error).
    auto const bytes = buildMinimalPe32Plus({});
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "empty.dll", rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 0u);
}

// ── Failure modes ────────────────────────────────────────────────

TEST(BinaryReaderPe, ShortBufferRejected) {
    std::vector<std::uint8_t> bytes{'M', 'Z'};  // 2 bytes
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
}

TEST(BinaryReaderPe, BadPeSignatureRejected) {
    std::vector<std::uint8_t> bytes(132, 0);
    bytes[0] = 'M'; bytes[1] = 'Z';
    putU32(bytes, 0x3C, 128);
    // Leave PE signature as four zero bytes — not "PE\0\0".
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "bad-sig.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("PE signature missing"),
              std::string::npos);
}

TEST(BinaryReaderPe, BadOptionalHeaderMagicRejected) {
    auto bytes = buildMinimalPe32Plus({"x"});
    // Overwrite the optional header magic.
    std::size_t const optHeaderOff = 128 + 4 + 20;
    bytes[optHeaderOff + 0] = 0xAB;
    bytes[optHeaderOff + 1] = 0xCD;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "bad-opt.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("optional header magic"),
              std::string::npos);
    // Hex format regression pin (post-fold #15): the message must
    // render optMagic as 4-digit hex (`0xCDAB` for the
    // little-endian-loaded value here), NOT decimal. The pre-fold
    // bug used `"0x" + std::to_string(u16)` which produced
    // `"0x52651"` for optMagic=0xCDAB — actively misleading.
    EXPECT_NE(r.error().detail.find("0xCDAB"), std::string::npos)
        << "optMagic must render as 4-digit hex; regression on the "
           "snprintf/std::format hex fix";
    EXPECT_EQ(r.error().detail.find("0x52651"), std::string::npos)
        << "optMagic must NOT render as decimal (was the bug)";
}

TEST(BinaryReaderPe, ExportRvaOutsideAnySectionRejected) {
    auto bytes = buildMinimalPe32Plus({"x"});
    // DataDirectories[0] RVA at optHeaderOff + 112; point it past
    // section coverage.
    std::size_t const optHeaderOff = 128 + 4 + 20;
    std::size_t const dataDirsOff  = optHeaderOff + 112;
    putU32(bytes, dataDirsOff + 0, 0xDEADBEEF);  // RVA into nowhere
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "bad-rva.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::SectionNotFound);
}

// D-FF1-PARTIAL-CORRUPTION-LOUD PE pin (2026-06-01): build a PE with
// one valid export + 2 entries whose name pointers are deliberately
// poisoned past EOF. The PE reader skips the corrupted entries +
// emits F_BinaryReaderPartialCorruption Warning summarizing the loss.
// Regression-blocker for the silent-skip surface this commit closes.
TEST(BinaryReaderPe, PartialCorruptionWarningFiresOnPoisonedNameRvas) {
    // Build with 3 names but corrupt 2 of the 3 name-RVAs to point
    // past EOF (which fails rvaToFileOff → ++corruptedNameSkips).
    auto built = buildPeExports({{"good", ExpKind::Function, {}},
                                 {"bad1", ExpKind::Function, {}},
                                 {"bad2", ExpKind::Function, {}}});
    auto& bytes = built.bytes;

    // Names table: 3 × u32 RVAs (namesTableRva == file offset). Poison
    // entries [1] and [2] to point past EOF (RVA 0xFFFFFFF0 — way past
    // any section).
    putU32(bytes, built.namesTableRva + 1 * 4, 0xFFFFFFF0u);
    putU32(bytes, built.namesTableRva + 2 * 4, 0xFFFFFFF1u);

    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "partial.dll", rep);
    ASSERT_TRUE(r.has_value())
        << "partial corruption must NOT abort — valid exports still surface";
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "good");
    bool sawPartial = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            sawPartial = true;
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
            EXPECT_NE(d.actual.find("skipped 2"), std::string::npos)
                << "counter must reflect 2 skips; actual: " << d.actual;
            break;
        }
    }
    EXPECT_TRUE(sawPartial)
        << "F_BinaryReaderPartialCorruption Warning must fire when "
           "name-RVAs resolve OOB — closes the silent-skip surface "
           "D-FF1-PARTIAL-CORRUPTION-LOUD pins";
}

TEST(BinaryReaderPe, ZeroExportTableRvaReturnsEmptySurface) {
    auto bytes = buildMinimalPe32Plus({"x"});
    // DataDirectories[0] RVA = 0 + size = 0 → no exports.
    std::size_t const optHeaderOff = 128 + 4 + 20;
    std::size_t const dataDirsOff  = optHeaderOff + 112;
    putU32(bytes, dataDirsOff + 0, 0u);
    putU32(bytes, dataDirsOff + 4, 0u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "no-exports.dll", rep);
    ASSERT_TRUE(r.has_value())
        << "Zero exports is valid — not an error";
    EXPECT_EQ(r->size(), 0u);
}

// ── (8) c159 kind classification (closes D-FF1-PE-OBJECT-EXPORTS) ──

// THE red-on-disable-per-kind pin: one Function (EAT RVA in `.text`),
// one Data (EAT RVA in `.data`), one Forwarder (EAT RVA inside the
// export-directory span → a "DLL.Symbol" string). Reverting the EAT
// walk (default every export to Function, the v1 behavior) fails the
// Data + Forwarder assertions; dropping the forwarder branch surfaces
// HeapAlloc as a bogus Function with an empty target.
TEST(BinaryReaderPe, ClassifiesFunctionDataAndForwarder) {
    auto built = buildPeExports({
        {"dss_add",    ExpKind::Function,  {}},
        {"dss_global", ExpKind::Data,      {}},
        {"HeapAlloc",  ExpKind::Forwarder, "NTDLL.RtlAllocateHeap"},
    });
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "mixed.dll", rep);
    ASSERT_TRUE(r.has_value())
        << "reader rejected the mixed-kind PE: "
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 3u);

    EXPECT_EQ((*r)[0].mangledName, "dss_add");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    EXPECT_TRUE((*r)[0].forwardTarget.empty());

    // A PE data export classifies as Object (the shared data kind — the
    // ELF STT_OBJECT / Mach-O __data precedent; there is no `Data`
    // enumerator).
    EXPECT_EQ((*r)[1].mangledName, "dss_global");
    EXPECT_EQ((*r)[1].kind, SymbolKind::Object);
    EXPECT_TRUE((*r)[1].forwardTarget.empty());

    EXPECT_EQ((*r)[2].mangledName, "HeapAlloc");
    EXPECT_EQ((*r)[2].kind, SymbolKind::Forwarder);
    EXPECT_EQ((*r)[2].forwardTarget, "NTDLL.RtlAllocateHeap");

    // Every row keeps Default/External (PE carries no STV_/STB_ column).
    for (auto const& row : *r) {
        EXPECT_EQ(row.visibility, SymbolVisibility::Default);
        EXPECT_EQ(row.linkage, SymbolLinkage::External);
    }
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── (9) c159 EAT / ordinal / forwarder-string fail-loud pins ──

TEST(BinaryReaderPe, EatRvaUnresolvableRejected) {
    // Poison AddressOfFunctions (export dir + 28) to an RVA in no
    // section — the EAT can't be located → fail loud (not silent).
    auto built = buildPeExports({{"f", ExpKind::Function, {}}});
    putU32(built.bytes, built.exportDirRva + 28, 0xDEADBEEFu);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "bad-eat.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::SectionNotFound);
    EXPECT_NE(r.error().detail.find("AddressOfFunctions"),
              std::string::npos);
}

TEST(BinaryReaderPe, EatCountRunsPastEofRejected) {
    // Poison AddressTableEntries (export dir + 20) to a huge count so
    // the EAT region overruns the buffer — the truncation-shaped read
    // must fail loud (W4 mid-EAT analog).
    auto built = buildPeExports({{"f", ExpKind::Function, {}}});
    putU32(built.bytes, built.exportDirRva + 20, 0x10000u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "eat-oob.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("Export Address Table"),
              std::string::npos);
}

TEST(BinaryReaderPe, OrdinalTableRvaUnresolvableRejected) {
    // Poison AddressOfNameOrdinals (export dir + 36) to an RVA in no
    // section — the ordinal table can't be located → fail loud.
    auto built = buildPeExports({{"f", ExpKind::Function, {}}});
    putU32(built.bytes, built.exportDirRva + 36, 0xDEADBEEFu);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "bad-ord.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::SectionNotFound);
    EXPECT_NE(r.error().detail.find("AddressOfNameOrdinals"),
              std::string::npos);
}

TEST(BinaryReaderPe, ForwarderStringUnterminatedRejected) {
    // Cut the buffer's final byte — the forwarder string's terminating
    // NUL (forwarder strings are packed last) — so it runs off EOF
    // unterminated. The bounded forwarder read must fail loud (W4
    // mid-forwarder-string analog), NOT return a truncated target.
    auto built = buildPeExports({{"Fwd", ExpKind::Forwarder, "OTHER.Symbol"}});
    built.bytes.resize(built.bytes.size() - 1);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "cut-fwd.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("forwarder string"), std::string::npos);
    EXPECT_NE(r.error().detail.find("unterminated"), std::string::npos);
}

TEST(BinaryReaderPe, OutOfRangeOrdinalSkippedAsPartialCorruption) {
    // An ordinal entry >= AddressTableEntries is an internal
    // inconsistency: skip that entry + warn (the partial-corruption
    // discipline), surfacing the rest. Poison name[1]'s ordinal to 99.
    auto built = buildPeExports({{"keep", ExpKind::Function, {}},
                                 {"drop", ExpKind::Function, {}}});
    // Ordinal table entry 1 (u16 LE at ordTableRva + 2) → 99 (>= 2).
    built.bytes[built.ordTableRva + 2] = 99;
    built.bytes[built.ordTableRva + 3] = 0;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "ord-oob.dll", rep);
    ASSERT_TRUE(r.has_value())
        << "an out-of-range ordinal must skip that entry, not abort";
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "keep");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    bool sawPartial = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            sawPartial = true;
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
            EXPECT_NE(d.actual.find("skipped 1"), std::string::npos)
                << "actual: " << d.actual;
            break;
        }
    }
    EXPECT_TRUE(sawPartial)
        << "an out-of-range ordinal must fire the partial-corruption Warning";
}
