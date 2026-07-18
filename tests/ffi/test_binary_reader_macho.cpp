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
#include <string_view>
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
constexpr std::uint32_t kLcIdDylib    = 0xDu;   // LC_ID_DYLIB (this dylib's install name)
constexpr std::uint32_t kLcSegment64  = 0x19u;  // unused-but-walked filler LC for tests

// n_type encodings used by the tests:
constexpr std::uint8_t kNExtBit    = 0x01u;
constexpr std::uint8_t kNTypeSect  = 0x0Eu;        // N_TYPE == N_SECT
constexpr std::uint8_t kNTypeUndf  = 0x00u;        // N_TYPE == N_UNDF
constexpr std::uint8_t kNPextBit   = 0x10u;
constexpr std::uint8_t kNStabFun   = 0x24u;        // N_FUN stab (high bit of stab range)

// Build a minimal Mach-O 64-bit binary with optional LC_DYSYMTAB and an
// optional LC_ID_DYLIB.
//
// Layout in execution order:
//   [0..31]    mach_header_64 (32 bytes)
//   [32..N1]   load commands:
//                LC_ID_DYLIB (dylib_command) — IF idDylibInstallName non-empty
//                LC_SYMTAB (24 bytes)
//                LC_DYSYMTAB (80 bytes) — IF includeDysymtab
//   [N1..N2]   symbol table — `syms.size() × 16 bytes`
//   [N2..]     string table — NUL-sentinel + packed NUL-terminated names
//
// D-FF1-READER-SONAME (c171): a non-empty `idDylibInstallName` PREPENDS a
// dylib_command (cmd=LC_ID_DYLIB=0xD): [0..3]=cmd, [4..7]=cmdsize (8-aligned,
// covering the name), [8..11]=name.offset (24, past the 6 u32 fields),
// [12..23]=timestamp/current/compat versions (0), then the NUL-terminated
// install name padded to cmdsize. ncmds + sizeofcmds are bumped accordingly.
// Empty (the default) reproduces the EXISTING image byte-for-byte (LC_SYMTAB
// stays at file offset 32), so pre-soname callers + fixed-offset pokes are
// unaffected.
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
                    std::uint32_t nextdefsym = 0,
                    std::string const& idDylibInstallName = {}) {
    constexpr std::size_t kHeaderSize  = 32;
    constexpr std::size_t kLcSymtabSz  = 24;
    constexpr std::size_t kLcDysymtabSz = 80;
    constexpr std::size_t kNlist64Sz   = 16;

    bool const hasIdDylib = !idDylibInstallName.empty();
    // dylib_command: 24-byte header + NUL-terminated install name, 8-aligned.
    std::size_t const idDylibSz = hasIdDylib
        ? ((24u + idDylibInstallName.size() + 1u + 7u) / 8u) * 8u
        : 0u;

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

    std::size_t const lcCount   = (includeDysymtab ? 2 : 1) + (hasIdDylib ? 1 : 0);
    std::size_t const sizeofcmds = idDylibSz
                                 + (includeDysymtab ? kLcSymtabSz + kLcDysymtabSz
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

    // ── LC_ID_DYLIB (prepended when an install name is requested) ──
    std::size_t lcOff = kHeaderSize;
    if (hasIdDylib) {
        putU32(b, lcOff + 0, kLcIdDylib);                            // cmd = 0xD
        putU32(b, lcOff + 4, static_cast<std::uint32_t>(idDylibSz)); // cmdsize
        putU32(b, lcOff + 8, 24u);                                   // name.offset
        // timestamp / current / compat versions (@ +12/+16/+20) left 0
        for (std::size_t k = 0; k < idDylibInstallName.size(); ++k)
            b[lcOff + 24 + k] = static_cast<std::uint8_t>(idDylibInstallName[k]);
        // trailing NUL already 0 (buffer is zero-initialized)
        lcOff += idDylibSz;
    }

    // ── LC_SYMTAB ──
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

// ── D-FF1-READER-SONAME (c171): LC_ID_DYLIB install-name extraction ──

// STRICT: the LC_ID_DYLIB install name surfaces VERBATIM on every row's
// `soname` — the WHOLE "@rpath/..." string, NOT leaf-reduced.
TEST(BinaryReaderMacho, ExtractsInstallNameFromLcIdDylib) {
    std::vector<Nlist> syms{sect(1), sect(1)};
    std::vector<std::string> names{"alpha", "beta"};
    auto const bytes = buildMinimalMacho64(
        syms, names, /*includeDysymtab=*/false,
        /*iextdefsym=*/0u, /*nextdefsym=*/0u,
        /*idDylibInstallName=*/"@rpath/libwidget.dylib");

    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "/build/out/libwidget.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "kind=" << binaryReadErrorKindName(r.error().kind)
        << " detail=" << r.error().detail;
    ASSERT_EQ(r->size(), 2u);
    for (auto const& row : *r) {
        EXPECT_EQ(row.soname, "@rpath/libwidget.dylib")
            << "install name must be kept whole (not leaf-reduced)";
    }
    EXPECT_EQ(rep.errorCount(), 0u);
}

// RED-ON-DISABLE: the EXISTING synthesizer emits no LC_ID_DYLIB, so
// every row's soname MUST be empty. Fails if the extractor ever
// fabricated an install name.
TEST(BinaryReaderMacho, NoLcIdDylibLeavesSonameEmpty) {
    std::vector<Nlist> syms{sect(1), sect(1)};
    std::vector<std::string> names{"alpha", "beta"};
    auto const bytes = buildMinimalMacho64(syms, names);  // no LC_ID_DYLIB
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "libwidget.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "kind=" << binaryReadErrorKindName(r.error().kind);
    ASSERT_EQ(r->size(), 2u);
    for (auto const& row : *r) {
        EXPECT_TRUE(row.soname.empty())
            << "no LC_ID_DYLIB must leave soname empty; got '"
            << row.soname << "'";
    }
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
    // 2060dc8 audit fold (silent-failure M1): mix one external +
    // one INTERNAL row so the test can discriminate a true N_EXT
    // fallthrough from a regression that flips to "full-scan slice
    // (dysymtabFilter=true, walkStart=0, walkEnd=nsyms)" — in the
    // regression case the internal row would surface, but the
    // !dysymtabFilter && !isExternal check in fallthrough mode
    // correctly filters it.
    std::vector<Nlist> syms{
        sect(1),                                                   // [0] external
        Nlist{0, kNTypeSect, 1, 0, 0x2000},                       // [1] internal (no N_EXT)
    };
    std::vector<std::string> names{"public_a", "internal_b"};
    auto const bytes = buildMinimalMacho64(syms, names,
                                            /*includeDysymtab=*/true,
                                            /*iextdefsym=*/0u,
                                            /*nextdefsym=*/0u);   // ← the case under pin
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "zero_nextdef.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "kind=" << binaryReadErrorKindName(r.error().kind);
    ASSERT_EQ(r->size(), 1u)
        << "nextdefsym==0 MUST fall through to N_EXT walk (not "
           "treat as full-scan slice) — only the external row should "
           "surface; a regression that flips to full-scan-slice would "
           "surface BOTH rows and fail size==1";
    EXPECT_EQ((*r)[0].mangledName, "public_a");
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
    // 2060dc8 audit fold (test-analyzer #2): the filtered N_UNDF row
    // has n_strx=0 (unnamed), so it's filtered BEFORE the readNul
    // step that bumps the corruption counter. Pin that the counter
    // stays at 0 — a regression that reordered the filters to put
    // n_strx==0 last AND removed isSectionDefined() would surface
    // a phantom F_BinaryReaderPartialCorruption Warning.
    EXPECT_EQ(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption),
              0u)
        << "filtered N_UNDF row must not contribute to the "
           "partial-corruption counter";
}

// 2060dc8 audit fold (silent-failure M2 / test-analyzer #1): pin
// N_ABS and N_INDR slice entries are also filtered by the defense-
// in-depth N_TYPE check. The 4ca5bff fold pinned only N_UNDF; the
// commit-message rationale cited N_UNDF/N_ABS/N_INDR as all
// admissible-but-malformed slice contents. A regression that
// special-cased the filter to `isUndefined()` instead of
// `!isSectionDefined()` would silently surface N_ABS / N_INDR as
// Function/Default exports. NType::isUndefined() predicate is also
// covered transitively here (the loop hits it via typeBits() ==
// kNTypeUndf on the N_UNDF row in the prior test; this test
// exercises the other two NType arms).
TEST(BinaryReaderMacho, DysymtabSliceFiltersNAbsAndNIndrEntries) {
    constexpr std::uint8_t kNTypeAbs  = 0x02u;
    constexpr std::uint8_t kNTypeIndr = 0x0Au;
    std::vector<Nlist> syms{
        Nlist{0, static_cast<std::uint8_t>(kNTypeAbs  | kNExtBit), 0, 0, 0},   // [0] N_ABS  — filter
        Nlist{0, static_cast<std::uint8_t>(kNTypeIndr | kNExtBit), 0, 0, 0},   // [1] N_INDR — filter
        sect(1),                                                                // [2] N_SECT — keep
    };
    std::vector<std::string> names{"_abs", "_indr", "real_export"};
    auto const bytes = buildMinimalMacho64(syms, names,
                                            /*includeDysymtab=*/true,
                                            /*iextdefsym=*/0u,
                                            /*nextdefsym=*/3u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "abs_indr_slice.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u)
        << "defense-in-depth: a regression that filtered only N_UNDF "
           "(via isUndefined()) instead of !isSectionDefined() would "
           "surface N_ABS + N_INDR as Function/Default exports — "
           "exactly the silent-failure surface the audit closed";
    EXPECT_EQ((*r)[0].mangledName, "real_export");
}

// 2060dc8 audit fold (test-analyzer #3): the 4ca5bff fold moved
// the isStab() skip to apply on BOTH paths (defense-in-depth). The
// dysymtab-slice arm of that defense was previously only
// transitively covered via the fallback `SkipsStabEntries` test;
// this pin exercises the slice path directly. A regression that
// re-localized stab-skip to fallback-only would silently regress
// here.
TEST(BinaryReaderMacho, DysymtabSliceFiltersStabEntries) {
    std::vector<Nlist> syms{
        Nlist{0, kNStabFun, 1, 0, 0x1000},                                     // [0] stab — filter
        sect(1),                                                                // [1] N_SECT — keep
    };
    std::vector<std::string> names{"stab_in_slice", "real_export"};
    auto const bytes = buildMinimalMacho64(syms, names,
                                            /*includeDysymtab=*/true,
                                            /*iextdefsym=*/0u,
                                            /*nextdefsym=*/2u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "stab_slice.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u)
        << "stab filter must apply on the dysymtab-slice path too "
           "(the 4ca5bff defense-in-depth fold) — a regression that "
           "scoped isStab() to the fallback only would surface the "
           "stab row as a Function/Default export";
    EXPECT_EQ((*r)[0].mangledName, "real_export");
}

// ====================================================================
// c160 HARDENING (D-FF1-MACHO-READER) -- the Mach-O twin of c159.
//
// The reader now walks the EXPORT TRIE (the dlsym surface) as the
// authoritative export list when present, classifies each terminal's
// address by SECTION membership (Function = executable __text; Object =
// __data / __const), and surfaces reexports as Forwarders. Fixtures:
//   * buildMachoDylib -- a synthetic single-arch MH_DYLIB carrying a
//     LC_SEGMENT_64 (an executable __text + a data __data section), a
//     LC_DYLD_INFO_ONLY export trie, and a LC_LOAD_DYLIB (so a reexport
//     ordinal resolves to a dylib leaf). The trie's addresses land in
//     __text / __data per the export's declared kind (the Mach-O analog
//     of the PE fixture's per-EAT-RVA section placement).
//   * the in-process WRITER->READER round-trip (W1) -- the strongest
//     oracle: encode a real DSS dylib via `dss::macho::encode` and read
//     its export trie back.
// ====================================================================

namespace {

// Section VA plan for the synthetic dylib (single __TEXT segment based
// at VA 0, so machHeaderVa == 0 and an export's image offset IS its VA):
//   __text  [0x0100, 0x0200)  flags = pure|some instructions -> Function
//   __data  [0x0200, 0x0300)  flags = S_REGULAR (0)          -> Object
constexpr std::uint64_t kTextSecAddr = 0x0100u;
constexpr std::uint64_t kTextSecSize = 0x0100u;
constexpr std::uint64_t kDataSecAddr = 0x0200u;
constexpr std::uint64_t kDataSecSize = 0x0100u;
constexpr std::uint32_t kSTextFlags  = 0x80000400u;   // PURE|SOME instructions
constexpr std::uint32_t kSDataFlags  = 0x00000000u;   // S_REGULAR

constexpr std::uint32_t kLcSegment64Full  = 0x19u;
constexpr std::uint32_t kLcDyldInfoOnly   = 0x80000022u;
constexpr std::uint32_t kLcLoadDylibC     = 0x0Cu;
constexpr std::uint64_t kExportReexport   = 0x08u;

enum class TrieKind { Function, Data, Reexport };

struct DylibExport {
    std::string   name;
    TrieKind      kind        = TrieKind::Function;
    std::string   reexName;                   // Reexport: target symbol name
    std::uint64_t reexOrdinal = 1;            // Reexport: 1-based dylib ordinal
    std::uint64_t addrOverride = 0;           // 0 = section-default address
};

void appendUleb(std::vector<std::uint8_t>& out, std::uint64_t v) {
    do {
        std::uint8_t byte = static_cast<std::uint8_t>(v & 0x7Fu);
        v >>= 7;
        if (v != 0u) byte |= 0x80u;
        out.push_back(byte);
    } while (v != 0u);
}
[[nodiscard]] std::size_t ulebLen(std::uint64_t v) {
    std::size_t n = 0;
    do { ++n; v >>= 7; } while (v != 0u);
    return n;
}

// Build a FLAT-ROOT export trie (root -> one leaf per export, edge =
// full name). Valid for dyld ENUMERATION (the reader walks every child);
// multi-level radix nesting + prefix accumulation is covered by the
// real-writer round-trip (W1). Terminal payloads mirror Apple's on-wire
// shape: regular = uleb(flags) uleb(imageOffset); reexport = uleb(flags)
// uleb(ordinal) cstring(importName).
[[nodiscard]] std::vector<std::uint8_t>
buildExportTrie(std::vector<DylibExport> const& exps) {
    std::size_t const N = exps.size();
    std::vector<std::vector<std::uint8_t>> payloads(N);
    for (std::size_t i = 0; i < N; ++i) {
        auto& pl = payloads[i];
        if (exps[i].kind == TrieKind::Reexport) {
            appendUleb(pl, kExportReexport);
            appendUleb(pl, exps[i].reexOrdinal);
            for (char c : exps[i].reexName)
                pl.push_back(static_cast<std::uint8_t>(c));
            pl.push_back(0);                        // import-name NUL
        } else {
            std::uint64_t const imageOff = exps[i].addrOverride != 0u
                ? exps[i].addrOverride
                : (exps[i].kind == TrieKind::Function
                       ? kTextSecAddr + i * 8u
                       : kDataSecAddr + i * 8u);
            appendUleb(pl, 0u);                     // flags = regular
            appendUleb(pl, imageOff);
        }
    }
    // Fixpoint the child node offsets (root = node0, leaves = node1..N).
    std::vector<std::uint64_t> off(N + 1, 0);
    auto const leafSize = [&](std::size_t i) -> std::uint64_t {
        return ulebLen(payloads[i].size()) + payloads[i].size() + 1u;
    };
    for (int iter = 0; iter < 8; ++iter) {
        std::uint64_t rootSize = 1u /*terminalSize*/ + 1u /*childCount*/;
        for (std::size_t i = 0; i < N; ++i)
            rootSize += exps[i].name.size() + 1u + ulebLen(off[i + 1]);
        std::uint64_t cur = rootSize;
        for (std::size_t i = 0; i < N; ++i) { off[i + 1] = cur; cur += leafSize(i); }
    }
    std::vector<std::uint8_t> out;
    appendUleb(out, 0u);                            // root terminalSize
    out.push_back(static_cast<std::uint8_t>(N));    // childCount
    for (std::size_t i = 0; i < N; ++i) {
        for (char c : exps[i].name) out.push_back(static_cast<std::uint8_t>(c));
        out.push_back(0);                           // edge NUL
        appendUleb(out, off[i + 1]);
    }
    for (std::size_t i = 0; i < N; ++i) {
        appendUleb(out, payloads[i].size());        // terminalSize
        out.insert(out.end(), payloads[i].begin(), payloads[i].end());
        out.push_back(0);                           // childCount = 0
    }
    return out;
}

struct BuiltDylib {
    std::vector<std::uint8_t> bytes;
    std::size_t exportOff    = 0;
    std::size_t exportSize   = 0;
    std::size_t dyldInfoLcOff = 0;   // LC_DYLD_INFO_ONLY LC start
};

void putName16(std::vector<std::uint8_t>& b, std::size_t off,
               std::string_view name) {
    for (std::size_t k = 0; k < name.size() && k < 16u; ++k)
        b[off + k] = static_cast<std::uint8_t>(name[k]);
}

// One dylib-load command in the assembled fixture. `cmd` is the LC type
// (LC_LOAD_DYLIB / LC_LAZY_LOAD_DYLIB / ...); all such commands share the
// 1-based reexport ordinal space, in emission order.
constexpr std::uint32_t kLcLazyLoadDylib = 0x20u;   // no LC_REQ_DYLD
struct DylibLC {
    std::uint32_t cmd  = kLcLoadDylibC;
    std::string   path;
};
// 8-aligned size of a dylib_command carrying `path` (24-byte header +
// the NUL-terminated path, rounded up to 8).
[[nodiscard]] std::size_t dylibLcSize(std::string const& path) {
    return ((24 + path.size() + 1 + 7) / 8) * 8;
}

// Assemble a minimal MH_DYLIB around a (possibly hand-crafted) export
// trie blob: header + LC_SEGMENT_64 __TEXT (__text exec + __data) +
// LC_DYLD_INFO_ONLY (export_off/size -> the trie) + the given dylib-load
// commands (in order -> the 1-based reexport ordinal space). The trie
// bytes sit right after the load commands.
[[nodiscard]] BuiltDylib
assembleDylibMulti(std::vector<std::uint8_t> const& trie,
                   std::vector<DylibLC> const& dylibs) {
    constexpr std::size_t kSegCmdSize   = 72 + 2 * 80;   // hdr + 2 sections
    constexpr std::size_t kDyldInfoSize = 48;
    std::size_t dylibsSize = 0;
    for (auto const& d : dylibs) dylibsSize += dylibLcSize(d.path);
    std::uint32_t const ncmds =
        static_cast<std::uint32_t>(2 + dylibs.size());
    std::size_t const sizeofcmds = kSegCmdSize + kDyldInfoSize + dylibsSize;
    std::size_t const trieOff   = 32 + sizeofcmds;
    std::size_t const totalSize = trieOff + trie.size();

    std::vector<std::uint8_t> b(totalSize, 0);
    putU32(b, 0,  0xFEEDFACFu);
    putU32(b, 4,  0x0100000Cu);                  // cputype CPU_TYPE_ARM64
    putU32(b, 8,  0u);
    putU32(b, 12, 0x6u);                         // MH_DYLIB
    putU32(b, 16, ncmds);
    putU32(b, 20, static_cast<std::uint32_t>(sizeofcmds));
    putU32(b, 24, 0u);
    putU32(b, 28, 0u);

    std::size_t lc = 32;
    // LC_SEGMENT_64 __TEXT (fileoff 0, filesize>0 -> the header segment,
    // vmaddr 0 -> machHeaderVa 0). Two sections: __text (exec), __data.
    putU32(b, lc + 0, kLcSegment64Full);
    putU32(b, lc + 4, static_cast<std::uint32_t>(kSegCmdSize));
    putName16(b, lc + 8, "__TEXT");
    putU64(b, lc + 24, 0u);                      // vmaddr
    putU64(b, lc + 32, 0x1000u);                 // vmsize
    putU64(b, lc + 40, 0u);                      // fileoff
    putU64(b, lc + 48, static_cast<std::uint64_t>(totalSize));  // filesize
    putU32(b, lc + 56, 5u);                      // maxprot r-x
    putU32(b, lc + 60, 5u);                      // initprot r-x
    putU32(b, lc + 64, 2u);                      // nsects
    putU32(b, lc + 68, 0u);
    std::size_t const s0 = lc + 72;              // section_64 __text
    putName16(b, s0 + 0,  "__text");
    putName16(b, s0 + 16, "__TEXT");
    putU64(b, s0 + 32, kTextSecAddr);
    putU64(b, s0 + 40, kTextSecSize);
    putU32(b, s0 + 64, kSTextFlags);
    std::size_t const s1 = s0 + 80;              // section_64 __data
    putName16(b, s1 + 0,  "__data");
    putName16(b, s1 + 16, "__DATA");
    putU64(b, s1 + 32, kDataSecAddr);
    putU64(b, s1 + 40, kDataSecSize);
    putU32(b, s1 + 64, kSDataFlags);
    lc += kSegCmdSize;

    std::size_t const dyldInfoLc = lc;           // LC_DYLD_INFO_ONLY
    putU32(b, lc + 0, kLcDyldInfoOnly);
    putU32(b, lc + 4, static_cast<std::uint32_t>(kDyldInfoSize));
    putU32(b, lc + 40, static_cast<std::uint32_t>(trieOff));   // export_off
    putU32(b, lc + 44, static_cast<std::uint32_t>(trie.size())); // export_size
    lc += kDyldInfoSize;

    for (auto const& d : dylibs) {                // dylib-load commands
        std::size_t const sz = dylibLcSize(d.path);
        putU32(b, lc + 0, d.cmd);
        putU32(b, lc + 4, static_cast<std::uint32_t>(sz));
        putU32(b, lc + 8, 24u);                  // name.offset
        for (std::size_t k = 0; k < d.path.size(); ++k)
            b[lc + 24 + k] = static_cast<std::uint8_t>(d.path[k]);
        lc += sz;
    }

    for (std::size_t k = 0; k < trie.size(); ++k) b[trieOff + k] = trie[k];

    BuiltDylib out;
    out.bytes         = std::move(b);
    out.exportOff     = trieOff;
    out.exportSize    = trie.size();
    out.dyldInfoLcOff = dyldInfoLc;
    return out;
}

// Back-compat wrapper: 0 or 1 LC_LOAD_DYLIB commands.
[[nodiscard]] BuiltDylib
assembleDylib(std::vector<std::uint8_t> const& trie,
              bool includeLoadDylib,
              std::string dylibPath = "/usr/lib/libReexport.dylib") {
    std::vector<DylibLC> dylibs;
    if (includeLoadDylib)
        dylibs.push_back(DylibLC{kLcLoadDylibC, std::move(dylibPath)});
    return assembleDylibMulti(trie, dylibs);
}

[[nodiscard]] BuiltDylib buildMachoDylib(std::vector<DylibExport> const& exps) {
    return assembleDylib(buildExportTrie(exps), /*includeLoadDylib=*/true);
}

// Build a "caterpillar" export trie: a spine of `depth` nodes where node
// 0 is a non-terminal root and nodes 1..depth are TERMINALS reached via a
// 1-byte edge from the prior node (so the accumulated names are "a",
// "aa", ..., growing linearly). Structurally valid, but total materialized
// name bytes are Theta(depth^2) -- the memory-exhaustion DoS the reader's
// name-byte cap must reject. Child offsets are laid out by fixpoint (their
// ULEB widths depend on the offsets they encode).
[[nodiscard]] std::vector<std::uint8_t> buildCaterpillarTrie(std::size_t depth) {
    std::vector<std::uint8_t> termPayload;      // shared regular-export payload
    appendUleb(termPayload, 0u);                // flags = regular
    appendUleb(termPayload, kTextSecAddr);      // address (in __text)
    auto const nodeSize = [&](std::size_t k, std::uint64_t childOff) -> std::uint64_t {
        std::uint64_t sz = (k == 0)
            ? 1u                                              // terminalSize uleb(0)
            : ulebLen(termPayload.size()) + termPayload.size();  // termSize + payload
        sz += 1u;                                             // childCount u8
        if (k < depth) sz += 1u /*edge 'a'*/ + 1u /*NUL*/ + ulebLen(childOff);
        return sz;
    };
    std::vector<std::uint64_t> off(depth + 1, 0);
    for (int iter = 0; iter < 16; ++iter) {
        std::uint64_t cur = 0;
        for (std::size_t k = 0; k <= depth; ++k) {
            off[k] = cur;
            cur += nodeSize(k, (k < depth) ? off[k + 1] : 0u);
        }
    }
    std::vector<std::uint8_t> out;
    for (std::size_t k = 0; k <= depth; ++k) {
        if (k == 0) {
            appendUleb(out, 0u);                 // root terminalSize = 0
        } else {
            appendUleb(out, termPayload.size()); // terminalSize
            out.insert(out.end(), termPayload.begin(), termPayload.end());
        }
        if (k < depth) {
            out.push_back(0x01);                 // childCount = 1
            out.push_back(static_cast<std::uint8_t>('a'));  // edge
            out.push_back(0x00);                 // edge NUL
            appendUleb(out, off[k + 1]);         // child node offset
        } else {
            out.push_back(0x00);                 // leaf: childCount = 0
        }
    }
    return out;
}

// Find a surfaced row by name (trie DFS order is not definition order).
[[nodiscard]] ImportSurface const*
findRow(std::vector<ImportSurface> const& rows, std::string_view name) {
    for (auto const& r : rows) if (r.mangledName == name) return &r;
    return nullptr;
}

// Build a minimal Mach-O with a LC_SEGMENT_64 (__text exec, __data) +
// LC_SYMTAB (nlist), NO export trie -- so the reader takes the nlist
// fallback and classifies each symbol by its n_sect (1 = __text, 2 =
// __data). Used to pin the fallback-path kind classification.
[[nodiscard]] std::vector<std::uint8_t>
buildMachoNlistWithSections(std::vector<Nlist> const& syms,
                            std::vector<std::string> const& names) {
    constexpr std::size_t kHeaderSize = 32;
    constexpr std::size_t kSegCmdSize = 72 + 2 * 80;
    constexpr std::size_t kLcSymtabSz = 24;
    constexpr std::size_t kNlist64Sz  = 16;

    std::vector<std::uint8_t> strTab;
    strTab.push_back(0);
    std::vector<std::uint32_t> nameOffsets;
    for (auto const& n : names) {
        nameOffsets.push_back(static_cast<std::uint32_t>(strTab.size()));
        for (char c : n) strTab.push_back(static_cast<std::uint8_t>(c));
        strTab.push_back(0);
    }

    std::size_t const sizeofcmds    = kSegCmdSize + kLcSymtabSz;
    std::size_t const symtabFileOff = kHeaderSize + sizeofcmds;
    std::size_t const strtabFileOff = symtabFileOff + syms.size() * kNlist64Sz;
    std::size_t const totalSize     = strtabFileOff + strTab.size();

    std::vector<std::uint8_t> b(totalSize, 0);
    putU32(b, 0,  kMachOMagic64);
    putU32(b, 4,  0x0100000Cu);
    putU32(b, 12, 0x6u);                          // MH_DYLIB
    putU32(b, 16, 2u);                            // ncmds
    putU32(b, 20, static_cast<std::uint32_t>(sizeofcmds));

    std::size_t lc = kHeaderSize;
    putU32(b, lc + 0, kLcSegment64Full);
    putU32(b, lc + 4, static_cast<std::uint32_t>(kSegCmdSize));
    putName16(b, lc + 8, "__TEXT");
    putU64(b, lc + 24, 0u);                       // vmaddr
    putU64(b, lc + 32, 0x1000u);
    putU64(b, lc + 40, 0u);                       // fileoff
    putU64(b, lc + 48, static_cast<std::uint64_t>(totalSize));  // filesize
    putU32(b, lc + 56, 5u);
    putU32(b, lc + 60, 5u);
    putU32(b, lc + 64, 2u);                       // nsects
    std::size_t const s0 = lc + 72;
    putName16(b, s0 + 0,  "__text");
    putName16(b, s0 + 16, "__TEXT");
    putU64(b, s0 + 32, kTextSecAddr);
    putU64(b, s0 + 40, kTextSecSize);
    putU32(b, s0 + 64, kSTextFlags);
    std::size_t const s1 = s0 + 80;
    putName16(b, s1 + 0,  "__data");
    putName16(b, s1 + 16, "__DATA");
    putU64(b, s1 + 32, kDataSecAddr);
    putU64(b, s1 + 40, kDataSecSize);
    putU32(b, s1 + 64, kSDataFlags);
    lc += kSegCmdSize;

    putU32(b, lc + 0,  kLcSymtab);
    putU32(b, lc + 4,  static_cast<std::uint32_t>(kLcSymtabSz));
    putU32(b, lc + 8,  static_cast<std::uint32_t>(symtabFileOff));
    putU32(b, lc + 12, static_cast<std::uint32_t>(syms.size()));
    putU32(b, lc + 16, static_cast<std::uint32_t>(strtabFileOff));
    putU32(b, lc + 20, static_cast<std::uint32_t>(strTab.size()));

    for (std::size_t i = 0; i < syms.size(); ++i) {
        std::size_t const off = symtabFileOff + i * kNlist64Sz;
        std::uint32_t const strx = (syms[i].n_strx == 0u)
            ? (i < nameOffsets.size() ? nameOffsets[i] : 0u)
            : syms[i].n_strx;
        putU32(b, off + 0, strx);
        b[off + 4] = syms[i].n_type;
        b[off + 5] = syms[i].n_sect;
        putU64(b, off + 8, syms[i].n_value);
    }
    for (std::size_t i = 0; i < strTab.size(); ++i)
        b[strtabFileOff + i] = strTab[i];
    return b;
}

} // namespace

// -- (c160-1) export-trie kind classification: the red-on-disable pin --
// One Function (address in __text), one Object (address in __data), one
// Reexport (EXPORT_SYMBOL_FLAGS_REEXPORT -> Forwarder + forwardTarget).
// Reverting the trie walk (defaulting every export to Function) fails
// the Object + Forwarder assertions; dropping the reexport branch
// surfaces the reexport as a bogus regular export.
TEST(BinaryReaderMacho, TrieClassifiesFunctionDataAndReexport) {
    auto const built = buildMachoDylib({
        {"_dss_add",    TrieKind::Function},
        {"_dss_global", TrieKind::Data},
        {"_dss_reexp",  TrieKind::Reexport, "_target_sym", 1},
    });
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "libmixed.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "reader rejected the mixed-kind dylib: "
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 3u);

    auto const* fn = findRow(*r, "_dss_add");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->kind, SymbolKind::Function);
    EXPECT_TRUE(fn->forwardTarget.empty());
    EXPECT_EQ(fn->libraryPath, "libmixed.dylib");

    auto const* da = findRow(*r, "_dss_global");
    ASSERT_NE(da, nullptr);
    EXPECT_EQ(da->kind, SymbolKind::Object)
        << "an export whose address lands in a non-executable data "
           "section is Object (the __data / STT_OBJECT precedent)";
    EXPECT_TRUE(da->forwardTarget.empty());

    auto const* rx = findRow(*r, "_dss_reexp");
    ASSERT_NE(rx, nullptr);
    EXPECT_EQ(rx->kind, SymbolKind::Forwarder);
    EXPECT_EQ(rx->forwardTarget, "libReexport.dylib._target_sym")
        << "reexport target = <dylib-leaf>.<import-name>";

    for (auto const& row : *r) {
        EXPECT_EQ(row.visibility, SymbolVisibility::Default);
        EXPECT_EQ(row.linkage, SymbolLinkage::External);
    }
    EXPECT_EQ(rep.errorCount(), 0u);
}

// A same-name reexport (empty import name) forwards to the export's own
// name in the target dylib.
TEST(BinaryReaderMacho, TrieReexportSameNameUsesOwnName) {
    auto const built = buildMachoDylib({
        {"_reexp_same", TrieKind::Reexport, /*reexName=*/"", 1},
    });
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].kind, SymbolKind::Forwarder);
    EXPECT_EQ((*r)[0].forwardTarget, "libReexport.dylib._reexp_same");
}

