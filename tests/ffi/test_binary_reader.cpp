// Plan 11 FF1 (ELF half) tests — `dss::ffi::readImportsFromBytes`.
//
// Pins:
//   * ELF magic + ELFCLASS64 + ELFDATA2LSB detection.
//   * `.dynsym` + `.dynstr` round-trip from a synthesized ELF.
//   * Format-blind dispatch: PE magic dispatches into the readPe
//     path (see test_binary_reader_pe.cpp); Mach-O 64-bit magic
//     dispatches into readMacho (see test_binary_reader_macho.cpp);
//     Mach-O FAT (0xCAFEBABE) and Mach-O 32 (0xFEEDFACE) surface
//     UnsupportedFormat with remediation-specific detail (anchors
//     D-FF1-MACHO-FAT and D-FF1-MACHO-32).
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
#include "byte_emit.hpp"
#include "diagnostic_count.hpp"
#include "repo_root.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::countCode;
using dss::test_support::appU16;
using dss::test_support::appU32;
using dss::test_support::appU64;

namespace {

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

// DT_* dynamic-array tags (gABI Fig. 5-10). DT_SONAME is the embedded,
// loader-resolvable library identity the c171 reader extracts.
constexpr std::uint64_t kDtNull   = 0;    // DT_NULL   — terminates the .dynamic array
constexpr std::uint64_t kDtSoname = 14;   // DT_SONAME — d_val = a .dynstr offset

// Build a minimal ELF64 LE with `.dynsym` + `.dynstr` containing the
// given symbols. The shstrtab section gets the well-known names.
// Returns the byte image.
//
// D-FF1-READER-SONAME (c171): a non-empty `soname` (default empty) adds a
// 5th `.dynamic` section (SHT_DYNAMIC=6, sh_entsize=16) holding two Elf64_Dyn
// entries — {d_tag=DT_SONAME, d_val=<the soname's .dynstr offset>} and a
// {DT_NULL, 0} terminator — and appends the soname string into the SAME
// `.dynstr` the symbol names index. Empty leaves the EXISTING 4-section image
// byte-for-byte unchanged, so every pre-soname caller (and the
// NoDynamicSectionLeavesSonameEmpty negative) is exact.
std::vector<std::uint8_t> buildMinimalElf64(std::vector<Sym> const& syms,
                                              std::vector<std::string> const& names,
                                              std::string const& soname = {}) {
    bool const hasDynamic = !soname.empty();
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
    // DT_SONAME's d_val indexes this SAME `.dynstr`; record the soname
    // string's offset before appending it (empty soname adds nothing).
    std::uint32_t sonameStrOff = 0;
    if (hasDynamic) {
        sonameStrOff = static_cast<std::uint32_t>(dynstr.size());
        for (char c : soname) dynstr.push_back(static_cast<std::uint8_t>(c));
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
    std::uint32_t const nDynamic  = hasDynamic ? pushName(".dynamic") : 0u;
    std::uint64_t const shstrtabSize = bytes.size() - shstrtabOff;

    // `.dynamic` array (only when a soname is requested): one DT_SONAME
    // entry (d_val = the soname's .dynstr offset) + a DT_NULL terminator,
    // 8-aligned, between `.shstrtab` and the section header table.
    std::uint64_t dynamicOff = 0, dynamicSize = 0;
    if (hasDynamic) {
        while (bytes.size() % 8 != 0) bytes.push_back(0);
        dynamicOff = bytes.size();
        appU64(bytes, kDtSoname); appU64(bytes, sonameStrOff);  // DT_SONAME
        appU64(bytes, kDtNull);   appU64(bytes, 0);             // DT_NULL
        dynamicSize = bytes.size() - dynamicOff;
    }

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
    // .dynamic (idx 4, SHT_DYNAMIC=6, sh_link=1 → .dynstr, entsize=16) —
    // only when a soname was requested. The reader keys on sh_type==6.
    if (hasDynamic) {
        writeShdr(nDynamic, 6, 0, dynamicOff, dynamicSize, 1, 16);
    }

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
    bytes[60] = static_cast<std::uint8_t>(hasDynamic ? 5 : 4);  // e_shnum
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

// ── D-FF1-READER-SONAME (c171): DT_SONAME extraction ─────────────

// STRICT: the `.dynamic`/DT_SONAME the builder emits must surface on
// EVERY row's `soname`, verbatim ("libwidget.so.2").
TEST(BinaryReaderElf, ExtractsDtSonameFromDynamic) {
    std::vector<std::string> names = {"printf", "malloc"};
    std::vector<Sym> syms;
    syms.push_back({0, info(1 /*STB_GLOBAL*/, 2 /*STT_FUNC*/), 0, 1, 0x1000, 16});
    syms.push_back({0, info(1, 2), 0, 1, 0x2000, 16});

    auto const bytes = buildMinimalElf64(syms, names, "libwidget.so.2");
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "/build/out/libwidget-9a3f.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->size(), 2u);
    for (auto const& row : *r) {
        EXPECT_EQ(row.soname, "libwidget.so.2");
    }
    // The path label stays on libraryPath — soname is the SEPARATE
    // embedded identity, NOT the on-disk basename.
    EXPECT_EQ((*r)[0].libraryPath, "/build/out/libwidget-9a3f.so");
    EXPECT_EQ(rep.errorCount(), 0u);
}

// RED-ON-DISABLE: the EXISTING synthesizer emits no `.dynamic`, so
// every row's soname MUST be empty. Fails if the extractor ever
// fabricated a soname (or defaulted it to the basename).
TEST(BinaryReaderElf, NoDynamicSectionLeavesSonameEmpty) {
    std::vector<std::string> names = {"printf", "malloc"};
    std::vector<Sym> syms;
    syms.push_back({0, info(1, 2), 0, 1, 0x1000, 16});
    syms.push_back({0, info(1, 2), 0, 1, 0x2000, 16});

    auto const bytes = buildMinimalElf64(syms, names);  // no .dynamic
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "libplain.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->size(), 2u);
    for (auto const& row : *r) {
        EXPECT_TRUE(row.soname.empty())
            << "no .dynamic/DT_SONAME must leave soname empty; got '"
            << row.soname << "'";
    }
}

// An out-of-range DT_SONAME d_val must bound to an empty soname
// (readNulTerminated stops at the .dynstr end) — no crash, no garbage.
TEST(BinaryReaderElf, DtSonameOffsetPastDynstrLeavesSonameEmpty) {
    std::vector<std::string> names = {"printf"};
    std::vector<Sym> syms;
    syms.push_back({0, info(1, 2), 0, 1, 0x1000, 16});
    auto bytes = buildMinimalElf64(syms, names, "libwidget.so.2");

    // Locate the .dynamic section (SHT_DYNAMIC=6) via the section header
    // table and poison its first Elf64_Dyn (DT_SONAME) d_val — at
    // sh_offset+8 — to a wildly out-of-range .dynstr offset.
    std::uint64_t shtOff = 0;
    std::memcpy(&shtOff, &bytes[40], 8);
    std::uint16_t shnum = 0;
    std::memcpy(&shnum, &bytes[60], 2);
    std::uint64_t dynOff = 0;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::size_t const sh = static_cast<std::size_t>(shtOff) + i * 64u;
        std::uint32_t shType = 0;
        std::memcpy(&shType, &bytes[sh + 4], 4);
        if (shType == 6u) {  // SHT_DYNAMIC
            std::memcpy(&dynOff, &bytes[sh + 24], 8);  // sh_offset
            break;
        }
    }
    ASSERT_NE(dynOff, 0u) << "fixture must contain a .dynamic section";
    std::uint64_t const badOff = 0xFFFFFFFFu;
    std::memcpy(&bytes[static_cast<std::size_t>(dynOff) + 8], &badOff, 8);

    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "libwidget.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->size(), 1u);
    EXPECT_TRUE((*r)[0].soname.empty())
        << "an out-of-range DT_SONAME d_val must resolve to empty, not read "
           "past .dynstr; got '" << (*r)[0].soname << "'";
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

namespace {
// The surfaced names, comma-joined, so an assertion can compare CONTENT in
// one shot and a failure prints exactly which names came back. Comparing the
// joined string (rather than a size) is deliberate: an equal-size surface
// holding the WRONG name must fail.
[[nodiscard]] std::string surfacedNames(std::vector<ImportSurface> const& rows) {
    std::string joined;
    for (auto const& row : rows) {
        if (!joined.empty()) joined += ",";
        joined += row.mangledName;
    }
    return joined;
}
}  // namespace

// ── SHN_UNDEF rows are the library's IMPORTS, never its exports ──
//
// D-LK-ELF-EMITS-ONE-DT-NEEDED-WHEN-TWO-LIBRARIES-ARE-REFERENCED — the
// root-cause pin. `.dynsym` is not an export table: it holds the
// definitions the library offers AND the references it makes, told apart
// only by `st_shndx` (SHN_UNDEF == a reference). This reader surfaced
// both, so DSS believed libtcl8.6.so EXPORTED the 14 zlib names it merely
// IMPORTS; `ingest()`'s first-source-wins then bound `deflateBound` to
// libtcl8.6.so and libz.so.1 vanished from the emitted DT_NEEDED set.
//
// The assertion is on CONTENT, not on the count: a size-only check passes
// for a surface holding the WRONG one of the two names.
TEST(BinaryReaderElf, SkipsUndefinedSymbolsWhichAreImportsNotExports) {
    std::vector<std::string> names = {"tcl_defines_this", "zlib_defines_this"};
    std::vector<Sym> syms;
    // A real definition: st_shndx = 1 (a real section index).
    syms.push_back({0, info(1 /*STB_GLOBAL*/, 2 /*STT_FUNC*/), 0, 1, 0x1000, 16});
    // A reference this library MAKES: st_shndx = 0 (SHN_UNDEF), which is
    // exactly the shape libtcl8.6.so carries for every zlib name.
    syms.push_back({0, info(1 /*STB_GLOBAL*/, 2 /*STT_FUNC*/), 0, 0, 0, 0});

    auto const bytes = buildMinimalElf64(syms, names);
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "libtcl-like.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    EXPECT_EQ(surfacedNames(*r), "tcl_defines_this")
        << "the SHN_UNDEF row is a reference this library MAKES, not a symbol "
           "it exports; surfacing it makes `ingest()` bind a caller's extern "
           "to a library that does not define it (the DT_NEEDED defect)";
}

// A WEAK undefined reference is still a reference. glibc-style weak imports
// (`__pthread_key_create` and friends) are SHN_UNDEF with STB_WEAK, so a
// filter written on the bind class instead of `st_shndx` would let them
// through. Content-anchored: the defined weak export MUST survive, so this
// cannot pass by filtering weakness itself.
TEST(BinaryReaderElf, WeakUndefinedIsSkippedButWeakDefinedSurvives) {
    std::vector<std::string> names = {"weak_defined_here", "weak_referenced_only"};
    std::vector<Sym> syms;
    syms.push_back({0, info(2 /*STB_WEAK*/, 2), 0, 1, 0x1000, 16});
    syms.push_back({0, info(2 /*STB_WEAK*/, 2), 0, 0, 0, 0});

    auto const bytes = buildMinimalElf64(syms, names);
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "libweak.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    ASSERT_EQ(surfacedNames(*r), "weak_defined_here");
    EXPECT_EQ((*r)[0].linkage, SymbolLinkage::Weak)
        << "definedness is the filter; the weak BIND must still be reported";
}

// SHN_ABS (0xFFF1) is a DEFINITION with no section — a version-script
// absolute, a linker-provided address constant. Only SHN_UNDEF means
// "not defined here", so the filter must not overreach to every special
// index. This is the control that keeps the fix from becoming
// `st_shndx < SHN_LORESERVE`.
TEST(BinaryReaderElf, AbsoluteSectionIndexIsADefinitionAndSurvives) {
    std::vector<std::string> names = {"abs_constant"};
    std::vector<Sym> syms;
    syms.push_back({0, info(1, 1 /*STT_OBJECT*/), 0, 0xFFF1 /*SHN_ABS*/, 0x40, 8});

    auto const bytes = buildMinimalElf64(syms, names);
    DiagnosticReporter rep;
    auto const r = readImportsFromBytes(
        std::span<std::uint8_t const>{bytes.data(), bytes.size()},
        "libabs.so", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    EXPECT_EQ(surfacedNames(*r), "abs_constant");
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

TEST(BinaryReader, PeMagicTooShortToBeValidIsCorrupted) {
    // FF1-PE landed 2026-06-01. PE magic ('MZ') now dispatches to
    // readPe(); a too-short PE buffer (no PE-signature header) fails
    // loud as CorruptedBinary (not UnsupportedFormat as before).
    std::vector<std::uint8_t> pe(64, 0);
    pe[0] = 'M'; pe[1] = 'Z';
    // DOS[0x3C] = 0 — PE signature would be at offset 0 which has 'MZ' not 'PE\0\0'.
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(pe, "fake.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("PE signature missing"),
              std::string::npos);
}

// FF1-MachO 2026-06-01: Mach-O 64-bit magic now dispatches into
// readMacho(); see tests/ffi/test_binary_reader_macho.cpp for the
// happy-path + corruption coverage. Here we pin the two
// UnsupportedFormat arms that the dispatch still surfaces:
// universal/FAT binaries and 32-bit Mach-O (both recognised, both
// route to UnsupportedFormat with remediation-specific messages —
// anchors D-FF1-MACHO-FAT and D-FF1-MACHO-32).
TEST(BinaryReader, MachoFatMagicDispatchesToUnsupportedFormat) {
    // FAT_MAGIC on disk. `struct fat_header` is BIG-ENDIAN ON DISK by
    // definition (Apple <mach-o/fat.h>), so 0xCAFEBABE is the byte
    // sequence CA FE BA BE — NOT the BE BA FE CA this fixture used to
    // carry. That stale spelling was the byte-swapped FAT_CIGAM view, and
    // it made this test green against a `guessFormat` that could never
    // classify a real universal binary. See the D-FF1-MACHO-FAT block in
    // tests/ffi/test_binary_reader_macho.cpp for the full fixtures.
    std::vector<std::uint8_t> fat = {0xCA, 0xFE, 0xBA, 0xBE, 0x00};
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(fat, "fake.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnsupportedFormat);
    EXPECT_NE(r.error().detail.find("FAT"), std::string::npos)
        << "operator must see remediation guidance (lipo -thin)";
    EXPECT_NE(r.error().detail.find("D-FF1-MACHO-FAT"), std::string::npos);
}

// The byte-SWAPPED spelling (FAT_CIGAM, 0xBEBAFECA) is what a
// little-endian host computes when it loads a real `fat_header` in HOST
// order — it is never the on-disk byte sequence of a universal binary.
// A file that literally starts BE BA FE CA is therefore NOT fat and must
// not borrow the `lipo -thin` remediation. This is exactly the input the
// pre-fix little-endian `readU32(b,0) == 0xCAFEBABE` test was matching.
TEST(BinaryReader, ByteSwappedFatSpellingIsNotAcceptedAsFat) {
    std::vector<std::uint8_t> swapped = {0xBE, 0xBA, 0xFE, 0xCA, 0x00};
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(swapped, "swapped.bin", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnknownFormat)
        << "BE BA FE CA is FAT_CIGAM (a host-order artifact), not a "
           "universal binary — accepting it as fat would hand the "
           "operator a `lipo -thin` instruction that cannot work";
}

// Diagnostic honesty: the UnknownFormat message enumerates what IS
// recognised, and `ar` archives ARE (FormatGuess::Ar → readAr). Omitting
// them told an operator holding a `.a` that static archives are not
// understood, which is false.
TEST(BinaryReader, UnknownFormatMessageEnumeratesEveryRecognisedMagic) {
    std::vector<std::uint8_t> garbage = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(garbage, "garbage.bin", rep);
    ASSERT_FALSE(r.has_value());
    auto const& d = r.error().detail;
    EXPECT_NE(d.find("\\x7FELF"), std::string::npos) << d;
    EXPECT_NE(d.find("MZ"), std::string::npos) << d;
    EXPECT_NE(d.find("!<arch>"), std::string::npos)
        << "`ar` archives dispatch to readAr — the recognised list must "
           "say so: " << d;
    EXPECT_NE(d.find("0xFEEDFACF"), std::string::npos) << d;
    EXPECT_NE(d.find("0xCAFEBABE"), std::string::npos) << d;
    EXPECT_NE(d.find("0xCAFEBABF"), std::string::npos)
        << "FAT_MAGIC_64 is recognised too (fat_arch_64 entries): " << d;
}

TEST(BinaryReader, Macho32MagicDispatchesToUnsupportedFormat) {
    // 0xFEEDFACE LE — 32-bit Mach-O (mach_header, not mach_header_64).
    std::vector<std::uint8_t> macho32 = {0xCE, 0xFA, 0xED, 0xFE, 0x00};
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(macho32, "fake.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::UnsupportedFormat);
    EXPECT_NE(r.error().detail.find("32-bit Mach-O"), std::string::npos);
    EXPECT_NE(r.error().detail.find("D-FF1-MACHO-32"), std::string::npos);
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
    EXPECT_GE(countCode(rep, DiagnosticCode::F_FileOpenFailed), 1u);
}

// Post-fold #1: pin that every failure path emits through the
// reporter (not just returns the BinaryReadError). The CLI's
// --suppress=<code> policy needs the diagnostic to actually reach
// the reporter to fire.
TEST(BinaryReaderFile, EmptyBytesEmitsFFileEmptyThroughReporter) {
    DiagnosticReporter rep;
    auto r = readImportsFromBytes({}, "empty.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countCode(rep, DiagnosticCode::F_FileEmpty), 1u);
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
    EXPECT_GE(countCode(rep, DiagnosticCode::F_UnknownBinaryFormat), 1u);
}

TEST(BinaryReaderReporter, PeMagicAlsoEmitsFCodeThroughReporter) {
    // FF1-PE landed 2026-06-01. A 4-byte PE-magic buffer dispatches
    // to readPe() which fails loud at the "file shorter than DOS
    // header" guard → emits F_CorruptedBinary, not F_UnsupportedBinaryFormat.
    DiagnosticReporter rep;
    std::vector<std::uint8_t> pe = {'M', 'Z', 0x00, 0x00};
    auto r = readImportsFromBytes(pe, "fake.dll", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countCode(rep, DiagnosticCode::F_CorruptedBinary), 1u);
}

TEST(BinaryReaderReporter, Elf32AlsoEmitsFCodeThroughReporter) {
    std::vector<std::uint8_t> bytes(64, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 1; bytes[5] = 1;  // ELFCLASS32
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "32bit.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countCode(rep, DiagnosticCode::F_UnsupportedElfClass), 1u);
}

// D-FF1-PARTIAL-CORRUPTION-LOUD pins (2026-06-01):
// pin that F_BinaryReaderPartialCorruption fires when a reader silently
// skips structurally-corrupt entries — the Warning is the only signal the
// operator gets about partial-loss surfaces. A regression that dropped
// the counter or short-circuited the emission would now fail loud.

TEST(BinaryReaderReporter,
     Elf64PartialCorruptionFiresWhenStNameIndexIsOutOfRange) {
    // Build an ELF with one valid symbol "good" + one symbol claiming
    // st_name = a huge index into .dynstr. readNulTerminated returns
    // empty for the out-of-range index → corruptedNameSkips=1 →
    // F_BinaryReaderPartialCorruption Warning fires; the valid symbol
    // is still surfaced.
    std::vector<Sym> syms;
    syms.push_back(Sym{0, info(1, 2), 0, 1, 0x1000, 0});  // valid, name = nameOffsets[0]
    syms.push_back(Sym{0xFFFFFFFFu, info(1, 2), 0, 1, 0x2000, 0});
        // st_name overridden by buildMinimalElf64 only for the FIRST N
        // syms (`if (i < nameOffsets.size())`). With names={"good"} the
        // second sym's st_name=0xFFFFFFFF survives — points past .dynstr.
    auto const bytes = buildMinimalElf64(syms, {"good"});
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "libtest.so", rep);
    ASSERT_TRUE(r.has_value())
        << "partial corruption must NOT abort — valid rows still surface";
    EXPECT_EQ(r->size(), 1u) << "only 'good' should be surfaced";
    EXPECT_EQ((*r)[0].mangledName, "good");
    EXPECT_GE(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption),
              1u)
        << "the silent-skip on a non-zero st_name with empty resolved "
           "name must fire F_BinaryReaderPartialCorruption Warning";
    // eb2c6c7 audit fold (test-analyzer Finding D): tighten the
    // negative WAE branch. Default config has `warningsAsErrors=false`
    // — the diagnostic MUST land at severity Warning and MUST NOT
    // increment errorCount(). A regression that unconditionally
    // elevates Warning→Error in `applyPolicy` would pass the
    // count-based check above but fail these two pins.
    bool sawPartialCorruption = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning)
                << "partial-corruption MUST remain Warning under default "
                   "config (warningsAsErrors=false)";
            sawPartialCorruption = true;
        }
    }
    EXPECT_TRUE(sawPartialCorruption);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "partial-corruption Warning MUST NOT bump errorCount() under "
           "default config";
}

TEST(BinaryReaderReporter,
     Elf64PartialCorruptionElevatesToErrorUnderWarningsAsErrors) {
    // D-FF1-PARTIAL-CORRUPTION-WAE-PIN (2026-06-01): pin that
    // --warnings-as-errors elevates F_BinaryReaderPartialCorruption
    // from Warning to Error end-to-end. The unsuppressable gate
    // bypasses --suppress + overrides (silencing) but NOT
    // warningsAsErrors (elevation), so strict-mode operators get
    // fail-loud exit code on partial-corruption signals.
    std::vector<Sym> syms;
    syms.push_back(Sym{0, info(1, 2), 0, 1, 0x1000, 0});
    syms.push_back(Sym{0xFFFFFFFFu, info(1, 2), 0, 1, 0x2000, 0});
    auto const bytes = buildMinimalElf64(syms, {"good"});

    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter rep{cfg};
    auto r = readImportsFromBytes(bytes, "libtest.so", rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 1u);
    bool sawElevated = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            sawElevated = true;
            EXPECT_EQ(d.severity, DiagnosticSeverity::Error)
                << "warningsAsErrors must elevate F_BinaryReader"
                   "PartialCorruption to Error";
            break;
        }
    }
    EXPECT_TRUE(sawElevated);
    EXPECT_GT(rep.errorCount(), 0u)
        << "elevated diagnostic must increment errorCount so the "
           "driver's exit-code gate fires under strict mode";
}

TEST(BinaryReaderReporter,
     Elf64NoPartialCorruptionWarningForByDesignSkips) {
    // Negative pin: by-design unnamed entries (st_name=0) + local-bind
    // entries (STB_LOCAL=0) must NOT count as partial corruption.
    std::vector<Sym> syms;
    // Local-bind: STB_LOCAL=0, STT_FUNC=2 → info = 0|2 = 2
    syms.push_back(Sym{0, info(0, 2), 0, 1, 0x1000, 0});
    // Section-symbol-style unnamed: st_name=0, STB_GLOBAL=1, STT_NOTYPE=0
    syms.push_back(Sym{0, info(1, 0), 0, 1, 0x2000, 0});
    auto const bytes = buildMinimalElf64(syms, {});
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "libtest.so", rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 0u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption),
              0u)
        << "by-design unnamed + local-bind skips must NOT trigger the "
           "partial-corruption Warning — those are structural filters, "
           "not corruption";
}

