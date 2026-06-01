// Plan 11 FF1-MachO tests — `dss::ffi::readImportsFromBytes` on Mach-O binaries.
//
// Pins:
//   * Mach-O 64-bit symbol-table round-trip from synthesized binaries.
//   * Load-command walk: LC_SYMTAB located + parsed; LC_DYSYMTAB
//     optional filter slice (iextdefsym/nextdefsym).
//   * Symbol filtering: stab entries (N_STAB) skipped; non-external
//     (N_EXT==0) skipped on the fallback path; undefined (N_UNDF)
//     skipped on the fallback path.
//   * n_type → SymbolVisibility mapping (N_PEXT → Hidden, else Default).
//   * Partial-corruption Warning: counter aggregates name-skip cases
//     into one F_BinaryReaderPartialCorruption Warning at end-of-parse
//     (D-FF1-PARTIAL-CORRUPTION-MACHO anchor close).
//   * --warnings-as-errors elevation pin (mirrors ELF + PE).
//   * Failure modes: truncated header, missing LC_SYMTAB, symtab past EOF,
//     strtab past EOF, malformed LC_DYSYMTAB slice, LC body past EOF.
//
// Test strategy: synthesize minimal Mach-O 64-bit binaries directly
// in C++ via the byte-emit helpers + read them back. Avoids shipping
// real .dylib fixtures (CI hermeticity); matches the ELF + PE test
// pattern. 3rd byte-emit consumer triggers D-FF1-TEST-BYTE-EMIT
// closure (hoisted helpers in tests/test_support/byte_emit.hpp).

#include "core/types/diagnostic_reporter.hpp"
#include "ffi/binary_reader.hpp"
#include "byte_emit.hpp"
#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;
using dss::test_support::appU32;
using dss::test_support::appU64;
using dss::test_support::countCode;
using dss::test_support::putU32;
using dss::test_support::putU64;