// (c160-2) W1 -- the in-process WRITER->READER round-trip against a real
// DSS MH_DYLIB -- lives in test_binary_reader_macho_roundtrip.cpp (it
// needs the link/asm writer headers, whose `dss::SymbolVisibility`
// clashes with `dss::ffi::SymbolVisibility` under this file's dual
// using-directives).

// -- (c160-3) nlist fallback classification by n_sect -----------------
TEST(BinaryReaderMacho, NlistFallbackClassifiesBySection) {
    // No export trie -> nlist fallback. n_sect=1 (__text, exec) ->
    // Function; n_sect=2 (__data) -> Object.
    std::vector<Nlist> syms{
        Nlist{0, static_cast<std::uint8_t>(kNTypeSect | kNExtBit), 1, 0, 0x100},
        Nlist{0, static_cast<std::uint8_t>(kNTypeSect | kNExtBit), 2, 0, 0x200},
    };
    std::vector<std::string> names{"_code_sym", "_data_sym"};
    auto const bytes = buildMachoNlistWithSections(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 2u);
    EXPECT_EQ((*r)[0].mangledName, "_code_sym");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    EXPECT_EQ((*r)[1].mangledName, "_data_sym");
    EXPECT_EQ((*r)[1].kind, SymbolKind::Object)
        << "n_sect=2 resolves to __data (non-exec) -> Object; a "
           "regression that defaulted to Function would fail here";
}