TEST(BinaryReaderReporter, CorruptedBinaryAlsoEmitsFCodeThroughReporter) {
    std::vector<std::uint8_t> bytes(32, 0);
    bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
    bytes[4] = 2; bytes[5] = 1;
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny.so", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_GE(countCode(rep, DiagnosticCode::F_CorruptedBinary), 1u);
}

// ── TF-C124: per-symbol EXPORT versions, read from a REAL library ────────
// D-FFI-BINARY-READER-SURFACES-NO-SYMBOL-VERSION.
//
// ★ WHY THESE READ A FILE INSTEAD OF SYNTHESIZING BYTES LIKE EVERY TEST
// ABOVE. The rest of this suite hand-assembles ELF images, which is the
// right instrument for "does the parser reject a truncated section table" —
// the shapes are ones no linker would ever emit, so no linker can supply
// them. Symbol versioning is the opposite case: the ONLY thing worth
// proving is that our decode agrees with what a REAL linker really writes,
// and a hand-built versym/verdef pair proves only that this file's encoder
// agrees with this file's decoder. Both could be wrong about bit 15, about
// whether index 1 names a version, about `vd_aux` being relative to the
// Verdef rather than the section — and the suite would stay green.
//
// ⚠ THE FIXTURE IS A COMMITTED BINARY, AND IT IS SELF-WITNESSING. No
// `.gitattributes` glob names it, so it rides git's own binary detection
// (`git ls-files --eol` reports `w/-text`; ELF's e_ident NULs make the
// detection unambiguous). Should a future broad `tests/** text eol=lf` rule
// ever capture it — the exact accident `examples/**/*.bin binary` had to be
// added to undo — the CR injection corrupts the section table and these
// tests go RED at `readImportsFromBytes`, loudly, on the first fresh clone.
// A latent corruption that announces itself is the property to keep; if you
// are pinning globs anyway, add `tests/ffi/data/** binary`.
//
// `tests/ffi/data/libdssver.so.1` is therefore GNU ld 2.42's own output
// (`gcc -shared -Wl,--version-script=`; the exact source and version script
// are committed beside it as `libdssver.source.c` + `libdssver.map`). It
// carries, in one image, all four shapes this feature must tell apart:
//   dss_ver_default@@DSSVER_2.0   default-versioned function
//   dss_ver_compat@DSSVER_1.0     NON-default compat  ┐ two definitions of
//   dss_ver_compat@@DSSVER_2.0    the default         ┘ ONE name — the
//                                                       glibc realpath shape
//   dss_ver_plain                 unversioned export (versym VER_NDX_GLOBAL)
//   dss_ver_data@@DSSVER_2.0      versioned DATA object, not a function
// `readelf -V` on it prints `2h(DSSVER_1.0)` for the compat row — the `h`
// IS the VERSYM_HIDDEN bit this code reads, produced by ld, not by us.
//
// ★ AND IT DELIBERATELY IMPORTS A VERSIONED SYMBOL TOO. The fixture calls
// glibc `realpath`, so ld emits a `.gnu.version_r` alongside the verdef —
// and ld numbers vernaux entries in the SAME index space it just used for
// verdef: verdef holds 1..3 (base, DSSVER_1.0, DSSVER_2.0) and verneed
// continues at 4 (GLIBC_2.3) and 5 (GLIBC_2.2.5), which verdef defines
// NEITHER of. A reader that resolved every versym slot through the verdef
// table without first discarding SHN_UNDEF rows would therefore hit indices
// that resolve to nothing — so `RealLibraryDefaultVersionedFunctionExport`
// asserting ZERO partial-corruption warnings is not decoration, it is the
// pin that those two undefined rows never reach the version lookup.

