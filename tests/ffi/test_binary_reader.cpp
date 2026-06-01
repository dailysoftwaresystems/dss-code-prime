// Plan 11 FF1 (ELF half) tests — `dss::ffi::readImportsFromBytes`.
//
// Pins:
//   * ELF magic + ELFCLASS64 + ELFDATA2LSB detection.
//   * `.dynsym` + `.dynstr` round-trip from a synthesized ELF.
//   * Format-blind dispatch: PE/Mach-O magics emit
//     UnsupportedFormat citing the future FF1-PE/FF1-MachO anchors.
//   * Failure modes: empty file, unknown magic, ELFCLASS32 reject,
//     truncated section table, missing .dynsym.
//   * Symbol kind / visibility / linkage mapping from ELF
//     STT_/STV_/STB_ to closed-enum ImportSurface fields.
//
// Test strategy: synthesize minimal ELF binaries directly in C++ via
// the byte-emit helpers + read them back. Avoids dependency on
// pre-staged test fixtures while pinning every parser branch.

#include "core/types/diagnostic_reporter.hpp"
#include "ffi/binary_reader.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace dss;
using namespace dss::ffi;

namespace {

// Append a u16/u32/u64 little-endian to a byte vector.
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

// One Elf64_Sym entry as packed bytes (24 bytes).
struct Sym {
    std::uint32_t name = 0;     // index into .dynstr
    std::uint8_t  info = 0;     // STB << 4 | STT
    std::uint8_t  other = 0;    // visibility
    std::uint16_t shndx = 0;
    std::uint64_t value = 0;
    std::uint64_t size = 0;
};

void appendSym(std::vector<std::uint8_t>& b, Sym const& s) {
    appU32(b, s.name);
    b.push_back(s.info);
    b.push_back(s.other);
    appU16(b, s.shndx);
    appU64(b, s.value);
    appU64(b, s.size);
}

// Build a minimal ELF64 LE with `.dynsym` + `.dynstr` containing the
// given symbols. The shstrtab section gets the well-known names.
// Returns the byte image.
std::vector<std::uint8_t> buildMinimalElf64(std::vector<Sym> const& syms,
                                              std::vector<std::string> const& names) {
    // Layout we'll lay down:
    //   [0..63]            Ehdr (64 bytes)
    //   [64..]             .dynstr  — concatenated NUL-terminated names (starts with NUL sentinel)
    //   [aligned 8]        .dynsym  — N entries of 24 bytes each
    //   [aligned 1]        .shstrtab — section names
    //   [aligned 8]        Section header table — 4 sections (NULL, dynstr, dynsym, shstrtab)
    //
    // sh_link of .dynsym points at .dynstr's section index.

    std::vector<std::uint8_t> bytes;
    bytes.resize(64, 0);  // Ehdr placeholder

    // Build .dynstr
    std::vector<std::uint32_t> nameOffsets;
    std::vector<std::uint8_t> dynstr;
    dynstr.push_back(0);  // sentinel
    for (auto const& n : names) {
        nameOffsets.push_back(static_cast<std::uint32_t>(dynstr.size()));
        for (char c : n) dynstr.push_back(static_cast<std::uint8_t>(c));
        dynstr.push_back(0);
    }
    std::uint64_t const dynstrOff = bytes.size();
    bytes.insert(bytes.end(), dynstr.begin(), dynstr.end());

    // Align to 8 for .dynsym
    while (bytes.size() % 8 != 0) bytes.push_back(0);
    std::uint64_t const dynsymOff = bytes.size();

    // .dynsym: slot 0 = STN_UNDEF (all-zero), then provided syms.
    Sym stnUndef{};
    appendSym(bytes, stnUndef);
    for (std::size_t i = 0; i < syms.size(); ++i) {
        Sym sym = syms[i];
        if (i < nameOffsets.size()) sym.name = nameOffsets[i];
        appendSym(bytes, sym);
    }
    std::uint64_t const dynsymSize = bytes.size() - dynsymOff;

    // shstrtab
    std::uint64_t const shstrtabOff = bytes.size();
    bytes.push_back(0);  // sentinel
    auto pushName = [&](char const* name) -> std::uint32_t {
        std::uint32_t off = static_cast<std::uint32_t>(bytes.size() - shstrtabOff);
        for (char const* p = name; *p; ++p) bytes.push_back(static_cast<std::uint8_t>(*p));
        bytes.push_back(0);
        return off;
    };
    std::uint32_t const nDynstr   = pushName(".dynstr");
    std::uint32_t const nDynsym   = pushName(".dynsym");
    std::uint32_t const nShstrtab = pushName(".shstrtab");
    std::uint64_t const shstrtabSize = bytes.size() - shstrtabOff;

    // Section header table — 4 entries (NULL, dynstr=1, dynsym=2, shstrtab=3)
    while (bytes.size() % 8 != 0) bytes.push_back(0);
    std::uint64_t const shtOff = bytes.size();

    auto writeShdr = [&](std::uint32_t name, std::uint32_t type,
                          std::uint64_t flags, std::uint64_t off,
                          std::uint64_t size, std::uint32_t link,
                          std::uint64_t entsize) {
        appU32(bytes, name);
        appU32(bytes, type);
        appU64(bytes, flags);
        appU64(bytes, 0);   // addr
        appU64(bytes, off);
        appU64(bytes, size);
        appU32(bytes, link);
        appU32(bytes, 0);   // info
        appU64(bytes, 1);   // addralign
        appU64(bytes, entsize);
    };
    // NULL section
    writeShdr(0, 0, 0, 0, 0, 0, 0);
    // .dynstr (idx 1, SHT_STRTAB=3)
    writeShdr(nDynstr,   3, 0, dynstrOff,
              dynstr.size(), 0, 0);
    // .dynsym (idx 2, SHT_DYNSYM=11, sh_link = 1 pointing at .dynstr,
    // entsize=24).
    writeShdr(nDynsym,  11, 0, dynsymOff, dynsymSize, 1, 24);
    // .shstrtab (idx 3, SHT_STRTAB=3)
    writeShdr(nShstrtab, 3, 0, shstrtabOff, shstrtabSize, 0, 0);

    // Now fill in the Ehdr at [0..63]
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2;  // EI_CLASS = ELFCLASS64
    bytes[5] = 1;  // EI_DATA  = ELFDATA2LSB
    bytes[6] = 1;  // EI_VERSION = EV_CURRENT
    // pad [7..15] left zero
    // e_type at [16..17] = ET_DYN = 3
    bytes[16] = 3; bytes[17] = 0;
    // e_machine at [18..19] = EM_X86_64 = 62
    bytes[18] = 62; bytes[19] = 0;
    // e_version at [20..23] = 1
    bytes[20] = 1;
    // e_entry [24..31], e_phoff [32..39] left zero (we have no PHT)
    // e_shoff at [40..47] = shtOff
    std::memcpy(&bytes[40], &shtOff, 8);
    // e_flags [48..51] = 0; e_ehsize [52..53] = 64; e_phentsize [54..55] = 0
    bytes[52] = 64;
    // e_phnum [56..57] = 0
    // e_shentsize [58..59] = 64; e_shnum [60..61] = 4; e_shstrndx [62..63] = 3
    bytes[58] = 64;
    bytes[60] = 4;
    bytes[62] = 3;

    return bytes;
}

constexpr std::uint8_t info(std::uint8_t bind, std::uint8_t type) {
    return static_cast<std::uint8_t>((bind << 4) | (type & 0xF));
}

} // namespace