// nlist entry whose n_sect indexes no parsed section (section table
// present) is per-entry corruption: skip + Warning, don't abort.
TEST(BinaryReaderMacho, NlistOutOfRangeNsectSkippedAsPartialCorruption) {
    std::vector<Nlist> syms{
        Nlist{0, static_cast<std::uint8_t>(kNTypeSect | kNExtBit), 1, 0, 0x100},
        Nlist{0, static_cast<std::uint8_t>(kNTypeSect | kNExtBit), 99, 0, 0x200},
    };
    std::vector<std::string> names{"_good", "_bad_sect"};
    auto const bytes = buildMachoNlistWithSections(syms, names);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(bytes, "lib.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "an out-of-range n_sect must skip that entry, not abort";
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "_good");
    EXPECT_GE(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption), 1u);
}

// -- (c160-4) export-trie bounds / structural fail-loud pins ----------

TEST(BinaryReaderMacho, TrieRegionPastEofFailsLoud) {
    auto built = buildMachoDylib({{"_f", TrieKind::Function}});
    // Poison LC_DYLD_INFO_ONLY.export_size (LC + 44) to overrun the buffer.
    putU32(built.bytes, built.dyldInfoLcOff + 44, 0x10000u);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "trie-oob.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("export trie"), std::string::npos);
    EXPECT_NE(r.error().detail.find("runs past EOF"), std::string::npos);
}