namespace {

[[nodiscard]] std::vector<std::uint8_t> readFixtureBytes(
    std::filesystem::path const& rel) {
    auto const path = dss::test::repoRoot() / rel;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                      std::istreambuf_iterator<char>());
}

[[nodiscard]] std::vector<std::uint8_t> readVersionedFixture() {
    return readFixtureBytes(
        std::filesystem::path{"tests"} / "ffi" / "data" / "libdssver.so.1");
}

[[nodiscard]] ImportSurface const* findRow(
    std::vector<ImportSurface> const& rows, std::string_view name) {
    for (auto const& r : rows) if (r.mangledName == name) return &r;
    return nullptr;
}

} // namespace

TEST(BinaryReaderElfSymbolVersion, RealLibraryDefaultVersionedFunctionExport) {
    auto const bytes = readVersionedFixture();
    ASSERT_FALSE(bytes.empty()) << "tests/ffi/data/libdssver.so.1 is missing "
                                  "or empty — this suite's only real-linker "
                                  "oracle cannot be substituted";
    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libdssver.so.1", rep);
    ASSERT_TRUE(rows.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption), 0u)
        << "a well-formed GNU ld image must produce no corruption warning — "
           "and specifically, this image's two VERSIONED UNDEFINED rows "
           "(realpath@GLIBC_2.3, __cxa_finalize@GLIBC_2.2.5) carry versym "
           "indices 4 and 5 that live in the VERNEED table, which the verdef "
           "chain does not define. If they ever reached the version lookup "
           "each would be counted as a dangling index and reported here";
    // Same fact, asserted positively: the library's own IMPORTS are not
    // exports, so nothing named realpath is in this surface at all.
    EXPECT_EQ(findRow(*rows, "realpath"), nullptr);

    auto const* row = findRow(*rows, "dss_ver_default");
    ASSERT_NE(row, nullptr) << "the reader did not surface dss_ver_default at "
                               "all — its `@@DSSVER_2.0` suffix must NOT leak "
                               "into mangledName";
    ASSERT_TRUE(row->elfSymbolVersion.has_value());
    EXPECT_EQ(row->elfSymbolVersion->name, "DSSVER_2.0");
    EXPECT_TRUE(row->elfSymbolVersion->isDefaultVersion)
        << "`sym@@VER` is the DEFAULT definition (VERSYM_HIDDEN clear)";
}

