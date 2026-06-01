// Plan 11 FF1-PE tests — `dss::ffi::readImportsFromBytes` on PE binaries.
//
// Pins:
//   * PE32+ export-directory round-trip from a synthesized PE binary.
//   * No-export DLL returns empty surface (success, not error).
//   * Failure modes: truncated DOS header, bad PE signature, malformed
//     optional header magic, export RVA outside any section, names
//     table runs past EOF.
//   * Symbol kind/visibility/linkage mapping: every PE export becomes
//     Function/Default/External (PE has no STT_/STV_/STB_ granularity
//     equivalent in the export directory; refinements anchored at
//     D-FF1-PE-OBJECT-EXPORTS).
//
// Test strategy: synthesize minimal PE32+ binaries directly in C++ via
// the byte-emit helpers + read them back. Avoids shipping real DLLs
// in tests (CI hermeticity).

#include "core/types/diagnostic_reporter.hpp"
#include "ffi/binary_reader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;

namespace {

inline void appU16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
inline void appU32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
inline void appU64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

// Write u32 at fixed offset (back-patch).
inline void putU32(std::vector<std::uint8_t>& b, std::size_t off,
                   std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        b[off + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
}

// Build a minimal PE32+ binary with one section (`.edata`) carrying
// the export directory + names. Returns the byte image.
//
// Layout in execution order:
//   [0..63]    DOS header (MZ + zero padding + peOffset at 0x3C)
//   [64..127]  Padding so the PE signature starts at offset 128
//   [128..131] PE signature 'P','E',0,0
//   [132..151] COFF header (20 bytes)
//   [152..391] Optional header — PE32+ requires 240 bytes
//   [392..431] Section header for .edata (40 bytes)
//   [432..]    .edata raw data: export directory + name pointers +
//              name strings
//
// All RVAs equal file offsets for simplicity (section's
// VirtualAddress == PointerToRawData).
std::vector<std::uint8_t>
buildMinimalPe32Plus(std::vector<std::string> const& exportNames) {
    constexpr std::uint32_t kPeOffset           = 128;
    constexpr std::uint32_t kSectionHdrOff      = kPeOffset + 4 + 20 + 240;
    constexpr std::uint32_t kSectionRawDataOff  = kSectionHdrOff + 40;
    constexpr std::uint16_t kPe32PlusMagic      = 0x020B;

    std::vector<std::uint8_t> b(kSectionRawDataOff, 0);

    // ── DOS header ──
    b[0] = 'M'; b[1] = 'Z';
    putU32(b, 0x3C, kPeOffset);

    // ── PE signature ──
    b[kPeOffset + 0] = 'P';
    b[kPeOffset + 1] = 'E';
    b[kPeOffset + 2] = 0;
    b[kPeOffset + 3] = 0;

    // ── COFF header ──
    std::size_t const coffOff = kPeOffset + 4;
    // Machine = 0x8664 (AMD64)
    b[coffOff + 0] = 0x64; b[coffOff + 1] = 0x86;
    // NumberOfSections = 1
    b[coffOff + 2] = 0x01; b[coffOff + 3] = 0x00;
    // SizeOfOptionalHeader = 240 (PE32+)
    b[coffOff + 16] = 0xF0; b[coffOff + 17] = 0x00;
    // (Other COFF fields stay zero for our purposes.)

    // ── Optional header (PE32+) ──
    std::size_t const optHeaderOff = coffOff + 20;
    // Magic = PE32+ (0x020B)
    b[optHeaderOff + 0] = static_cast<std::uint8_t>(kPe32PlusMagic & 0xFF);
    b[optHeaderOff + 1] = static_cast<std::uint8_t>(kPe32PlusMagic >> 8);
    // DataDirectories[0] = (exportRva, exportSize) at optHeaderOff + 112
    std::size_t const dataDirsOff = optHeaderOff + 112;

    // ── Build .edata section content first (so we know the sizes) ──
    //
    // Export directory header (40 bytes) at section start:
    //   [ 0.. 3] ExportFlags
    //   [ 4.. 7] TimeDateStamp
    //   [ 8..11] Version (major/minor)
    //   [12..15] NameRva (DLL name; pointed at "lib.dll" string)
    //   [16..19] OrdinalBase
    //   [20..23] AddressTableEntries (function count)
    //   [24..27] NumberOfNamePointers (named export count)
    //   [28..31] AddressOfFunctions
    //   [32..35] AddressOfNames
    //   [36..39] AddressOfNameOrdinals
    //
    // Then: name pointer table (4 bytes × N) at AddressOfNames RVA
    // Then: ordinals table (2 bytes × N) — we leave it but don't read
    // Then: function RVA table (4 bytes × N)
    // Then: NUL-terminated name strings packed back-to-back.

    std::uint32_t const sectionRva = kSectionRawDataOff;  // RVA = file offset
    std::uint32_t const exportDirRva = sectionRva;
    constexpr std::uint32_t kExportDirSize = 40;

    std::uint32_t const namesTableRva = exportDirRva + kExportDirSize;
    std::uint32_t const namesTableSize =
        4u * static_cast<std::uint32_t>(exportNames.size());

    // Skip the function-RVA + ordinal tables (we don't read them in v1
    // but they should exist for completeness — keep them zero).
    std::uint32_t const funcsTableRva = namesTableRva + namesTableSize;
    std::uint32_t const funcsTableSize = namesTableSize;
    std::uint32_t const ordsTableRva   = funcsTableRva + funcsTableSize;
    std::uint32_t const ordsTableSize  =
        2u * static_cast<std::uint32_t>(exportNames.size());

    std::uint32_t const stringsRva = ordsTableRva + ordsTableSize;

    // Lay out the strings + remember each name's RVA.
    std::vector<std::uint32_t> nameRvas;
    std::vector<std::uint8_t> stringsRaw;
    nameRvas.reserve(exportNames.size());
    for (auto const& n : exportNames) {
        nameRvas.push_back(stringsRva
            + static_cast<std::uint32_t>(stringsRaw.size()));
        for (char c : n) stringsRaw.push_back(static_cast<std::uint8_t>(c));
        stringsRaw.push_back(0);
    }
    std::uint32_t const stringsSize =
        static_cast<std::uint32_t>(stringsRaw.size());
    std::uint32_t const sectionSize =
        kExportDirSize + namesTableSize + funcsTableSize + ordsTableSize
        + stringsSize;

    // Now back-patch DataDirectories[0] = (exportDirRva, kExportDirSize)
    putU32(b, dataDirsOff + 0, exportDirRva);
    putU32(b, dataDirsOff + 4, kExportDirSize);

    // ── Section header ──
    char const* const sectionName = ".edata";
    std::memcpy(&b[kSectionHdrOff], sectionName, std::strlen(sectionName));
    putU32(b, kSectionHdrOff +  8, sectionSize);         // VirtualSize
    putU32(b, kSectionHdrOff + 12, sectionRva);          // VirtualAddress
    putU32(b, kSectionHdrOff + 16, sectionSize);         // SizeOfRawData
    putU32(b, kSectionHdrOff + 20, kSectionRawDataOff);  // PointerToRawData

    // ── Append section raw data ──
    // Export directory (40 bytes).
    std::vector<std::uint8_t> ed(kExportDirSize, 0);
    // NumberOfNamePointers @ [24..27]
    putU32(ed, 24, static_cast<std::uint32_t>(exportNames.size()));
    // AddressTableEntries @ [20..23]
    putU32(ed, 20, static_cast<std::uint32_t>(exportNames.size()));
    // AddressOfFunctions @ [28..31]
    putU32(ed, 28, funcsTableRva);
    // AddressOfNames @ [32..35]
    putU32(ed, 32, namesTableRva);
    // AddressOfNameOrdinals @ [36..39]
    putU32(ed, 36, ordsTableRva);
    b.insert(b.end(), ed.begin(), ed.end());

    // Names table: u32 RVAs.
    for (auto rva : nameRvas) appU32(b, rva);
    // Funcs table: zero stubs.
    for (std::size_t i = 0; i < exportNames.size(); ++i) appU32(b, 0u);
    // Ordinals table: zero stubs.
    for (std::size_t i = 0; i < exportNames.size(); ++i) appU16(b, 0u);
    // Name strings.
    b.insert(b.end(), stringsRaw.begin(), stringsRaw.end());

    return b;
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