TEST(BinaryReaderMacho, TrieChildOffsetPastTrieFailsLoud) {
    // Hand-crafted trie: root (non-terminal) with 1 child whose node
    // offset (99) is past the 5-byte trie -- a corrupted child link.
    //   [00]=terminalSize 0  [01]=childCount 1  ['x'][00]  [63]=childOff 99
    std::vector<std::uint8_t> trie{0x00, 0x01,
                                   static_cast<std::uint8_t>('x'), 0x00, 0x63};
    auto built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "bad-child.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("child node offset"), std::string::npos);
}

TEST(BinaryReaderMacho, TrieUnterminatedEdgeStringFailsLoud) {
    // root with childCount 1 whose edge string has no NUL before the
    // trie end -> fail loud (never read past the region).
    std::vector<std::uint8_t> trie{0x00, 0x01,
                                   static_cast<std::uint8_t>('a'),
                                   static_cast<std::uint8_t>('b')};
    auto built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "bad-edge.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("not NUL-terminated"), std::string::npos);
}

TEST(BinaryReaderMacho, TrieCycleFailsLoud) {
    // root's single child edge points back to offset 0 (itself) -> the
    // visited-offset guard fails loud instead of looping forever.
    //   [00]=termSize 0  [01]=childCount 1  ['x'][00]  [00]=childOff 0
    std::vector<std::uint8_t> trie{0x00, 0x01,
                                   static_cast<std::uint8_t>('x'), 0x00, 0x00};
    auto built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "cycle.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("cycle"), std::string::npos);
}