TEST(BinaryReaderElfSymbolVersion, RealLibraryCompatAndDefaultOfOneNameDiffer) {
    auto const bytes = readVersionedFixture();
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libdssver.so.1", rep);
    ASSERT_TRUE(rows.has_value());

    // ONE name, TWO definitions — the shape that makes `isDefaultVersion`
    // load-bearing rather than decorative. Both rows must surface, and they
    // must be distinguishable, because a consumer that cannot tell them
    // apart will sooner or later request the compat one.
    std::vector<std::pair<std::string, bool>> compat;
    for (auto const& r : *rows) {
        if (r.mangledName != "dss_ver_compat") continue;
        ASSERT_TRUE(r.elfSymbolVersion.has_value());
        compat.emplace_back(r.elfSymbolVersion->name,
                            r.elfSymbolVersion->isDefaultVersion);
    }
    ASSERT_EQ(compat.size(), 2u)
        << "libdssver.so.1 defines dss_ver_compat at BOTH DSSVER_1.0 and "
           "DSSVER_2.0; the reader must surface both rows";
    std::sort(compat.begin(), compat.end());
    EXPECT_EQ(compat[0].first, "DSSVER_1.0");
    EXPECT_FALSE(compat[0].second)
        << "`sym@VER` is the NON-default compat definition — ld marks it with "
           "the VERSYM_HIDDEN bit (readelf prints `2h`), and mistaking it for "
           "the default is exactly the realpath@GLIBC_2.2.5 misbind";
    EXPECT_EQ(compat[1].first, "DSSVER_2.0");
    EXPECT_TRUE(compat[1].second);
}