namespace {

// nlist_64 packed: [n_strx u32][n_type u8][n_sect u8][n_desc u16][n_value u64]
struct Nlist {
    std::uint32_t n_strx;
    std::uint8_t  n_type;
    std::uint8_t  n_sect;
    std::uint16_t n_desc;
    std::uint64_t n_value;
};

constexpr std::uint32_t kMachOMagic64 = 0xFEEDFACFu;
constexpr std::uint32_t kLcSymtab     = 0x2u;
constexpr std::uint32_t kLcDysymtab   = 0xBu;
constexpr std::uint32_t kLcSegment64  = 0x19u;  // unused-but-walked filler LC for tests

// n_type encodings used by the tests:
constexpr std::uint8_t kNExtBit    = 0x01u;
constexpr std::uint8_t kNTypeSect  = 0x0Eu;        // N_TYPE == N_SECT
constexpr std::uint8_t kNTypeUndf  = 0x00u;        // N_TYPE == N_UNDF
constexpr std::uint8_t kNPextBit   = 0x10u;
constexpr std::uint8_t kNStabFun   = 0x24u;        // N_FUN stab (high bit of stab range)

// Build a minimal Mach-O 64-bit binary with optional LC_DYSYMTAB.
//
// Layout in execution order:
//   [0..31]    mach_header_64 (32 bytes)
//   [32..N1]   load commands:
//                LC_SYMTAB (24 bytes)
//                LC_DYSYMTAB (80 bytes) — IF includeDysymtab
//   [N1..N2]   symbol table — `syms.size() × 16 bytes`
//   [N2..]     string table — NUL-sentinel + packed NUL-terminated names
//
// Each `n_strx` field is rewritten to point at the corresponding
// `nameOffsets[i]` (within the layout-built string table). Pass
// `n_strx` rewrite semantic (post-4ca5bff audit fold):
//   * Nlist.n_strx == 0  → rewrite to `nameOffsets[i]` (default layout).
//   * Nlist.n_strx != 0  → honor caller's value verbatim (poison cases
//                          like 0xFFFFFFFF for partial-corruption tests).
// The prior triple-arm ternary with `kStrxLeaveAsIs` sentinel was
// removed — there was no test that used the sentinel, the dead branch
// added confusion, and a future test that adds a 2nd name without
// noticing the rewrite would silently regress (4ca5bff audit:
// code-reviewer M1 + test-analyzer #5 + simplifier #2 converged).

std::vector<std::uint8_t>
buildMinimalMacho64(std::vector<Nlist> syms,
                    std::vector<std::string> const& names,
                    bool includeDysymtab     = false,
                    std::uint32_t iextdefsym = 0,
                    std::uint32_t nextdefsym = 0) {
    constexpr std::size_t kHeaderSize  = 32;
    constexpr std::size_t kLcSymtabSz  = 24;
    constexpr std::size_t kLcDysymtabSz = 80;
    constexpr std::size_t kNlist64Sz   = 16;

    // String table layout: leading NUL sentinel, then names packed.
    std::vector<std::uint8_t> strTab;
    strTab.push_back(0);
    std::vector<std::uint32_t> nameOffsets;
    nameOffsets.reserve(names.size());
    for (auto const& n : names) {
        nameOffsets.push_back(static_cast<std::uint32_t>(strTab.size()));
        for (char c : n) strTab.push_back(static_cast<std::uint8_t>(c));
        strTab.push_back(0);
    }

    std::size_t const lcCount   = includeDysymtab ? 2 : 1;
    std::size_t const sizeofcmds = (includeDysymtab ? kLcSymtabSz + kLcDysymtabSz
                                                    : kLcSymtabSz);
    std::size_t const symtabFileOff = kHeaderSize + sizeofcmds;
    std::size_t const strtabFileOff = symtabFileOff + syms.size() * kNlist64Sz;
    std::size_t const totalSize     = strtabFileOff + strTab.size();

    std::vector<std::uint8_t> b(totalSize, 0);

    // ── mach_header_64 ──
    putU32(b, 0,  kMachOMagic64);
    putU32(b, 4,  0x01000007u);                                 // cputype = CPU_TYPE_X86_64
    putU32(b, 8,  0x00000003u);                                 // cpusubtype = CPU_SUBTYPE_X86_64_ALL
    putU32(b, 12, 0x00000006u);                                 // filetype = MH_DYLIB
    putU32(b, 16, static_cast<std::uint32_t>(lcCount));         // ncmds
    putU32(b, 20, static_cast<std::uint32_t>(sizeofcmds));      // sizeofcmds
    putU32(b, 24, 0u);                                          // flags
    putU32(b, 28, 0u);                                          // reserved

    // ── LC_SYMTAB ──
    std::size_t lcOff = kHeaderSize;
    putU32(b, lcOff + 0,  kLcSymtab);
    putU32(b, lcOff + 4,  static_cast<std::uint32_t>(kLcSymtabSz));
    putU32(b, lcOff + 8,  static_cast<std::uint32_t>(symtabFileOff));
    putU32(b, lcOff + 12, static_cast<std::uint32_t>(syms.size()));
    putU32(b, lcOff + 16, static_cast<std::uint32_t>(strtabFileOff));
    putU32(b, lcOff + 20, static_cast<std::uint32_t>(strTab.size()));
    lcOff += kLcSymtabSz;

    if (includeDysymtab) {
        // ── LC_DYSYMTAB ── (only iextdefsym/nextdefsym are read v1)
        putU32(b, lcOff + 0,  kLcDysymtab);
        putU32(b, lcOff + 4,  static_cast<std::uint32_t>(kLcDysymtabSz));
        putU32(b, lcOff + 8,  0u);                              // ilocalsym
        putU32(b, lcOff + 12, 0u);                              // nlocalsym
        putU32(b, lcOff + 16, iextdefsym);
        putU32(b, lcOff + 20, nextdefsym);
        // remaining fields zero
    }

    // ── Symbol table ──
    for (std::size_t i = 0; i < syms.size(); ++i) {
        std::size_t const off = symtabFileOff + i * kNlist64Sz;
        // n_strx==0 → rewrite to layout-built nameOffsets[i]; non-zero
        // honors the caller-supplied value (poison-tests). See helper
        // docblock above for the rewrite contract.
        std::uint32_t const writtenStrx = (syms[i].n_strx == 0u)
            ? (i < nameOffsets.size() ? nameOffsets[i] : 0u)
            : syms[i].n_strx;
        putU32(b, off + 0,  writtenStrx);
        b[off + 4] = syms[i].n_type;
        b[off + 5] = syms[i].n_sect;
        b[off + 6] = static_cast<std::uint8_t>(syms[i].n_desc & 0xFF);
        b[off + 7] = static_cast<std::uint8_t>((syms[i].n_desc >> 8) & 0xFF);
        putU64(b, off + 8, syms[i].n_value);
    }

    // ── String table ──
    for (std::size_t i = 0; i < strTab.size(); ++i) {
        b[strtabFileOff + i] = strTab[i];
    }

    return b;
}

// Convenience: defined-external function symbol (N_TYPE=SECT, N_EXT).
Nlist sect(std::uint8_t sect = 1, std::uint8_t pext = 0) {
    return Nlist{0, static_cast<std::uint8_t>(kNTypeSect | kNExtBit | pext),
                 sect, 0, 0x1000};
}

} // namespace