TEST(BinaryReaderMacho, TrieTruncatedTerminalUlebFailsLoud) {
    // A single node whose terminalSize ULEB has the continuation bit set
    // but no following byte within the trie -> bounded ULEB fails loud.
    std::vector<std::uint8_t> trie{0x80};   // continuation bit, nothing after
    auto built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "trunc-uleb.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("ULEB"), std::string::npos);
}

TEST(BinaryReaderMacho, TrieReexportUnterminatedNameFailsLoud) {
    // A reexport terminal whose import name has no NUL within the
    // terminal payload -> fail loud (bounded string read).
    //   root: termSize 0, childCount 1, edge "_r"+NUL, childOff -> leaf
    //   leaf: termSize = payload len, payload = uleb(0x08) uleb(1) 'A' 'B'
    //         (NO trailing NUL) , childCount 0
    std::vector<std::uint8_t> payload{0x08, 0x01,
                                      static_cast<std::uint8_t>('A'),
                                      static_cast<std::uint8_t>('B')};
    std::vector<std::uint8_t> trie;
    appendUleb(trie, 0u);                            // root terminalSize
    trie.push_back(0x01);                            // childCount
    trie.push_back(static_cast<std::uint8_t>('_'));
    trie.push_back(static_cast<std::uint8_t>('r'));
    trie.push_back(0x00);                            // edge NUL
    std::size_t const leafOff = trie.size() + 1;     // +1 for the childOff byte
    appendUleb(trie, leafOff);                       // childOff (1 byte)
    appendUleb(trie, payload.size());                // leaf terminalSize
    trie.insert(trie.end(), payload.begin(), payload.end());
    trie.push_back(0x00);                            // leaf childCount
    auto built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "bad-reexp.dylib", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("import-name"), std::string::npos);
}