// ── Happy-path: read a synthesized ELF64 with 3 symbols ──────────

TEST(BinaryReaderElf, ReadsDynamicSymbolsRoundTrip) {
    std::vector<std::string> names = {"printf", "errno", "malloc"};
    std::vector<Sym> syms;
    syms.push_back({0, info(1 /*STB_GLOBAL*/, 2 /*STT_FUNC*/), 0, 1, 0x1000, 16});
    syms.push_back({0, info(1, 1 /*STT_OBJECT*/), 2 /*STV_HIDDEN*/, 1, 0x2000, 4});
    syms.push_back({0, info(2 /*STB_WEAK*/, 2 /*STT_FUNC*/), 0, 1, 0x3000, 32});

    auto const bytes = buildMinimalElf64(syms, names);
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "libtest.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->size(), 3u);

    EXPECT_EQ((*r)[0].mangledName, "printf");
    EXPECT_EQ((*r)[0].libraryPath, "libtest.so");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    EXPECT_EQ((*r)[0].visibility, SymbolVisibility::Default);
    EXPECT_EQ((*r)[0].linkage, SymbolLinkage::External);

    EXPECT_EQ((*r)[1].mangledName, "errno");
    EXPECT_EQ((*r)[1].kind, SymbolKind::Object);
    EXPECT_EQ((*r)[1].visibility, SymbolVisibility::Hidden);

    EXPECT_EQ((*r)[2].mangledName, "malloc");
    EXPECT_EQ((*r)[2].linkage, SymbolLinkage::Weak);
}