TEST(BinaryReaderElfSymbolVersion, RealLibraryUnversionedExportCarriesNoVersion) {
    auto const bytes = readVersionedFixture();
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libdssver.so.1", rep);
    ASSERT_TRUE(rows.has_value());

    auto const* row = findRow(*rows, "dss_ver_plain");
    ASSERT_NE(row, nullptr);
    EXPECT_FALSE(row->elfSymbolVersion.has_value())
        << "this export's versym slot is VER_NDX_GLOBAL (1), which names the "
           "verdef BASE entry — the FILE's own soname. Recording that as a "
           "version would make every unversioned export look versioned at "
           "`libdssver.so.1`, and the emitted verneed would demand a version "
           "no library defines";
}

TEST(BinaryReaderElfSymbolVersion, RealLibraryVersionedDataObjectIsVersionedToo) {
    auto const bytes = readVersionedFixture();
    ASSERT_FALSE(bytes.empty());
    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libdssver.so.1", rep);
    ASSERT_TRUE(rows.has_value());

    // Versioning is a property of the SYMBOL, not of code: a DATA export
    // carries a versym slot exactly like a function, and a data import binds
    // through the same verneed.
    auto const* row = findRow(*rows, "dss_ver_data");
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->kind, SymbolKind::Object);
    ASSERT_TRUE(row->elfSymbolVersion.has_value());
    EXPECT_EQ(row->elfSymbolVersion->name, "DSSVER_2.0");
}