// -- (c160-5) export-trie per-entry corruption: skip + warn -----------
// An export whose address resolves to no section is skipped + summarized
// (the trie region + structure are sound, only the one pointer is bad).
TEST(BinaryReaderMacho, TrieAddressInNoSectionSkippedAsPartialCorruption) {
    auto const built = buildMachoDylib({
        {"_ok",  TrieKind::Function},
        {"_oob", TrieKind::Function, "", 1, /*addrOverride=*/0x9000u},
    });
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "oob-addr.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "an out-of-section export address must skip, not abort";
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].mangledName, "_ok");
    EXPECT_GE(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption), 1u)
        << "an export address in no section fires the partial-corruption "
           "Warning";
}

// -- (c160-6) export-trie memory-exhaustion DoS (name explosion) ------
// A "caterpillar" trie is structurally VALID -- distinct node offsets,
// NUL-terminated edges, all child offsets in range, no cycle -- yet its
// terminals' accumulated names grow linearly with depth, so the TOTAL
// materialized name bytes are Theta(depth^2). Without a cap a ~1MB such
// dylib inflates to gigabytes and OOM-kills the process; the reader's
// whole job is untrusted real dylibs, so this is a live DoS. The
// total-name-bytes cap must FAIL LOUD before the quadratic allocation.
// RED-ON-DISABLE: delete the cap and the reader instead returns `depth`
// rows (quadratic name materialization), flipping the ASSERT_FALSE.
TEST(BinaryReaderMacho, TrieCaterpillarNameExplosionFailsLoud) {
    auto const trie  = buildCaterpillarTrie(/*depth=*/2000);
    auto const built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "caterpillar.dylib", rep);
    ASSERT_FALSE(r.has_value())
        << "a caterpillar name-explosion must fail loud, not allocate "
           "quadratically (surfaced " << (r.has_value() ? r->size() : 0)
        << " rows)";
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("total materialized name"),
              std::string::npos)
        << "actual: " << r.error().detail;
}