// ── Local symbols are skipped (don't export) ─────────────────────

TEST(BinaryReaderElf, SkipsLocalBindSymbols) {
    std::vector<std::string> names = {"local_helper", "exported_fn"};
    std::vector<Sym> syms;
    syms.push_back({0, info(0 /*STB_LOCAL*/, 2), 0, 1, 0x1000, 16});
    syms.push_back({0, info(1 /*STB_GLOBAL*/, 2), 0, 1, 0x2000, 16});

    auto const bytes = buildMinimalElf64(syms, names);
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "lib.so", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "exported_fn");
}

// ── Failure modes ────────────────────────────────────────────────

TEST(BinaryReader, EmptyFileFailsLoud) {
    std::vector<std::uint8_t> empty;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(empty, "empty.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::FileEmpty);
}

TEST(BinaryReader, UnknownMagicFailsLoud) {
    std::vector<std::uint8_t> garbage = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(garbage, "garbage.bin", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnknownFormat);
}

TEST(BinaryReader, PeMagicEmitsUnsupportedFormatCitingFutureAnchor) {
    std::vector<std::uint8_t> pe = {'M', 'Z', 0x00, 0x00, 0x00};
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(pe, "fake.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnsupportedFormat);
    EXPECT_NE(r.error().detail.find("FF1-PE"), std::string::npos);
}

TEST(BinaryReader, MachoMagicEmitsUnsupportedFormatCitingFutureAnchor) {
    std::vector<std::uint8_t> macho = {0xCF, 0xFA, 0xED, 0xFE, 0x00};  // 0xFEEDFACF LE
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(macho, "fake.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnsupportedFormat);
    EXPECT_NE(r.error().detail.find("FF1-MachO"), std::string::npos);
}

TEST(BinaryReaderElf, RejectsElfClass32) {
    // ELF magic but EI_CLASS=1 (ELFCLASS32) — v1 supports ELF64 only.
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 1;  // ELFCLASS32
    bytes[5] = 1;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "32bit.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnsupportedElfClass);
}

TEST(BinaryReaderElf, RejectsElfBigEndian) {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2;
    bytes[5] = 2;  // ELFDATA2MSB
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "big.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnsupportedElfClass);
}

TEST(BinaryReaderElf, ShorterThanEhdrFailsLoud) {
    std::vector<std::uint8_t> bytes(32, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2; bytes[5] = 1;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
}

TEST(BinaryReaderElf, DynsymSizeNotMultipleOfEntsizeFailsLoud) {
    // Post-fold #1 silent-failure fix: dynsymSize / 24 silently floored
    // before. Now rejects with CorruptedBinary if size % 24 != 0.
    // Build a normal ELF then poke the dynsym sh_size to 25.
    std::vector<Sym> syms{ {0, info(1, 2), 0, 1, 0x1000, 16} };
    auto bytes = buildMinimalElf64(syms, {"f"});
    // Locate shdr 2 (.dynsym) and corrupt its sh_size field.
    std::uint64_t shtOff = 0;
    std::memcpy(&shtOff, &bytes[40], 8);
    // sh_size is at offset 32 of each shdr; shdr 2 starts at shtOff + 2*64.
    bytes[shtOff + 2 * 64 + 32 + 0] = 25;
    bytes[shtOff + 2 * 64 + 32 + 1] = 0;  // size now declared 25, not multiple of 24

    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "corrupt.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
}

TEST(BinaryReaderElf, NoDynsymSectionFailsLoud) {
    // Build an ELF with NO .dynsym (only NULL + .shstrtab).
    std::vector<std::uint8_t> bytes;
    bytes.resize(64, 0);
    // shstrtab
    std::uint64_t const shstrtabOff = bytes.size();
    bytes.push_back(0);
    std::uint32_t const nShstrtab = static_cast<std::uint32_t>(bytes.size() - shstrtabOff);
    for (char const* p = ".shstrtab"; *p; ++p) bytes.push_back(static_cast<std::uint8_t>(*p));
    bytes.push_back(0);
    std::uint64_t const shstrtabSize = bytes.size() - shstrtabOff;
    while (bytes.size() % 8 != 0) bytes.push_back(0);
    std::uint64_t const shtOff = bytes.size();
    // NULL section
    for (int i = 0; i < 64; ++i) bytes.push_back(0);
    // .shstrtab section
    appU32(bytes, nShstrtab);
    appU32(bytes, 3);    // SHT_STRTAB
    appU64(bytes, 0); appU64(bytes, 0);
    appU64(bytes, shstrtabOff);
    appU64(bytes, shstrtabSize);
    appU32(bytes, 0); appU32(bytes, 0);
    appU64(bytes, 1); appU64(bytes, 0);

    // Fill Ehdr
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2; bytes[5] = 1; bytes[6] = 1;
    bytes[16] = 3;   // ET_DYN
    bytes[18] = 62;  // EM_X86_64
    bytes[20] = 1;
    std::memcpy(&bytes[40], &shtOff, 8);
    bytes[52] = 64; bytes[58] = 64; bytes[60] = 2; bytes[62] = 1;

    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "stripped.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::SectionNotFound);
}

// ── readImports(path) — file-based entry point ───────────────────

// Post-fold #1: pin the file-based entry's ENOENT path.
// `readImports(path)` opens an ifstream; a nonexistent path produces
// `FileOpenFailed`. Without this test, a regression that drops the
// `if (!in)` guard would silently fall through to FileEmpty.
TEST(BinaryReaderFile, NonExistentPathReturnsFileOpenFailed) {
    DiagnosticReporter rep;
    auto r = readImports(
        std::filesystem::path{"/this/path/does/not/exist/libnope.so"},
        rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::FileOpenFailed);
    // Verify it ALSO emitted F_FileOpenFailed through the reporter
    // (post-fold #1 wired the kind→F_* mapping).
    bool sawF = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_FileOpenFailed) sawF = true;
    }
    EXPECT_TRUE(sawF);
}

// Post-fold #1: pin that every failure path emits through the
// reporter (not just returns the BinaryReadError). The CLI's
// --suppress=<code> policy needs the diagnostic to actually reach
// the reporter to fire.
TEST(BinaryReaderFile, EmptyBytesEmitsFFileEmptyThroughReporter) {
    DiagnosticReporter rep;
    auto r = readImportsFromBytes({}, "empty.so", rep);
    ASSERT_FALSE(r.has_value());
    bool sawF = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_FileEmpty) sawF = true;
    }
    EXPECT_TRUE(sawF);
}