TEST(BinaryReaderElfSymbolVersion, SynthesizedLibraryWithNoVersionSectionsIsSilent) {
    // The complement of the fixture: an image with NO `.gnu.version` at all
    // (the overwhelmingly common case) must leave every row unversioned and
    // must NOT report an anomaly. This one IS synthesized on purpose — it is
    // an assertion about ABSENT sections, which no linker can be asked for.
    std::vector<Sym> syms;
    syms.push_back(Sym{0, info(1 /*STB_GLOBAL*/, 2 /*STT_FUNC*/), 0, 1, 0x1000, 0});
    std::vector<std::string> const names = {"unversioned_fn"};
    auto const bytes = buildMinimalElf64(syms, names);
    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libplain.so", rep);
    ASSERT_TRUE(rows.has_value());
    ASSERT_EQ(rows->size(), 1u);
    EXPECT_FALSE((*rows)[0].elfSymbolVersion.has_value());
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption), 0u);
}

TEST(BinaryReaderElfSymbolVersion, VersionSectionsResolveInEitherHeaderOrder) {
    // GNU ld emits `.gnu.version` BEFORE `.gnu.version_d` (this host's
    // libz.so.1: sections 6 then 7; the fixture: 4 then 5), and a reader that
    // stops scanning once it has found the verdef therefore works — right up
    // until a linker orders them the other way, at which point every version
    // in the image vanishes with nothing reporting it. Section ORDER is a
    // linker convention, not a gABI guarantee.
    //
    // Rather than hand-author an ELF in the reversed order (which would put
    // this file's idea of the layout under test instead of the reader's),
    // take the REAL fixture and swap the two 64-byte SECTION HEADER records.
    // Nothing else moves: section CONTENT stays where it is, and no `sh_link`
    // anywhere in this image names either of the two swapped indices, so the
    // result is the same library described in the other order.
    auto bytes = readVersionedFixture();
    ASSERT_FALSE(bytes.empty());
    std::uint64_t const shoff = static_cast<std::uint64_t>(bytes[40])
        | (static_cast<std::uint64_t>(bytes[41]) << 8)
        | (static_cast<std::uint64_t>(bytes[42]) << 16)
        | (static_cast<std::uint64_t>(bytes[43]) << 24);
    std::uint16_t const shnum =
        static_cast<std::uint16_t>(bytes[60] | (bytes[61] << 8));
    auto sectionType = [&](std::uint16_t i) {
        std::size_t const o = static_cast<std::size_t>(shoff) + i * 64u + 4u;
        return static_cast<std::uint32_t>(bytes[o]) | (bytes[o + 1] << 8)
             | (bytes[o + 2] << 16) | (bytes[o + 3] << 24);
    };
    int versymIdx = -1, verdefIdx = -1;
    for (std::uint16_t i = 0; i < shnum; ++i) {
        if (sectionType(i) == 0x6fffffffu) versymIdx = i;   // SHT_GNU_versym
        if (sectionType(i) == 0x6ffffffdu) verdefIdx = i;   // SHT_GNU_verdef
    }
    ASSERT_GE(versymIdx, 0);
    ASSERT_GE(verdefIdx, 0);
    ASSERT_LT(versymIdx, verdefIdx)
        << "the fixture is expected to carry ld's usual order; if this ever "
           "flips, the swap below stops being the reversal it claims to be";
    std::array<std::uint8_t, 64> tmp{};
    std::size_t const a = static_cast<std::size_t>(shoff) + versymIdx * 64u;
    std::size_t const b = static_cast<std::size_t>(shoff) + verdefIdx * 64u;
    std::memcpy(tmp.data(), bytes.data() + a, 64);
    std::memcpy(bytes.data() + a, bytes.data() + b, 64);
    std::memcpy(bytes.data() + b, tmp.data(), 64);

    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libdssver.so.1", rep);
    ASSERT_TRUE(rows.has_value());
    auto const* row = findRow(*rows, "dss_ver_default");
    ASSERT_NE(row, nullptr);
    ASSERT_TRUE(row->elfSymbolVersion.has_value())
        << "with `.gnu.version_d` ahead of `.gnu.version` in the section "
           "header table, the version decode must still find both";
    EXPECT_EQ(row->elfSymbolVersion->name, "DSSVER_2.0");
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption), 0u);
}