// -- (c160-7) empty-name terminal parity with the nlist path ----------
// A terminal reached with an EMPTY accumulated name (a root-as-terminal
// here) is not a usable ImportSurface row. The nlist path skip+counts an
// empty resolved name; the trie path must do the same (not emit a
// nameless row). The normally-named child export still surfaces.
TEST(BinaryReaderMacho, TrieEmptyNameTerminalSkippedAsPartialCorruption) {
    // Root IS a terminal (regular export, addr in __text) AND has one
    // child "_ok" -> a terminal (addr in __text).
    std::vector<std::uint8_t> rootPayload;
    appendUleb(rootPayload, 0u);                 // flags = regular
    appendUleb(rootPayload, kTextSecAddr);       // addr in __text
    std::vector<std::uint8_t> trie;
    appendUleb(trie, rootPayload.size());        // root terminalSize
    trie.insert(trie.end(), rootPayload.begin(), rootPayload.end());
    trie.push_back(0x01);                        // childCount = 1
    trie.push_back(static_cast<std::uint8_t>('_'));
    trie.push_back(static_cast<std::uint8_t>('o'));
    trie.push_back(static_cast<std::uint8_t>('k'));
    trie.push_back(0x00);                        // edge NUL
    std::size_t const leafOff = trie.size() + 1; // +1 for the 1-byte childOff
    appendUleb(trie, leafOff);                   // childOff -> leaf
    std::vector<std::uint8_t> leafPayload;
    appendUleb(leafPayload, 0u);                 // flags = regular
    appendUleb(leafPayload, kTextSecAddr + 8u);  // addr in __text
    appendUleb(trie, leafPayload.size());        // leaf terminalSize
    trie.insert(trie.end(), leafPayload.begin(), leafPayload.end());
    trie.push_back(0x00);                        // leaf childCount = 0

    auto const built = assembleDylib(trie, /*includeLoadDylib=*/false);
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "empty-name.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << "an empty-name terminal must skip, not abort: "
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 1u)
        << "the nameless root-terminal must NOT surface as a row; only "
           "the named child export does";
    EXPECT_EQ((*r)[0].mangledName, "_ok");
    EXPECT_EQ((*r)[0].kind, SymbolKind::Function);
    EXPECT_GE(countCode(rep, DiagnosticCode::F_BinaryReaderPartialCorruption), 1u)
        << "the empty-name skip fires the partial-corruption Warning "
           "(parity with the nlist empty-resolved-name path)";
}