// ── Diagnostic-name round-trip ───────────────────────────────────

TEST(BinaryReaderError, KindNameRoundTrip) {
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::FileOpenFailed),
              "FileOpenFailed");
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::FileEmpty),
              "FileEmpty");
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::UnknownFormat),
              "UnknownFormat");
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::UnsupportedFormat),
              "UnsupportedFormat");
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::CorruptedBinary),
              "CorruptedBinary");
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::UnsupportedElfClass),
              "UnsupportedElfClass");
    EXPECT_EQ(binaryReadErrorKindName(BinaryReadErrorKind::SectionNotFound),
              "SectionNotFound");
}

// ── F_* diagnostic-code round-trip ───────────────────────────────

TEST(BinaryReaderError, FDiagnosticCodesRoundTrip) {
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_FileOpenFailed),
              "F_FileOpenFailed");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_UnknownBinaryFormat),
              "F_UnknownBinaryFormat");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_UnsupportedBinaryFormat),
              "F_UnsupportedBinaryFormat");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_CorruptedBinary),
              "F_CorruptedBinary");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_UnsupportedElfClass),
              "F_UnsupportedElfClass");
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::F_SectionNotFound),
              "F_SectionNotFound");
}

TEST(BinaryReaderError, FCodePrefixUsesFLetter) {
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::F_FileOpenFailed),
              "F0001");
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::F_SectionNotFound),
              "F0007");
}