TEST(BinaryReaderElfSymbolVersion, RealSystemZlibDeflateBoundIsVersioned) {
    // The third-party oracle the anchor was written from: a library nobody
    // in this repository authored, built by a distribution, with a version
    // script that versions SOME of its exports and not others. MEASURED on
    // this workstation's WSL (zlib 1.3, glibc 2.39): `deflateBound` is
    // `@@ZLIB_1.2.0` while `deflate` and `compress2` are unversioned — a
    // fact about libz's own `zlib.map`, not a defect anywhere.
    //
    // Host-conditional by necessity (there is no `.so` on a Windows host)
    // and therefore a SUPPLEMENT to the committed fixture above, never a
    // substitute for it: the four pins that matter run on every leg.
    std::filesystem::path const libz{
        "/usr/lib/x86_64-linux-gnu/libz.so.1"};
    std::error_code ec;
    if (!std::filesystem::exists(libz, ec)) {
        GTEST_SKIP() << "no " << libz.string() << " on this host — the "
                        "committed libdssver.so.1 pins are the ones that "
                        "must hold everywhere; this arm adds a third-party "
                        "library on the Linux legs";
    }
    std::ifstream in(libz, std::ios::binary);
    ASSERT_TRUE(in.good());
    std::vector<std::uint8_t> const bytes(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty());

    DiagnosticReporter rep;
    auto rows = readImportsFromBytes(bytes, "libz.so.1", rep);
    ASSERT_TRUE(rows.has_value());

    auto const* bound = findRow(*rows, "deflateBound");
    ASSERT_NE(bound, nullptr);
    ASSERT_TRUE(bound->elfSymbolVersion.has_value());
    EXPECT_EQ(bound->elfSymbolVersion->name, "ZLIB_1.2.0");
    EXPECT_TRUE(bound->elfSymbolVersion->isDefaultVersion);

    auto const* plain = findRow(*rows, "deflate");
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->elfSymbolVersion.has_value())
        << "libz versions deflateBound but not deflate; a reader that made "
           "both look alike would be inventing one of the two";
}

// (Former `Ff1ProducedRowsHaveNoCSignature` test removed at FF2
// post-#2 type-design fold: `cSignature` field dropped from
// `ImportSurface` since no producer or consumer needed it.
// Anchored D-FF2-1: re-add `optional<FnSigTypeId>` only if FF3 needs
// to attach the resolved sig to the row instead of the HIR node.)