// -- (c160-8) reexport ordinal spans ALL dylib-load commands ----------
// The reexport library ordinal is 1-based across EVERY dylib-load command
// (LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB /
// LC_LAZY_LOAD_DYLIB / LC_LOAD_UPWARD_DYLIB). A lazy-load command first
// (ordinal 1) + a real load command second (ordinal 2): a reexport naming
// ordinal 2 must resolve to the SECOND dylib's leaf. Omitting lazy-load
// from the ordinal space would shift ordinals and name the wrong dylib.
TEST(BinaryReaderMacho, ReexportOrdinalSpansAllDylibLoadCommands) {
    auto const trie = buildExportTrie({
        {"_reexp", TrieKind::Reexport, "_tgt", /*reexOrdinal=*/2},
    });
    auto const built = assembleDylibMulti(trie, {
        DylibLC{kLcLazyLoadDylib, "/usr/lib/libLazy.dylib"},   // ordinal 1
        DylibLC{kLcLoadDylibC,    "/usr/lib/libReal.dylib"},   // ordinal 2
    });
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "reexp-ord.dylib", rep);
    ASSERT_TRUE(r.has_value())
        << (r.has_value() ? "" : r.error().detail);
    ASSERT_EQ(r->size(), 1u);
    EXPECT_EQ((*r)[0].kind, SymbolKind::Forwarder);
    EXPECT_EQ((*r)[0].forwardTarget, "libReal.dylib._tgt")
        << "ordinal 2 must index the SECOND dylib-load command; a "
           "regression that skipped LC_LAZY_LOAD_DYLIB in the ordinal "
           "space would resolve the wrong dylib";
}