// ── Post-fold #2 (pr-test-analyzer Gap 1, P9):
//      rangeExceedsBuffer u64-wrap-bypass direct unit test ──
//
// The CRITICAL silent-failure the post-fold #1 closed was the naive
// `off + size > totalSize` wrapping on u64 — a hostile/corrupted
// `.so` with `sh_offset = UINT64_MAX-4, sh_size = 8` would slip past
// the check. Pinning the helper directly (vs only the end-to-end
// ELF synthesis) defends against future parser-order refactors that
// could move the bounds check past earlier validations.

TEST(RangeExceedsBuffer, ZeroSizeAtStartFits) {
    EXPECT_FALSE(rangeExceedsBuffer(0, 0, 100));
}

TEST(RangeExceedsBuffer, ZeroSizeAtExactEndFits) {
    EXPECT_FALSE(rangeExceedsBuffer(100, 0, 100));
}

TEST(RangeExceedsBuffer, OneByteOverrunExceeds) {
    EXPECT_TRUE(rangeExceedsBuffer(99, 2, 100));
}

TEST(RangeExceedsBuffer, OffsetAtTotalWithSizeExceeds) {
    EXPECT_TRUE(rangeExceedsBuffer(100, 1, 100));
}

TEST(RangeExceedsBuffer, OffsetBeyondTotalExceeds) {
    EXPECT_TRUE(rangeExceedsBuffer(101, 0, 100));
}

TEST(RangeExceedsBuffer, U64WrapBypassCatchesHostileOffset) {
    // The exact silent-failure CRITICAL: hostile sh_offset =
    // UINT64_MAX - 4 + sh_size = 8 would wrap to 4 under naive
    // `off + size`, slipping under any `totalSize`. The safe form
    // computes `size > totalSize - off`, catching the overflow.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rangeExceedsBuffer(kMax - 4, 8, 100));
}

TEST(RangeExceedsBuffer, MaxOffsetMaxTotalExceeds) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_TRUE(rangeExceedsBuffer(kMax, 1, kMax));
}

// ── Post-fold #2: pin every failure path to reporter F_* emission ─

// pr-test-analyzer Gap 2 (priority 8): the toDiagnosticCode mapping
// + emitAndReturn wiring is what makes `--suppress=F_*` work. The
// existing tests assert error().kind but didn't scan the reporter
// for the matching F_* code. A regression that bypasses
// emitAndReturn (e.g. `return std::unexpected(BinaryReadError{...})`
// directly) would compile clean + ship — and `--suppress` would
// silently stop working for that path.
TEST(BinaryReaderReporter, UnknownMagicAlsoEmitsFCodeThroughReporter) {
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(
        std::vector<std::uint8_t>{0xAA, 0xBB, 0xCC, 0xDD, 0xEE},
        "garbage.bin", rep);
    ASSERT_FALSE(r.has_value());
    bool sawF = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_UnknownBinaryFormat) sawF = true;
    }
    EXPECT_TRUE(sawF);
}

TEST(BinaryReaderReporter, PeMagicAlsoEmitsFCodeThroughReporter) {
    DiagnosticReporter rep;
    std::vector<std::uint8_t> pe = {'M', 'Z', 0x00, 0x00};
    auto r = readImportsFromBytes(pe, "fake.dll", rep);
    ASSERT_FALSE(r.has_value());
    bool sawF = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_UnsupportedBinaryFormat) sawF = true;
    }
    EXPECT_TRUE(sawF);
}

TEST(BinaryReaderReporter, Elf32AlsoEmitsFCodeThroughReporter) {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 1; bytes[5] = 1;  // ELFCLASS32
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "32bit.so", rep);
    ASSERT_FALSE(r.has_value());
    bool sawF = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_UnsupportedElfClass) sawF = true;
    }
    EXPECT_TRUE(sawF);
}

TEST(BinaryReaderReporter, CorruptedBinaryAlsoEmitsFCodeThroughReporter) {
    std::vector<std::uint8_t> bytes(32, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2; bytes[5] = 1;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny.so", rep);
    ASSERT_FALSE(r.has_value());
    bool sawF = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_CorruptedBinary) sawF = true;
    }
    EXPECT_TRUE(sawF);
}

// (Former `Ff1ProducedRowsHaveNoCSignature` test removed at FF2
// post-#2 type-design fold: `cSignature` field dropped from
// `ImportSurface` since no producer or consumer needed it.
// Anchored D-FF2-1: re-add `optional<FnSigTypeId>` only if FF3 needs
// to attach the resolved sig to the row instead of the HIR node.)