TEST(BinaryReaderMacho, RoundTripsLcSymtabExternalSymbols) {
    std::vector<Nlist> syms{sect(1), sect(1), sect(1)};
    std::vector<std::string> names{"alpha", "beta", "gamma"};
    auto const bytes = buildMinimalMacho64(syms, names);

    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "libtest.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "kind=" << binaryReadErrorKindName(r.error().kind)
        << " detail=" << r.error().detail;
    ASSERT_EQ(r->size(), 3u);
    EXPECT_EQ((*r)[0].mangledName, "alpha");
    EXPECT_EQ((*r)[1].mangledName, "beta");
    EXPECT_EQ((*r)[2].mangledName, "gamma");
    EXPECT_EQ((*r)[0].libraryPath, "libtest.dylib");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    EXPECT_EQ((*r)[0].visibility, SymbolVisibility::Default);
    EXPECT_EQ((*r)[0].linkage, SymbolLinkage::External);
}

TEST(BinaryReaderMacho, NPextBitMapsToHiddenVisibility) {
    // One symbol with N_PEXT set (private extern → Hidden), one default.
    std::vector<Nlist> syms{sect(1, kNPextBit), sect(1)};
    std::vector<std::string> names{"private_ext", "public_def"};
    auto const bytes = buildMinimalMacho64(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0].mangledName, "private_ext");
    EXPECT_EQ((*r)[0].visibility, SymbolVisibility::Hidden);
    EXPECT_EQ((*r)[1].visibility, SymbolVisibility::Default);
}

TEST(BinaryReaderMacho, SkipsStabEntries) {
    // 1st row is a stab entry (n_type=N_FUN=0x24, all stab bits set);
    // 2nd is a normal external sect-defined symbol. Output must
    // contain only the second.
    std::vector<Nlist> syms{
        Nlist{0, kNStabFun, 1, 0, 0x1000},
        sect(1),
    };
    std::vector<std::string> names{"debug_stab", "real_export"};
    auto const bytes = buildMinimalMacho64(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "real_export");
}

TEST(BinaryReaderMacho, FallbackPathSkipsNonExternalSymbols) {
    // n_type=N_SECT (defined) but N_EXT bit clear — not exported.
    // Without LC_DYSYMTAB the fallback walk filters via N_EXT.
    std::vector<Nlist> syms{
        Nlist{0, kNTypeSect, 1, 0, 0x1000},                    // internal-only
        sect(1),                                                // external — keep
    };
    std::vector<std::string> names{"internal_only", "exported"};
    auto const bytes = buildMinimalMacho64(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "exported");
}

TEST(BinaryReaderMacho, FallbackPathSkipsUndefinedImports) {
    // N_EXT set but N_TYPE == N_UNDF — undefined imports, not exports.
    std::vector<Nlist> syms{
        Nlist{0, static_cast<std::uint8_t>(kNTypeUndf | kNExtBit), 0, 0, 0},
        sect(1),
    };
    std::vector<std::string> names{"_imported_dep", "_local_export"};
    auto const bytes = buildMinimalMacho64(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "_local_export");
}

TEST(BinaryReaderMacho, LcDysymtabSliceFiltersExternalDefs) {
    // 4 rows: [0]=internal, [1]=exported A, [2]=exported B, [3]=trailing.
    // LC_DYSYMTAB.iextdefsym=1, nextdefsym=2 — only [1] and [2] surface.
    // Note: with LC_DYSYMTAB present the reader bypasses the N_EXT
    // filter and trusts the slice (mirrors real linker behavior).
    std::vector<Nlist> syms{
        Nlist{0, kNTypeSect, 1, 0, 0x1000},                    // [0] internal
        sect(1),                                                // [1] external A
        sect(1),                                                // [2] external B
        Nlist{0, kNTypeSect, 1, 0, 0x4000},                    // [3] trailing
    };
    std::vector<std::string> names{"internal", "alpha", "beta", "trailing"};
    auto const bytes = buildMinimalMacho64(syms, names,
                                            /*includeDysymtab=*/true,
                                            /*iextdefsym=*/1u,
                                            /*nextdefsym=*/2u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0].mangledName, "alpha");
    EXPECT_EQ((*r)[1].mangledName, "beta");
}

TEST(BinaryReaderMacho, TruncatedHeaderFailsLoud) {
    std::vector<std::uint8_t> bytes(16, 0);                    // < kMachOHeaderSize
    putU32(bytes, 0, kMachOMagic64);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "trunc.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("shorter than mach_header_64"),
              std::string::npos);
}

TEST(BinaryReaderMacho, NoLcSymtabFailsLoud) {
    // Valid header with ncmds=0 → no LC_SYMTAB → SectionNotFound.
    std::vector<std::uint8_t> bytes(32, 0);
    putU32(bytes, 0,  kMachOMagic64);
    putU32(bytes, 16, 0u);                                     // ncmds = 0
    putU32(bytes, 20, 0u);                                     // sizeofcmds = 0
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "stripped.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::SectionNotFound);
    EXPECT_NE(r.error().detail.find("no LC_SYMTAB"), std::string::npos);
}

TEST(BinaryReaderMacho, SymtabRunsPastEofFailsLoud) {
    std::vector<Nlist> syms{sect(1)};
    std::vector<std::string> names{"only"};
    auto bytes = buildMinimalMacho64(syms, names);
    // Corrupt LC_SYMTAB.symoff to point past EOF.
    putU32(bytes, 32 + 8, static_cast<std::uint32_t>(bytes.size() + 4096));
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "corrupt_symoff.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("symbol table"), std::string::npos);
}

TEST(BinaryReaderMacho, StrtabRunsPastEofFailsLoud) {
    std::vector<Nlist> syms{sect(1)};
    std::vector<std::string> names{"only"};
    auto bytes = buildMinimalMacho64(syms, names);
    // Corrupt LC_SYMTAB.stroff to point past EOF.
    putU32(bytes, 32 + 16, static_cast<std::uint32_t>(bytes.size() + 4096));
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "corrupt_stroff.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("string table"), std::string::npos);
}

TEST(BinaryReaderMacho, DysymtabExtdefSliceExceedsNsymsFailsLoud) {
    std::vector<Nlist> syms{sect(1), sect(1)};
    std::vector<std::string> names{"a", "b"};
    auto bytes = buildMinimalMacho64(syms, names,
                                      /*includeDysymtab=*/true,
                                      /*iextdefsym=*/0u,
                                      /*nextdefsym=*/99u);    // 99 > nsyms=2
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "bad_dysym.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("extdef slice"), std::string::npos);
}

TEST(BinaryReaderMacho, PartialCorruptionWarningFiresOnPoisonedStrx) {
    // First symbol is valid; second has n_strx pointing past the
    // string table. readNulTerminated returns empty → corrupted-name
    // skip counter increments; valid row still surfaces.
    std::vector<Nlist> syms{
        sect(1),
        Nlist{0xFFFFFFFFu, static_cast<std::uint8_t>(kNTypeSect | kNExtBit),
              1, 0, 0x2000},
    };
    std::vector<std::string> names{"good"};                    // only 1 name → row [1] n_strx survives as poisoned
    auto const bytes = buildMinimalMacho64(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "partial.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "partial corruption must NOT abort — valid rows still surface";
    EXPECT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "good");
    EXPECT_GE(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption),
              1u)
        << "the silent-skip on a non-zero n_strx with empty resolved name "
           "must fire F_BinaryReaderPartialCorruption Warning";
    // Negative WAE branch: default config (warningsAsErrors=false) —
    // diagnostic MUST be Warning, errorCount==0. Tightening pattern
    // matches ELF + PE.
    bool sawPartialCorruption = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
            sawPartialCorruption = true;
        }
    }
    EXPECT_TRUE(sawPartialCorruption);
    EXPECT_EQ(rep.errorCount(), 0u);
}

TEST(BinaryReaderMacho, PartialCorruptionWarningNegativeByDesignSkips) {
    // Two valid external symbols + one with n_strx=0 (by-design unnamed).
    // n_strx=0 entries are filtered without contributing to the
    // corruption counter — the Warning MUST NOT fire.
    std::vector<Nlist> syms{
        sect(1),
        Nlist{0, static_cast<std::uint8_t>(kNTypeSect | kNExtBit), 1, 0, 0x2000},
        sect(1),
    };
    std::vector<std::string> names{"a", "", "c"};              // middle row n_strx will be 0 (unnamed)
    auto bytes = buildMinimalMacho64(syms, names);
    // Force the middle row's n_strx to 0 (by-design unnamed).
    std::size_t const symtabOff = 32 + 24;                     // header + LC_SYMTAB
    putU32(bytes, symtabOff + 16 + 0, 0u);                     // [1].n_strx = 0
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "clean.dylib", rep);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->size(), 2u);
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption),
              0u)
        << "by-design unnamed (n_strx=0) entries MUST NOT contribute to "
           "the partial-corruption counter";
}

TEST(BinaryReaderMacho, PartialCorruptionElevatesToErrorUnderWarningsAsErrors) {
    // D-FF1-PARTIAL-CORRUPTION-WAE-PIN (Mach-O arm): mirrors the ELF
    // + PE WAE pins. --warnings-as-errors elevates the Mach-O
    // partial-corruption Warning to Error and bumps errorCount.
    std::vector<Nlist> syms{
        sect(1),
        Nlist{0xFFFFFFFFu, static_cast<std::uint8_t>(kNTypeSect | kNExtBit),
              1, 0, 0x2000},
    };
    std::vector<std::string> names{"good"};
    auto const bytes = buildMinimalMacho64(syms, names);

    DiagnosticReporter::Config cfg;
    cfg.policy.warningsAsErrors = true;
    DiagnosticReporter rep{cfg};
    auto r = readImportsFromBytes(bytes, "wae.dylib", rep);
    ASSERT_TRUE(r.has_value());
    bool sawElevated = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            EXPECT_EQ(d.severity, DiagnosticSeverity::Error)
                << "WAE must elevate the partial-corruption Warning to Error";
            sawElevated = true;
        }
    }
    EXPECT_TRUE(sawElevated);
    EXPECT_GT(rep.errorCount(), 0u)
        << "elevated diagnostic must bump errorCount() so downstream "
           "exit-code gates fire correctly";
}

TEST(BinaryReaderMacho, LcCmdsizeBelowPreambleFailsLoud) {
    // Hand-build a Mach-O with one LC whose cmdsize=4 (< 8-byte preamble).
    std::vector<std::uint8_t> bytes(32 + 8, 0);
    putU32(bytes, 0,  kMachOMagic64);
    putU32(bytes, 16, 1u);                                     // ncmds = 1
    putU32(bytes, 20, 8u);                                     // sizeofcmds = 8
    // LC at offset 32 with cmd=LC_SEGMENT_64 + cmdsize=4 (corrupt)
    putU32(bytes, 32, kLcSegment64);
    putU32(bytes, 36, 4u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny_cmdsize.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("smaller than the 8-byte preamble"),
              std::string::npos);
}

// 4ca5bff audit fold (test-analyzer HIGH#1): LC_SYMTAB cmdsize<24
// must fail loud. Without this guard a too-small LC_SYMTAB body
// would let `readU32(bytes, *symtabOff + 8..23)` read across into
// the next LC, pulling whatever happens to follow as
// symoff/nsyms/stroff/strsize — silent corrupted parse.
TEST(BinaryReaderMacho, LcSymtabCmdsizeBelowMinimumFailsLoud) {
    // Hand-build: header + LC_SYMTAB with cmdsize=12 (< 24).
    std::vector<std::uint8_t> bytes(32 + 12, 0);
    putU32(bytes, 0,  kMachOMagic64);
    putU32(bytes, 16, 1u);                                     // ncmds = 1
    putU32(bytes, 20, 12u);                                    // sizeofcmds = 12
    putU32(bytes, 32, kLcSymtab);
    putU32(bytes, 36, 12u);                                    // cmdsize = 12 (< 24)
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny_symtab.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("LC_SYMTAB cmdsize"), std::string::npos);
}

// 4ca5bff audit fold (test-analyzer HIGH#2): LC_DYSYMTAB cmdsize<24
// must fail loud. Without this guard a too-small LC_DYSYMTAB body
// would let `readU32(bytes, *dysymtabOff + 16..23)` read across
// into the next LC, silently using whatever follows as
// iextdefsym/nextdefsym — wrong filter slice applied silently.
TEST(BinaryReaderMacho, LcDysymtabCmdsizeBelowMinimumFailsLoud) {
    // Hand-build: header + LC_DYSYMTAB with cmdsize=12 (< 24).
    std::vector<std::uint8_t> bytes(32 + 12, 0);
    putU32(bytes, 0,  kMachOMagic64);
    putU32(bytes, 16, 1u);                                     // ncmds = 1
    putU32(bytes, 20, 12u);                                    // sizeofcmds = 12
    putU32(bytes, 32, kLcDysymtab);
    putU32(bytes, 36, 12u);                                    // cmdsize = 12 (< 24)
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "tiny_dysymtab.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("LC_DYSYMTAB cmdsize"), std::string::npos);
}

// 4ca5bff audit fold (test-analyzer HIGH#3): LC body past sizeofcmds
// region must fail loud. Without this guard a binary claiming
// ncmds=2 + sizeofcmds=8 (one LC's worth) would iterate past the
// declared LC region into raw data, reading symbol-table bytes as
// LC headers — silent garbage parse.
TEST(BinaryReaderMacho, LcBodyRunsPastSizeofcmdsRegionFailsLoud) {
    // Hand-build: header claims ncmds=2 but sizeofcmds covers just
    // one preamble (8 bytes). The second loop iteration's
    // `lcOff + preamble > lcEnd` check catches the overrun.
    std::vector<std::uint8_t> bytes(32 + 16, 0);
    putU32(bytes, 0,  kMachOMagic64);
    putU32(bytes, 16, 2u);                                     // ncmds = 2 (LIES)
    putU32(bytes, 20, 8u);                                     // sizeofcmds = 8 (only one)
    // LC #1 occupies the full 8-byte sizeofcmds region.
    putU32(bytes, 32, kLcSegment64);
    putU32(bytes, 36, 8u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "overrun_lc.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("preamble runs past sizeofcmds"),
              std::string::npos);
}

// 4ca5bff audit fold (test-analyzer MEDIUM#4): LC_DYSYMTAB present
// with nextdefsym==0 must fall through to the N_EXT walk over the
// full symtab. Documents the "uniform fallthrough" behavior so a
// future regression that flips to "return empty" or "treat as
// full-scan slice" is caught. Real linker behavior: a stripped
// binary may declare LC_DYSYMTAB with all-zero counts; v1 surfaces
// whatever N_EXT entries are still in LC_SYMTAB.
TEST(BinaryReaderMacho, DysymtabWithZeroNextdefsymFallsThroughToNExtWalk) {
    std::vector<Nlist> syms{sect(1), sect(1)};
    std::vector<std::string> names{"a", "b"};
    auto const bytes = buildMinimalMacho64(syms, names,
                                            /*includeDysymtab=*/true,
                                            /*iextdefsym=*/0u,
                                            /*nextdefsym=*/0u);   // ← the case under pin
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "zero_nextdef.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "kind=" << binaryReadErrorKindName(r.error().kind);
    // Fallthrough → N_EXT walk → both rows surface (sect() has N_EXT set).
    ASSERT_EQ(r->size(), 2u)
        << "nextdefsym==0 MUST fall through to N_EXT walk, not "
           "return empty silently — see macho_reader.cpp comment "
           "block at the LC_DYSYMTAB filter branch";
    EXPECT_EQ((*r)[0].mangledName, "a");
    EXPECT_EQ((*r)[1].mangledName, "b");
}

// 4ca5bff audit fold (code-reviewer H1 + silent-failure A3
// convergence): defense-in-depth N_TYPE filter applies even when
// LC_DYSYMTAB present. A malformed/corrupted DYSYMTAB whose extdef
// slice contains N_UNDF entries (undefined imports, n_value=0)
// must NOT surface them as Function/Default exports. Real linkers
// pre-filter; trust-but-verify is cheap.
TEST(BinaryReaderMacho, DysymtabSliceWithUndefinedEntryIsFiltered) {
    // Slice [0, 2) contains: [0] N_UNDF (undefined import), [1] N_SECT.
    // Defense-in-depth: [0] gets filtered, only [1] surfaces.
    std::vector<Nlist> syms{
        Nlist{0, static_cast<std::uint8_t>(kNTypeUndf | kNExtBit), 0, 0, 0},
        sect(1),
    };
    std::vector<std::string> names{"_undef_in_slice", "real_export"};
    auto const bytes = buildMinimalMacho64(syms, names,
                                            /*includeDysymtab=*/true,
                                            /*iextdefsym=*/0u,
                                            /*nextdefsym=*/2u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "did_slice.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u)
        << "defense-in-depth: even with DYSYMTAB declaring 2 extdefs, "
           "the N_UNDF row must be filtered out (a regression that "
           "trusts the slice absolutely would surface garbage)";
    EXPECT_EQ((*r)[0].mangledName, "real_export");
}
