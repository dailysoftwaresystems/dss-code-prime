// Mach-O 64-bit .o writer tests — plan 14 LK3 cycle 1.
//
// Pins golden byte-level invariants of the emitted MH_OBJECT:
//   * mach_header_64 magic = 0xFEEDFACF (MH_MAGIC_64).
//   * cputype = 0x01000007 (CPU_TYPE_X86_64); filetype = MH_OBJECT (1).
//   * ncmds = 2 (LC_SEGMENT_64 + LC_SYMTAB).
//   * LC_SEGMENT_64 at byte 32; section_64 for `__text` immediately
//     follows the segment command.
//   * section_64.sectname = "__text"; segname = "__TEXT" (two-level
//     naming — D-LK3-1 closure).
//   * section_64.flags = 0x80000400 (S_REGULAR | S_ATTR_PURE_INSTRUCTIONS
//     | S_ATTR_SOME_INSTRUCTIONS).
//   * nlist_64 records are 16 bytes packed.
//   * relocation_info records are 8 bytes packed; r_info high 4 bits =
//     r_type (BRANCH=2 for rel32 → call sym).
//   * String table NUL-seeded (n_strx=0 means "no name") — same shape
//     as ELF (D-LK4-9 substrate consumer).
//
// Also pins that the shipped `macho64-x86_64-darwin.format.json`
// loads cleanly via `loadShipped`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/macho.hpp"
#include "link/format/macho_chained_fixups.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
#include "macho_test_support.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// D-TEST-LE-READ-HELPERS CLOSED at 8aabc04 audit fold; local LE
// readers harmonized to the shared substrate at
// `tests/link/link_test_support.hpp`. The fold left local copies
// in this file dead-but-present; the 5ac97ae audit fold (4-agent
// convergence: code-reviewer HIGH-1 + type-design Q5 + simplifier
// S1 + comment-analyzer) drops them.
using dss::link_format::test::readU16LE;
using dss::link_format::test::readU32LE;
using dss::link_format::test::readU64LE;

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
    auto f = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-x86_64-darwin) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// The arm64 sibling loader (D-LK3-MACHO-ARM64-OBJECT): arm64 target + the new
// macho64-arm64-darwin relocatable-object format.
[[nodiscard]] Loaded loadShippedArm64() {
    Loaded out;
    auto t = TargetSchema::loadShipped("arm64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(arm64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped("macho64-arm64-darwin");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-arm64-darwin) failed";
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

TEST(MachOFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(loaded.format->name(), "macho64-x86_64-darwin");
    EXPECT_EQ(loaded.format->macho().cputype, 0x01000007u);
    EXPECT_EQ(loaded.format->macho().cpusubtype, 3u);
    EXPECT_TRUE(loaded.format->macho().filetype == MachOObjectType::Object);
    auto const* textRow = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(textRow, nullptr);
    EXPECT_EQ(textRow->name, "__text");
    EXPECT_EQ(textRow->segment, "__TEXT");
    EXPECT_EQ(textRow->addrAlign, 4u);   // log2(16)
    EXPECT_EQ(textRow->type, 0x80000400u);
    // Mach-O has NO section headers for symtab/strtab.
    EXPECT_EQ(loaded.format->sectionByKind(SectionKind::Symtab), nullptr);
    EXPECT_EQ(loaded.format->sectionByKind(SectionKind::Strtab), nullptr);
}

// ── mach_header_64 golden bytes ─────────────────────────────────

TEST(MachOWriter, MachHeader64IdentityBytesMatchAppleAbi) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GE(bytes.size(), 32u);
    EXPECT_EQ(rep.errorCount(), 0u);

    // magic = MH_MAGIC_64 = 0xFEEDFACF
    EXPECT_EQ(readU32LE(bytes, 0), 0xFEEDFACFu);
    // cputype = CPU_TYPE_X86_64 = 0x01000007
    EXPECT_EQ(readU32LE(bytes, 4), 0x01000007u);
    // cpusubtype = CPU_SUBTYPE_X86_64_ALL = 3
    EXPECT_EQ(readU32LE(bytes, 8), 3u);
    // filetype = MH_OBJECT = 1
    EXPECT_EQ(readU32LE(bytes, 12), 1u);
    // ncmds = 2 (LC_SEGMENT_64 + LC_SYMTAB)
    EXPECT_EQ(readU32LE(bytes, 16), 2u);
    // sizeofcmds = 72 + 80*1 (segment+section) + 24 (symtab) = 176
    EXPECT_EQ(readU32LE(bytes, 20), 176u);
    // flags = 0 (MH_SUBSECTIONS_VIA_SYMBOLS deliberately NOT set
    // because cycle 1 emits a flat __text without subsection markers;
    // anchored as D-LK3-2 for the future subsection-emit cycle)
    EXPECT_EQ(readU32LE(bytes, 24), 0u);
    // reserved = 0
    EXPECT_EQ(readU32LE(bytes, 28), 0u);
}

// ── LC_SEGMENT_64 + section_64 two-level naming ─────────────────

TEST(MachOWriter, LcSegment64ContainsOneSectionWithTwoLevelNaming) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 32u + 72u + 80u);

    // LC_SEGMENT_64 at byte 32
    EXPECT_EQ(readU32LE(bytes, 32), 0x19u);  // LC_SEGMENT_64
    EXPECT_EQ(readU32LE(bytes, 36), 72u + 80u);  // cmdsize
    // segname (16 bytes) starts at 40 — empty for MH_OBJECT
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(bytes[40 + i], 0u) << "LC_SEGMENT_64 segname must be empty";
    }
    // nsects @ +72+64 = +136 (segment_command_64 fields:
    // cmd(4)+cmdsize(4)+segname(16)+vmaddr(8)+vmsize(8)+fileoff(8)+
    // filesize(8)+maxprot(4)+initprot(4)+nsects(4)+flags(4)).
    EXPECT_EQ(readU32LE(bytes, 32 + 64), 1u);  // nsects = 1

    // section_64 starts at byte 32 + 72 = 104
    // sectname[16] = "__text\0\0\0\0\0\0\0\0\0\0"
    EXPECT_EQ(bytes[104 + 0], '_');
    EXPECT_EQ(bytes[104 + 1], '_');
    EXPECT_EQ(bytes[104 + 2], 't');
    EXPECT_EQ(bytes[104 + 3], 'e');
    EXPECT_EQ(bytes[104 + 4], 'x');
    EXPECT_EQ(bytes[104 + 5], 't');
    EXPECT_EQ(bytes[104 + 6], 0u);
    // segname[16] = "__TEXT\0\0\0\0\0\0\0\0\0\0" at offset 104+16=120
    EXPECT_EQ(bytes[120 + 0], '_');
    EXPECT_EQ(bytes[120 + 1], '_');
    EXPECT_EQ(bytes[120 + 2], 'T');
    EXPECT_EQ(bytes[120 + 3], 'E');
    EXPECT_EQ(bytes[120 + 4], 'X');
    EXPECT_EQ(bytes[120 + 5], 'T');
    EXPECT_EQ(bytes[120 + 6], 0u);
    // section_64.flags @ offset 104 + 16 + 16 + 8 + 8 + 4 + 4 + 4 + 4 = 168
    // (sectname[16] + segname[16] + addr(8) + size(8) + offset(4) +
    //  align(4) + reloff(4) + nreloc(4) → flags)
    EXPECT_EQ(readU32LE(bytes, 168), 0x80000400u);
}

// ── LC_SYMTAB locates symbol + string tables ───────────────────

TEST(MachOWriter, LcSymtabReferencesNlist64AndStringTable) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // LC_SYMTAB starts at byte 32 + 72 + 80 = 184
    EXPECT_EQ(readU32LE(bytes, 184), 0x02u);  // LC_SYMTAB
    EXPECT_EQ(readU32LE(bytes, 188), 24u);    // cmdsize
    std::uint32_t const symoff = readU32LE(bytes, 192);
    std::uint32_t const nsyms = readU32LE(bytes, 196);
    std::uint32_t const stroff = readU32LE(bytes, 200);
    std::uint32_t const strsize = readU32LE(bytes, 204);
    EXPECT_EQ(nsyms, 1u);
    EXPECT_GT(symoff, 0u);
    EXPECT_LE(symoff + 16u, bytes.size());

    // nlist_64 record: n_strx(u32) + n_type(u8) + n_sect(u8) +
    // n_desc(u16) + n_value(u64). Total 16 bytes.
    EXPECT_EQ(readU32LE(bytes, symoff + 0), 1u)
        << "n_strx points 1 byte past the leading NUL "
           "('_sym_7' lives at offset 1 in the strtab)";
    // n_type = N_SECT | N_EXT = 0x0F
    EXPECT_EQ(bytes[symoff + 4], 0x0Fu);
    // n_sect = 1 (1-based)
    EXPECT_EQ(bytes[symoff + 5], 1u);
    // n_value = 0 (function offset 0 in .text)
    EXPECT_EQ(readU64LE(bytes, symoff + 8), 0u);

    // String table starts with NUL (n_strx=0 = "no name")
    EXPECT_EQ(bytes[stroff], 0u);
    // Then "_sym_7\0" at offset 1
    EXPECT_EQ(bytes[stroff + 1], '_');
    EXPECT_EQ(bytes[stroff + 2], 's');
    EXPECT_EQ(bytes[stroff + 3], 'y');
    EXPECT_EQ(bytes[stroff + 4], 'm');
    EXPECT_EQ(bytes[stroff + 5], '_');
    EXPECT_EQ(bytes[stroff + 6], '7');
    EXPECT_EQ(bytes[stroff + 7], 0u);
    EXPECT_GE(strsize, 8u);
}

// ── D-LK-OBJECT-EXTERN-SYMBOL-NAMES: real (pre-mangled) name verbatim ──

TEST(MachOWriter, ObjectSymtabEmitsPipelineMangledNameVerbatimNoDoubleUnderscore) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{10};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    // The compile pipeline pre-mangles a Mach-O defined name with a leading
    // `_` (applyCMangling). The writer must emit it VERBATIM — a writer-side
    // re-mangle would DOUBLE it to `__public_fn`. Seeding the already-`_`-
    // prefixed name is what makes a double-mangle detectable.
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_public_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // Single-symbol layout matches the LcSymtab test: LC_SYMTAB at 184,
    // stroff at 200; the name sits at strtab offset 1 (past the leading NUL).
    std::uint32_t const stroff = readU32LE(bytes, 200);
    std::string name;
    for (std::size_t p = stroff + 1; p < bytes.size() && bytes[p] != 0; ++p) {
        name.push_back(static_cast<char>(bytes[p]));
    }
    EXPECT_EQ(name, "_public_fn")
        << "Mach-O writer must emit the pipeline-mangled name verbatim — "
           "exactly one leading underscore, never a re-mangled `__public_fn`";
}

// ── relocation_info r_info packing ─────────────────────────────

TEST(MachOWriter, RelocationInfoPacksTypeLengthPcrelExternSymbolnum) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};  // call rel32
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};        // extern
    rel.kind   = RelocationKind{1};  // → BRANCH
    rel.addend = 0;                  // Mach-O convention
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // section_64.reloff @ offset 104 + 16 + 16 + 8 + 8 + 4 + 4 = 160
    std::uint32_t const relocOff = readU32LE(bytes, 160);
    std::uint32_t const relocCount = readU32LE(bytes, 164);
    ASSERT_EQ(relocCount, 1u);
    ASSERT_GT(relocOff, 0u);
    ASSERT_LE(relocOff + 8u, bytes.size());

    // r_address = 1 (patch site within .text)
    EXPECT_EQ(readU32LE(bytes, relocOff + 0), 1u);
    // r_info bits: type(28..31)=2(BRANCH); extern(27)=1; length(25..26)=2;
    // pcrel(24)=1; symbolnum(0..23) = symtab index of target.
    std::uint32_t const rInfo = readU32LE(bytes, relocOff + 4);
    EXPECT_EQ((rInfo >> 28) & 0xFu, 2u);          // r_type = BRANCH
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u);          // r_extern = 1
    EXPECT_EQ((rInfo >> 25) & 0x3u, 2u);          // r_length = 2 (4 bytes)
    EXPECT_EQ((rInfo >> 24) & 0x1u, 1u);          // r_pcrel = 1
    // symtab index: 0 = caller (defined), 1 = extern target.
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u);
}

// ── D-LK3-MACHO-ARM64-OBJECT: the arm64 sibling object format ───

// The shipped macho64-arm64-darwin.format.json loads with the arm64 identity —
// the arm64 mirror of ShippedFileLoadsCleanly. RED-on-disable: delete the file →
// loadShipped fails.
TEST(MachOFormatJson, ShippedArm64FileLoadsCleanly) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_EQ(loaded.format->name(), "macho64-arm64-darwin");
    // CPU_TYPE_ARM64 = (12 | CPU_ARCH_ABI64 0x01000000) = 0x0100000C.
    EXPECT_EQ(loaded.format->macho().cputype, 0x0100000Cu);
    EXPECT_EQ(loaded.format->macho().cpusubtype, 0u);   // CPU_SUBTYPE_ARM64_ALL
    EXPECT_TRUE(loaded.format->macho().filetype == MachOObjectType::Object);
    auto const* textRow = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(textRow, nullptr);
    EXPECT_EQ(textRow->name, "__text");
    EXPECT_EQ(textRow->segment, "__TEXT");
    EXPECT_EQ(textRow->type, 0x80000400u);
    // The arm64 relocation vocabulary: BRANCH26(1)/PAGE21(2)/PAGEOFF12(3)/UNSIGNED(4)
    // — the walker validates emitted reloc kinds against these.
    EXPECT_EQ(loaded.format->relocationByKind(RelocationKind{2})->nativeId, 0x35000000u); // PAGE21
    EXPECT_EQ(loaded.format->relocationByKind(RelocationKind{3})->nativeId, 0x44000000u); // PAGEOFF12
}

// mach_header_64 golden bytes for arm64 — the arm64 mirror of the x86_64 header
// pin. The cputype byte (0x0100000C) is the distinguishing field a foreign
// toolchain reads to accept the .o as arm64 (proven live: `file` reports "Mach-O
// 64-bit object arm64" and system clang links it → exit 42, this cycle's witness).
TEST(MachOWriter, MachHeader64Arm64IdentityBytesMatchAppleAbi) {
    auto loaded = loadShippedArm64();
    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 42); // arm64 RET
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GE(bytes.size(), 32u);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_EQ(readU32LE(bytes, 0), 0xFEEDFACFu);    // MH_MAGIC_64
    EXPECT_EQ(readU32LE(bytes, 4), 0x0100000Cu);    // cputype = CPU_TYPE_ARM64
    EXPECT_EQ(readU32LE(bytes, 8), 0u);             // cpusubtype = CPU_SUBTYPE_ARM64_ALL
    EXPECT_EQ(readU32LE(bytes, 12), 1u);            // filetype = MH_OBJECT
    EXPECT_EQ(readU32LE(bytes, 24), 0u);            // flags = 0
}

// ── D-LK-OBJECT-EXTERN-CALL-MACHO: undefined extern carries its REAL name ──

// The Mach-O analog of ElfWriter.ObjectExternCallEmitsUndefImportNameAndPlt32
// Reloc (c141): an arm64 MH_OBJECT with a BL to an extern import must emit the
// undefined symbol under its REAL pipeline-mangled import name (`_libc_fn`) as
// N_UNDF|N_EXT, with the BRANCH26 relocation's r_extern/r_symbolnum pointing at
// that symtab entry — so a FOREIGN linker (ld64/clang) resolves it against libc
// or a sibling object. Before this cycle the name was the internal `_sym_<id>`
// (the exact blocker the D-LK3-MACHO-ARM64-OBJECT cycle documented: ld64 cannot
// resolve `_sym_7`). RED-ON-DISABLE: revert the macho.cpp undefined-extern loop
// to the `_sym_` spelling → the name assertion fails (`_sym_20` != `_libc_fn`).
// The shipped format also declares `externCallDispatch: "direct-plt"` (pinned
// here) so the LOWERING tier accepts the extern call — DSS never builds a stub
// in a `.o`; ld64 synthesizes it (the ELF c141 direct-plt contract).
TEST(MachOWriter, Arm64ObjectExternCallEmitsUndefImportRealName) {
    auto loaded = loadShippedArm64();
    ASSERT_TRUE(loaded.target && loaded.format);
    ASSERT_TRUE(loaded.format->externCallDispatch().has_value())
        << "macho64-arm64-darwin must declare externCallDispatch so extern "
           "calls lower (D-LK-OBJECT-EXTERN-CALL-MACHO)";

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{10};
    caller.bytes  = {0x00, 0x00, 0x00, 0x94};   // BL #0 (imm26 unresolved)
    caller.relocations.push_back(Relocation{/*offset=*/0u, /*target=*/SymbolId{20},
                                            /*kind=*/RelocationKind{1},  // BRANCH26
                                            /*addend=*/0});
    mod.functions.push_back(std::move(caller));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "_caller",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "_libc_fn";     // pipeline-mangled (leading `_`, macho)
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "an MH_OBJECT with an extern-call import must encode";

    // LC_SYMTAB fields (single-segment/single-section MH_OBJECT layout, same
    // as the LcSymtab pin): cmd@184, symoff@192, nsyms@196, stroff@200.
    std::uint32_t const symoff = readU32LE(bytes, 192);
    std::uint32_t const nsyms  = readU32LE(bytes, 196);
    std::uint32_t const stroff = readU32LE(bytes, 200);
    ASSERT_EQ(nsyms, 2u) << "defined caller + undefined extern";

    // nlist_64[1] = the undefined extern (defined-then-undefined order).
    std::size_t const n1 = symoff + 16;
    std::uint32_t const nStrx = readU32LE(bytes, n1 + 0);
    std::string name;
    for (std::size_t p = stroff + nStrx; p < bytes.size() && bytes[p] != 0; ++p) {
        name.push_back(static_cast<char>(bytes[p]));
    }
    EXPECT_EQ(name, "_libc_fn")
        << "undefined extern must carry its REAL import name (verbatim, one "
           "leading underscore), never the internal _sym_<id> fallback";
    EXPECT_EQ(bytes[n1 + 4], 0x01u)      // n_type = N_UNDF|N_EXT
        << "extern is N_UNDF|N_EXT";
    EXPECT_EQ(bytes[n1 + 5], 0u)         // n_sect = NO_SECT
        << "undefined symbol carries no section";

    // The BRANCH26 relocation targets that symtab entry: r_extern=1,
    // r_symbolnum=1 (index of the extern nlist).
    std::uint32_t const relocOff = readU32LE(bytes, 160);
    std::uint32_t const rInfo    = readU32LE(bytes, relocOff + 4);
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u)  << "r_extern = 1 (symbol-relative)";
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u)   << "r_symbolnum = the extern's index";
}

// An ARM64-DISTINCT relocation (PAGE21, kind=2 → r_type=3) packs correctly — proves
// the arm64 format's reloc table is loaded, NOT x86_64's (whose kind=2 is UNSIGNED,
// r_type=0). RED-on-disable: a wrong nativeId in the format flips the packed r_type.
TEST(MachOWriter, Arm64RelocationInfoPacksPage21) {
    auto loaded = loadShippedArm64();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    caller.bytes = {0x00, 0x00, 0x00, 0x90, 0xC0, 0x03, 0x5F, 0xD6}; // adrp x0,#0 ; ret
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};        // extern
    rel.kind   = RelocationKind{2};  // → ARM64_RELOC_PAGE21
    rel.addend = 0;
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint32_t const relocOff = readU32LE(bytes, 160);
    ASSERT_EQ(readU32LE(bytes, 164), 1u);           // one reloc
    ASSERT_LE(relocOff + 8u, bytes.size());
    std::uint32_t const rInfo = readU32LE(bytes, relocOff + 4);
    EXPECT_EQ((rInfo >> 28) & 0xFu, 3u);            // r_type = 3 (PAGE21) — NOT x86's 0
    EXPECT_EQ((rInfo >> 27) & 0x1u, 1u);            // r_extern
    EXPECT_EQ((rInfo >> 25) & 0x3u, 2u);            // r_length = 2 (4 bytes)
    EXPECT_EQ((rInfo >> 24) & 0x1u, 1u);            // r_pcrel = 1
    EXPECT_EQ(rInfo & 0x00FFFFFFu, 1u);             // symtab index of the extern target
}

// ── Wrong-format kind rejection ────────────────────────────────

TEST(MachOWriter, NonMachOFormatKindEmitsK_NoMatchingObjectFormat) {
    auto loaded = loadShipped();
    auto elf = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(elf.has_value());

    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, **elf, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── macho.cputype = 0 validate rejection ───────────────────────

TEST(MachOFormatJson, ZeroCputypeRejectedByValidate) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"bad-macho","kind":"macho"},
      "macho": { "cputype": 0, "filetype": 1 }
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Mach-O section row missing `segment` rejected ──────────────

TEST(MachOFormatJson, EmptySegmentRejectedByValidate) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"bad-macho-seg","kind":"macho"},
      "macho": { "cputype": 16777223, "filetype": 1 },
      "sections":[{"kind":"text","name":"__text","type":0,"flags":0,"addrAlign":4,"entrySize":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── ELF/PE section row with `segment` set rejected ─────────────

TEST(ElfFormatJson, SegmentFieldRejectedOnElfSection) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"bad-elf","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62 },
      "sections":[{"kind":"text","name":".text","segment":"__TEXT","type":1,"flags":6,"addrAlign":16,"entrySize":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Mach-O MH_OBJECT section with non-zero virtualAddress rejected ──

TEST(MachOFormatJson, NonZeroVirtualAddressRejectedOnMhObject) {
    // Mach-O MH_OBJECT relocatable files use section_64.addr = 0
    // (vmaddr assignment happens at exec build time via
    // LC_SEGMENT_64). The MH_EXECUTE path will use virtualAddress
    // — anchored at D-LK3-2. Pin the cycle-1 validate-rejection so
    // a future MachO-row edit can't silently no-op.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"bad-macho-va","kind":"macho"},
      "macho": { "cputype": 16777223, "filetype": 1 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── D-LK10-ENTRY-MACHO-SECTIONVA-COMPUTED: schema __text VA inconsistent
//    with the computed textFileOff fails loud ───────────────────────────

TEST(MachOExecWriter, SchemaTextVaInconsistentWithTextFileOffFailsLoud) {
    // The exec walker derives textFileOff = alignUp(headerAndCmds,
    // segmentPageSize) but trusts the schema's __text.virtualAddress to equal
    // pageZeroSize + textFileOff. This synthetic exec format declares a VA of
    // pageZeroSize + 0x2000 (TWO segmentPageSizes — congruent, so validate()
    // accepts it: validate only checks `>= pageZeroSize` + `% segmentPageSize`,
    // NOT this stronger equality), but a trivial module's header+load-commands
    // fit in ONE page so the real textFileOff is 0x1000. The walker must FAIL
    // LOUD on the mismatch rather than emit a section_64.addr dyld would
    // mis-map. RED-on-disable: without the `textSegmentVaMatchesFileOff` check
    // the encode SUCCEEDS and `bytes.empty()` flips to false.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": { "name": "macho-va-inconsistent-test", "version": "1.0", "kind": "macho" },
      "dataModel": "LP64",
      "bitFieldStrategy": "gnu_packed",
      "entryPoint": "",
      "externCallDispatch": "direct-plt",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": true
      },
      "sections": [
        { "kind": "text", "name": "__text", "segment": "__TEXT", "type": 2147484672, "flags": 0, "addrAlign": 16, "entrySize": 0, "virtualAddress": 4294975488 }
      ],
      "relocations": [
        { "name": "X86_64_RELOC_BRANCH", "kind": 1, "nativeId": 369098752 },
        { "name": "X86_64_RELOC_UNSIGNED_8", "kind": 2, "nativeId": 100663296 },
        { "name": "X86_64_RELOC_UNSIGNED_4", "kind": 3, "nativeId": 33554432 }
      ]
    })");
    ASSERT_TRUE(r.has_value())
        << "the synthetic exec format must PASS validate (VA 0x100002000 is "
           "congruent + >= pageZeroSize); the textFileOff inequality is exactly "
           "what validate does NOT check";
    auto const& fmt = *r.value();
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "an inconsistent schema VA must abort the encode, not emit a "
           "mis-mapped binary";
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode)
        << "must fail loud with the __text VA / textFileOff inconsistency "
           "diagnostic";
}

// ── Non-zero addend fails loud (Mach-O has no Rela addend) ─────

TEST(MachOWriter, NonZeroAddendFailsLoud) {
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
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

// ── Multi-function module exercises running-offset arithmetic ──

TEST(MachOWriter, MultiFunctionModuleEmitsSequentialTextBytesAndIndices) {
    // Two functions back-to-back; the second's symbol must have
    // n_value = len(first.bytes). Pins the running-offset
    // accumulator across functions (test-analyzer convergence —
    // single-function tests cannot exercise the multi-function
    // index/offset arithmetic).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes = {0x90, 0x90, 0xC3};   // nop nop ret = 3 bytes
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{2};
    b.bytes = {0xC3};
    mod.functions.push_back(std::move(b));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // symoff via LC_SYMTAB at byte 184; symoff field at +8 = 192.
    std::uint32_t const symoff = readU32LE(bytes, 184 + 8);
    std::uint32_t const nsyms  = readU32LE(bytes, 184 + 12);
    ASSERT_EQ(nsyms, 2u);

    // Sym[0] = function `a`: n_value = 0.
    EXPECT_EQ(readU64LE(bytes, symoff + 0 * 16 + 8), 0u);
    // Sym[1] = function `b`: n_value = 3 (right after `a`'s bytes).
    EXPECT_EQ(readU64LE(bytes, symoff + 1 * 16 + 8), 3u);
    // Both symbols are N_SECT | N_EXT = 0x0F.
    EXPECT_EQ(bytes[symoff + 0 * 16 + 4], 0x0Fu);
    EXPECT_EQ(bytes[symoff + 1 * 16 + 4], 0x0Fu);
}

// ── End-to-end via the format-blind linker::link() dispatch ────────────

TEST(LinkerEndToEnd, MachODispatchProducesNonEmptyBytes) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::MachO);
    EXPECT_FALSE(image.bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // Magic bytes 0xFEEDFACF (little-endian)
    ASSERT_GE(image.bytes.size(), 4u);
    EXPECT_EQ(image.bytes[0], 0xCFu);
    EXPECT_EQ(image.bytes[1], 0xFAu);
    EXPECT_EQ(image.bytes[2], 0xEDu);
    EXPECT_EQ(image.bytes[3], 0xFEu);
}

// ── LK3 cycle 2: MH_EXECUTE writer ────────────────────────────

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
    auto f = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-x86_64-darwin-exec) failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}
} // namespace

TEST(MachOExecFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShippedExec();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::MachO);
    EXPECT_TRUE(loaded.format->macho().filetype == MachOObjectType::Execute);
    auto const& im = loaded.format->machoImage();
    EXPECT_EQ(im.pageZeroSize, 0x100000000ull);
    EXPECT_EQ(im.dylinkerPath, "/usr/lib/dyld");
    ASSERT_EQ(im.loadDylibs.size(), 1u);
    EXPECT_EQ(im.loadDylibs[0].path, "/usr/lib/libSystem.B.dylib");
}

TEST(MachOExecWriter, MachHeaderFiletypeEqualsMhExecute) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 32u);
    // mach_header_64.filetype @ +12 = MH_EXECUTE = 2.
    EXPECT_EQ(readU32LE(bytes, 12), 2u);
    // flags @ +24 contains MH_PIE (0x200000) bit.
    EXPECT_NE(readU32LE(bytes, 24) & 0x200000u, 0u);
}

TEST(MachOExecWriter, PageZeroSegmentEmittedFirst) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // First load command at offset 32 is LC_SEGMENT_64 (0x19).
    ASSERT_GE(bytes.size(), 32u + 72u);
    EXPECT_EQ(readU32LE(bytes, 32), 0x19u);
    // segname @ +40 = "__PAGEZERO"
    EXPECT_EQ(bytes[40], '_');
    EXPECT_EQ(bytes[41], '_');
    EXPECT_EQ(bytes[42], 'P');
    EXPECT_EQ(bytes[43], 'A');
    // vmsize @ +64 = pageZeroSize
    EXPECT_EQ(readU64LE(bytes, 64), 0x100000000ull);
}

TEST(MachOExecWriter, LcMainEntryOffPointsToFirstFunction) {
    auto loaded = loadShippedExec();
    // 2 functions: f[0] is some prelude (0x90 NOP + 0xC3 ret), f[1] is the entry.
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes  = {0x90, 0xC3};
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{42};
    b.bytes  = {0xC3};
    mod.functions.push_back(std::move(b));
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Default entryPoint = functions[0] (cycle-2 convention) — entryoff
    // = textFileOff + 0. textFileOff = headerAndCmds page-aligned.
    // No easy way to derive textFileOff in the test without parsing
    // the load commands. Instead pin the property: LC_MAIN.entryoff
    // is a multiple of the page size (because textFileOff is page-
    // aligned and entryFnIdx=0 contributes 0).
    // Locate LC_MAIN by scanning load commands.
    std::size_t off = 32;  // start of load commands
    bool sawLcMain = false;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x80000028u) {       // LC_MAIN
            std::uint64_t const entryOff = readU64LE(bytes, off + 8);
            EXPECT_EQ(entryOff % 0x1000u, 0u);
            sawLcMain = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawLcMain);
}

// F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): a symbol-address DATA global must emit a
// dyld REBASE opcode stream, so a PIE image's absolute pointer is slid at load.
// HOST-INDEPENDENT structural pin — red-on-disable on EVERY leg: revert the
// rebase wiring (rebaseOff/Size in LC_DYLD_INFO_ONLY) and rebase_size returns to
// 0, failing this test even on the Windows/Linux legs that cannot RUN a Mach-O.
// The macOS CI leg is the runtime witness; this is the always-on guard the dss
// cross-target bar pairs with it.
TEST(MachOExecWriter, SymbolAddressDataGlobalEmitsDyldRebaseStream) {
    // DSS's Mach-O exec target is arm64-only (the x86_64 darwin exec format
    // declares no __data section row); mirror the arm64-exit dynamic recipe.
    auto targetR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(targetR.has_value());
    auto target = std::move(targetR).value();
    auto formatR = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(formatR.has_value());
    auto format = std::move(formatR).value();

    // The abs64 reloc kind, found by the SAME agnostic formula the pipeline uses
    // (widthBytes==8 && !pcRelative) — never a hardcoded kind id.
    RelocationKind abs64{0};
    bool foundAbs64 = false;
    for (auto const& r : target->relocations())
        if (r.widthBytes == 8 && !r.pcRelative) { abs64 = r.kind; foundAbs64 = true; break; }
    ASSERT_TRUE(foundAbs64)
        << "arm64 target must declare an 8-byte non-pc-relative reloc (ARM64_RELOC_UNSIGNED)";

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _abs ; RET
    // Reference the extern (call26) so it gets a __stubs/__got slot — an
    // unreferenced import has no binding site. kind 1 = ARM64_RELOC_BRANCH26.
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // An extern import forces the dynamic writer (encodeExecDynamic) — the only
    // path that lays out __DATA and emits LC_DYLD_INFO_ONLY.
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    // `target` — a plain 8-byte mutable data global (NO reloc): must be SKIPPED
    // by the rebase stream (only pointer slots are rebased).
    AssembledData targetData;
    targetData.symbol    = SymbolId{50};
    targetData.section   = DataSectionKind::Data;
    targetData.bytes.assign(8, 0);
    targetData.alignment = Alignment::ofRuntimePow2(8);
    mod.dataItems.push_back(std::move(targetData));
    // `p` — a symbol-address pointer: 8-byte slot + abs64 reloc → `target`.
    AssembledData p;
    p.symbol    = SymbolId{51};
    p.section   = DataSectionKind::Data;
    p.bytes.assign(8, 0);
    p.alignment = Alignment::ofRuntimePow2(8);
    p.relocations.push_back(Relocation{0u, SymbolId{50}, abs64, 0});
    mod.dataItems.push_back(std::move(p));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawDyldInfo = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            std::uint32_t const rebaseOff  = readU32LE(bytes, off + 8);
            std::uint32_t const rebaseSize = readU32LE(bytes, off + 12);
            // THE PIN: a symbol-address data global must yield a non-empty rebase
            // stream. rebase_size==0 means the wiring regressed (red-on-disable).
            ASSERT_GT(rebaseSize, 0u)
                << "symbol-address data global must emit a dyld REBASE stream; "
                   "rebase_size==0 means the PIE-rebase wiring regressed.";
            ASSERT_LE(static_cast<std::size_t>(rebaseOff) + rebaseSize, bytes.size());
            // Decode: SET_TYPE_IMM|POINTER (0x11), ≥1 SET_SEGMENT_AND_OFFSET (0x2X)
            // + DO_REBASE (0x5X), terminated by DONE (0x00).
            EXPECT_EQ(bytes[rebaseOff], 0x11u);  // SET_TYPE_IMM | REBASE_TYPE_POINTER
            bool sawSetSeg = false, sawDoRebase = false, sawDone = false;
            std::size_t bi = rebaseOff + 1;
            std::size_t const end = rebaseOff + rebaseSize;
            while (bi < end) {
                std::uint8_t const opHi = bytes[bi] & 0xF0u;
                bi++;
                if (opHi == 0x20u) {            // SET_SEGMENT_AND_OFFSET_ULEB
                    sawSetSeg = true;
                    while (bi < end && (bytes[bi] & 0x80u)) bi++;  // skip ULEB cont.
                    if (bi < end) bi++;                            // last ULEB byte
                } else if (opHi == 0x50u) {     // DO_REBASE_IMM_TIMES
                    sawDoRebase = true;
                } else if (opHi == 0x00u) {     // DONE
                    sawDone = true; break;
                }
            }
            EXPECT_TRUE(sawSetSeg);
            EXPECT_TRUE(sawDoRebase);
            EXPECT_TRUE(sawDone);
            sawDyldInfo = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawDyldInfo)
        << "LC_DYLD_INFO_ONLY must be present on the legacy darwin exec path.";
}

// c145 (D-LK-RELRO-CONST-DATA-RELOCATABLE): a CONST symbol-address global (relro)
// is FOLDED into the writable `__DATA,__data` section in the MH_EXECUTE image and
// dyld rebases its slot via the __DATA rebase stream — the task-blessed "treat
// relro like data" placement (over a separate __DATA_CONST). No fail-loud.
// RED-on-disable: without the relro→__data merge the item is dropped/rejected and
// rebase_size returns to 0. Mirrors the F5 symbol-address pin above, relro-routed.
TEST(MachOExecWriter, RelRoConstItemFoldsIntoDataAndEmitsDyldRebase) {
    auto targetR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(targetR.has_value());
    auto target = std::move(targetR).value();
    auto formatR = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(formatR.has_value());
    auto format = std::move(formatR).value();
    RelocationKind abs64{0};
    bool foundAbs64 = false;
    for (auto const& r : target->relocations())
        if (r.widthBytes == 8 && !r.pcRelative) { abs64 = r.kind; foundAbs64 = true; break; }
    ASSERT_TRUE(foundAbs64);

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _abs ; RET
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // An extern import forces the dynamic writer (the only path that lays __DATA).
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_abs", "/usr/lib/libSystem.B.dylib"});
    // A plain data target (no reloc — must NOT be rebased).
    AssembledData targetData;
    targetData.symbol    = SymbolId{50};
    targetData.section   = DataSectionKind::Data;
    targetData.bytes.assign(8, 0);
    targetData.alignment = Alignment::ofRuntimePow2(8);
    mod.dataItems.push_back(std::move(targetData));
    // A CONST pointer table → relro (the c145 routing): folds into __data + rebased.
    AssembledData p;
    p.symbol    = SymbolId{51};
    p.section   = DataSectionKind::RelRoConst;
    p.bytes.assign(8, 0);
    p.alignment = Alignment::ofRuntimePow2(8);
    p.relocations.push_back(Relocation{0u, SymbolId{50}, abs64, 0});
    mod.dataItems.push_back(std::move(p));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "a relro item must NOT fail loud in a Mach-O exec image";
    ASSERT_FALSE(bytes.empty());

    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawRebase = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            std::uint32_t const rebaseSize = readU32LE(bytes, off + 12);
            ASSERT_GT(rebaseSize, 0u)
                << "the relro pointer folded into __data must get a dyld REBASE; "
                   "rebase_size==0 means the relro merge/rebase wiring regressed.";
            sawRebase = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawRebase) << "LC_DYLD_INFO_ONLY with a rebase for the relro slot.";
}

// D-LK-MACHO-DATA-EXTERN-DEAD-STUB (c119): the __stubs band is COMPACTED to the
// FUNCTION externs — a DATA extern (got-indirect) is never called, so it gets a
// __got slot but NO __stubs stub. A module with 1 func + 1 data extern must emit
// exactly ONE stub (numFuncExterns), not two. RED-ON-DISABLE: the pre-c119 lockstep
// layout emitted a dead stub per data extern → __stubs size would be 2×stubSize.
TEST(MachOExecWriter, DataExternGetsGotSlotButNoStub) {
    auto targetR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(targetR.has_value());
    auto target = std::move(targetR).value();
    auto formatR = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    ASSERT_TRUE(formatR.has_value());
    auto format = std::move(formatR).value();

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _func_ext ; RET
    // A FUNCTION extern (#99) is CALLED (BRANCH26) → it gets a __stubs stub + __got
    // slot. A DATA extern (#98, isData) just needs to be in externImports — the
    // walker builds a __got slot per extern (dyld-bound) but NO stub for data.
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_func_ext", "/usr/lib/libSystem.B.dylib"});
    ExternImport dataExt{SymbolId{98}, "_data_ext", "/usr/lib/libSystem.B.dylib"};
    dataExt.isData = true;
    mod.externImports.push_back(std::move(dataExt));

    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *target, *format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Walk LC_SEGMENT_64 sections → find __stubs → read its `size` (section_64 @ +40).
    constexpr std::uint32_t kLcSegment64 = 0x19u;
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    std::size_t off = 32;
    bool sawStubs = false;
    std::uint32_t stubsSize = 0, gotSize = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == kLcSegment64) {
            std::uint32_t const nsects = readU32LE(bytes, off + 64);
            std::size_t sec = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s, sec += 80) {
                char const* nm = reinterpret_cast<char const*>(&bytes[sec]);
                // sectname is a NUL-padded char[16]; compare INCLUDING the NUL
                // terminator (the string literals carry it) so "__got" can't match
                // a hypothetical "__got_more".
                if (std::memcmp(nm, "__stubs", 8) == 0) { stubsSize = readU32LE(bytes, sec + 40); sawStubs = true; }
                if (std::memcmp(nm, "__got", 6) == 0)   { gotSize   = readU32LE(bytes, sec + 40); }
            }
        }
        off += cmdsize;
    }
    ASSERT_TRUE(sawStubs) << "the __stubs section must be present";
    // arm64 stub = 12 bytes (ADRP x16 + LDR x16 + BR x16). ONE func extern → 12.
    // The dead-stub bug (a stub per data extern too) would give 2×12 = 24.
    EXPECT_EQ(stubsSize, 12u)
        << "a DATA extern must NOT get a __stubs stub — expected 1 func-extern stub "
           "(12 B); 24 B means the data extern got a dead stub (pre-c119 lockstep).";
    // BOTH externs still get a __got slot (8 B each): the func's stub jumps through
    // it, the data extern's got slot holds the dyld-bound object address.
    EXPECT_EQ(gotSize, 16u)
        << "every extern (func + data) keeps a __got slot — 2 × 8 B.";
}

TEST(MachOExecWriter, IntraModuleBranchAppliedByteForByte) {
    // Branch (rel32, kind 1) from fn[0] to fn[1].
    // sectionVa = pageZeroSize + 0x1000 = 0x100001000.
    // P = sectionVa + 1, S = sectionVa + 6, A = 0 → value = 1.
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
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
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // Locate __text file offset by parsing the __TEXT segment's
    // __text section_64 record. __TEXT segment is the 2nd LC_SEGMENT_64.
    std::size_t off = 32;
    std::uint32_t textFileOff = 0;
    int segIdx = 0;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            ++segIdx;
            if (segIdx == 2) {
                // section_64 starts at off + 72; section.offset @ +96.
                textFileOff = readU32LE(bytes, off + 72 + 48);
                break;
            }
        }
        off += cmdsize;
    }
    ASSERT_NE(textFileOff, 0u);
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 1u);
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);
}

TEST(MachOExecWriter, ExternTargetFailsLoudAsUndefined) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(MachOExecFormatJsonValidate, ObjWithImageBlockRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"obj-with-image","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": 1, "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecFormatJsonValidate, ObjWithBindNowFalseRejected) {
    // Symmetric reject: MH_OBJECT must not set image.bindNow=false.
    // Eager-vs-lazy is an exec-image concept; .o files do not bind
    // at all (the linker resolves at exec build time). Without this
    // rule a JSON typo would silently load. (Type-design HIGH fold,
    // LK6 cycle 2c post-fold review.)
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"obj-with-bindnow-false","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": 1, "flags": 0 },
      "image": { "bindNow": false },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecFormatJsonValidate, ExecMissingLoadDylibsRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"exec-no-dylibs","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld" },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}]
    })");
    ASSERT_FALSE(r.has_value());
}

// ── New tests folded from 7-agent review of LK3 cycle 2 ────────

TEST(MachOExecFormatJsonValidate, DylibFiletypeRejected) {
    // Anchored D-LK3-3: MH_DYLIB declared on closed enum but
    // rejected by validate() until LK6 dynamic linking lands.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"a-dylib","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "dylib", "flags": 0 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecFormatJsonValidate, SectionVaBelowPageZeroRejected) {
    // silent-failure H4 + code-reviewer C2: __text virtualAddress
    // must be >= pageZeroSize, else sectionVa - pageZeroSize
    // underflows.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"underflow","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4096}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecFormatJsonValidate, MissingDylinkerPathRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"no-dyld","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecWriter, EmptyTextFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    mod.functions.push_back(std::move(fn));
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(MachOExecWriter, RelocOffsetPastFunctionBytesFailsLoud) {
    auto loaded = loadShippedExec();
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xC3};
    Relocation rel;
    rel.offset = 4;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));
    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3, 0xC3, 0xC3, 0xC3};
    mod.functions.push_back(std::move(f1));
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(MachOExecWriter, TextSegmentVmaddrEqualsPageZeroEnd) {
    // Load-bearing invariant: __TEXT.vmaddr must equal pageZeroSize
    // (otherwise __TEXT either overlaps __PAGEZERO or leaves a gap;
    // dyld rejects both). The walker computes it; a future
    // refactor that drifts this would silently produce a non-
    // loadable image.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Walk load commands until __TEXT (the 2nd LC_SEGMENT_64).
    std::size_t off = 32;  // start of load commands
    int segIdx = 0;
    bool sawText = false;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            ++segIdx;
            if (segIdx == 2) {
                // segname @ +8 must be "__TEXT"
                EXPECT_EQ(bytes[off + 8], '_');
                EXPECT_EQ(bytes[off + 9], '_');
                EXPECT_EQ(bytes[off + 10], 'T');
                // vmaddr @ +24 must equal pageZeroSize (4 GiB)
                EXPECT_EQ(readU64LE(bytes, off + 24), 0x100000000ull);
                // fileoff @ +40 must equal 0 (mach header is in __TEXT)
                EXPECT_EQ(readU64LE(bytes, off + 40), 0u);
                // nsects @ +64 == 1 (just __text this cycle)
                EXPECT_EQ(readU32LE(bytes, off + 64), 1u);
                sawText = true;
                break;
            }
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawText);
}

TEST(MachOExecWriter, LcLoadDylibStructurePinnedByteForByte) {
    // Load-bearing dyld invariant: LC_LOAD_DYLIB.name offset must
    // be 24 (cmd+24 points at the dylib path string). If a future
    // refactor changes the field layout, dyld silently looks for
    // the path at the wrong offset, fails to find libSystem, and
    // the process never starts. Pin the layout byte-for-byte.
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::size_t off = 32;
    bool sawDylib = false;
    while (off + 8 <= bytes.size()) {
        std::uint32_t const cmd     = readU32LE(bytes, off);
        std::uint32_t const cmdsize = readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x0Cu) {  // LC_LOAD_DYLIB
            // name offset @ +8 must be 24 (cmd+24)
            EXPECT_EQ(readU32LE(bytes, off + 8), 24u);
            // timestamp / current_version / compat_version @ +12/+16/+20
            // are 0 today (reserved for future cycle).
            EXPECT_EQ(readU32LE(bytes, off + 12), 0u);
            EXPECT_EQ(readU32LE(bytes, off + 16), 0u);
            EXPECT_EQ(readU32LE(bytes, off + 20), 0u);
            // path bytes @ +24 begin with "/usr/lib/libSystem"
            EXPECT_EQ(bytes[off + 24], '/');
            EXPECT_EQ(bytes[off + 25], 'u');
            EXPECT_EQ(bytes[off + 26], 's');
            EXPECT_EQ(bytes[off + 27], 'r');
            sawDylib = true;
            break;
        }
        off += cmdsize;
    }
    EXPECT_TRUE(sawDylib);
}

TEST(IsImageFlavorAccessor, ConsistentAcrossThreeFormats) {
    // type-design O1 fold-in: the isImageFlavor() accessor exposes
    // the cross-format triplet check beyond validate(). Pin it
    // matches the shipped JSONs.
    auto objE = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(objE.has_value());
    EXPECT_FALSE((**objE).isImageFlavor());

    auto execE = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(execE.has_value());
    EXPECT_TRUE((**execE).isImageFlavor());

    auto objP = ObjectFormatSchema::loadShipped("pe64-x86_64-windows");
    ASSERT_TRUE(objP.has_value());
    EXPECT_FALSE((**objP).isImageFlavor());

    auto execP = ObjectFormatSchema::loadShipped("pe64-x86_64-windows-exec");
    ASSERT_TRUE(execP.has_value());
    EXPECT_TRUE((**execP).isImageFlavor());

    auto objM = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    ASSERT_TRUE(objM.has_value());
    EXPECT_FALSE((**objM).isImageFlavor());

    auto execM = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(execM.has_value());
    EXPECT_TRUE((**execM).isImageFlavor());
}

TEST(MachOExecWriter, DisplacementOverflowFailsLoud) {
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
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── LK6 cycle 2c: extern imports produce a dynamic Mach-O image ─

TEST(MachOExecWriter, ExternImportsProduceDynamicImage) {
    // Cycle 2c walker has landed: extern imports now produce a
    // real Mach-O dynamic image (parallel to ELF cycle 2b.2). Pins
    // MH_EXECUTE byte + non-empty bytes + dylib/symbol strings.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "_printf";
    imp.libraryPath = "/usr/lib/libSystem.B.dylib";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // filetype @ +12 = MH_EXECUTE (2)
    EXPECT_EQ(static_cast<std::uint32_t>(bytes[12]) |
              (static_cast<std::uint32_t>(bytes[13]) << 8) |
              (static_cast<std::uint32_t>(bytes[14]) << 16) |
              (static_cast<std::uint32_t>(bytes[15]) << 24), 2u);
    std::string_view fileView{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fileView.find("/usr/lib/libSystem.B.dylib"),
              std::string_view::npos);
    EXPECT_NE(fileView.find("_printf"), std::string_view::npos);
}

TEST(MachOExecWriter, DynamicImageEmitsExpectedSegments) {
    // Dynamic Mach-O carries 4 segments: __PAGEZERO + __TEXT (with
    // __text + __stubs) + __DATA_CONST (with __got) + __LINKEDIT.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::string_view fv{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fv.find("__PAGEZERO"),   std::string_view::npos);
    EXPECT_NE(fv.find("__TEXT"),       std::string_view::npos);
    EXPECT_NE(fv.find("__stubs"),      std::string_view::npos);
    EXPECT_NE(fv.find("__DATA_CONST"), std::string_view::npos);
    EXPECT_NE(fv.find("__got"),        std::string_view::npos);
    EXPECT_NE(fv.find("__LINKEDIT"),   std::string_view::npos);
}

TEST(MachOExecWriter, BindNowFalseFailsLoudCitingDLK613) {
    // The lazy-binding upgrade is anchored at D-LK6-13. Until it
    // lands, the walker must fail loud on `image.bindNow = false`.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-lazy-pending","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": false
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("D-LK6-13") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor);
}

// D-LK6-14 substrate (e4508b9 → next 2026-06-01): schema JSON
// accepts the `useChainedFixups` flag. Defaults to false (legacy
// LC_DYLD_INFO_ONLY opcode stream path stays fully supported).
TEST(MachOExecFormatJson, UseChainedFixupsDefaultsToFalse) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cfx-default","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE((*r)->machoImage().useChainedFixups);
}

TEST(MachOExecFormatJson, UseChainedFixupsAcceptsTrue) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cfx-on","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "useChainedFixups": true
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->machoImage().useChainedFixups);
}

TEST(MachOExecFormatJson, UseChainedFixupsRejectsNonBoolean) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cfx-bad","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "useChainedFixups": "yes"
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

// D-LK6-14-INTEGRATION-PAYLOAD closed 2026-06-01: wire
// `dss::macho::detail::buildChainedFixupsPayload` into
// encodeExecDynamic — when `useChainedFixups=true`, replace the
// legacy LC_DYLD_INFO_ONLY emission with LC_DYLD_CHAINED_FIXUPS
// pointing at the payload in __LINKEDIT. Companion
// D-LK6-14-INTEGRATION-GOT-SLOTS (open) populates __got slots
// with DYLD_CHAINED_PTR_64 bitfields + drops LC_DYSYMTAB.
// Byte-level tests pin LC structure; runtime loadability is FF6
// territory.
namespace {
// Inline-test fixture used by the 4 chained-fixups integration
// pins below. Same shape as the legacy BindNowFalse fixture —
// one extern, one function, one BRANCH relocation — but with
// `image.useChainedFixups = true`.
[[nodiscard]] std::shared_ptr<ObjectFormatSchema const>
loadChainedFixupsExecFormat() {
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-cfx-integration","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "useChainedFixups": true
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })");
    if (!fmt.has_value()) return nullptr;
    return *fmt;
}
[[nodiscard]] AssembledModule chainedFixupsTestModule() {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    return mod;
}
// LC_DYLD_CHAINED_FIXUPS = 0x80000034 (LC_REQ_DYLD bit set).
constexpr std::uint32_t kLcDyldChainedFixups = 0x80000034u;
constexpr std::uint32_t kLcDyldInfoOnly      = 0x80000022u;
} // namespace

TEST(MachOExecWriter, ChainedFixupsLcPresent) {
    // Primary mutual-exclusion pin: useChainedFixups=true emits
    // LC_DYLD_CHAINED_FIXUPS AND no LC_DYLD_INFO_ONLY. A regression
    // that re-introduces both LCs (or the wrong one) fails this.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    EXPECT_TRUE(dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups).has_value())
        << "LC_DYLD_CHAINED_FIXUPS must be emitted on the chained "
           "path";
    EXPECT_FALSE(dss::macho::test::findLoadCommand(bytes, kLcDyldInfoOnly).has_value())
        << "LC_DYLD_INFO_ONLY must NOT be emitted when "
           "useChainedFixups=true — legacy + modern are mutually "
           "exclusive";
}

TEST(MachOExecWriter, ChainedFixupsLcCmdsizeIs16Bytes) {
    // LC_DYLD_CHAINED_FIXUPS uses the linkedit_data_command shape:
    // cmd / cmdsize / dataoff / datasize = 4+4+4+4 = 16 bytes.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const cmdsize =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 4));
    EXPECT_EQ(cmdsize, 16u)
        << "linkedit_data_command shape: cmd+cmdsize+dataoff+datasize "
           "= 4+4+4+4 = 16 bytes";
    // dataoff must lie within the buffer + datasize must be non-zero
    // (we have 1 import → at least 1 byte name + NUL + header + starts
    // + import row).
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    std::uint32_t const datasize =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 12));
    EXPECT_GT(datasize, 0u);
    EXPECT_LE(static_cast<std::uint64_t>(dataoff) +
              static_cast<std::uint64_t>(datasize),
              bytes.size())
        << "dataoff + datasize must lie within the emitted binary";
}

TEST(MachOExecWriter, ChainedFixupsPayloadImportsCountMatchesExterns) {
    // Read the payload's dyld_chained_fixups_header.imports_count
    // field (at payload offset 16) and assert it equals the number
    // of externImports (1 in the fixture).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    // dyld_chained_fixups_header layout:
    //   [ 0.. 3] fixups_version
    //   [ 4.. 7] starts_offset
    //   [ 8..11] imports_offset
    //   [12..15] symbols_offset
    //   [16..19] imports_count
    std::uint32_t const importsCount =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 16));
    EXPECT_EQ(importsCount, mod.externImports.size())
        << "payload imports_count must equal module.externImports.size()";
}

TEST(MachOExecWriter, ChainedFixupsSymbolsPoolContainsExternName) {
    // Read symbols_offset from the payload header (at payload+12),
    // then walk into the symbols pool past the NUL sentinel and
    // assert the first name is "_printf" (the fixture's mangledName).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    std::uint32_t const symbolsOffset =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 12));
    // Symbols pool: leading NUL sentinel at relative offset 0; first
    // import's name at offset 1.
    std::size_t const firstNameOff =
        static_cast<std::size_t>(dataoff) +
        static_cast<std::size_t>(symbolsOffset) + 1u;
    ASSERT_LT(firstNameOff, bytes.size());
    std::string firstName;
    for (std::size_t i = firstNameOff;
         i < bytes.size() && bytes[i] != 0u; ++i) {
        firstName.push_back(static_cast<char>(bytes[i]));
    }
    EXPECT_EQ(firstName, "_printf")
        << "symbols pool's first NUL-terminated name must be the "
           "fixture's extern mangledName ('_printf'); this pins the "
           "end-to-end ExternImport.mangledName → ChainedFixupImport.name "
           "→ payload pool flow";
}

// 8aabc04 audit fold (test-analyzer + test-analyzer-dim-2 HIGH):
// pin LC_DYSYMTAB stays emitted on the chained path. The companion
// D-LK6-14-INTEGRATION-GOT-SLOTS will drop it together with __got
// slot bitfield population — a premature regression that drops
// LC_DYSYMTAB here would produce structurally broken chained
// binaries with no failing test.
// D-LK6-14-INTEGRATION-GOT-SLOTS closed: LC_DYSYMTAB is DROPPED on
// the chained-fixups path (chained pointers in __got encode the
// import ordinal directly via DYLD_CHAINED_PTR_64 bits [0..23], so
// the indirect symbol table is redundant). The prior pin (which
// pinned PRESENCE during the substrate window) is now inverted.
TEST(MachOExecWriter, ChainedFixupsDropsLcDysymtab) {
    constexpr std::uint32_t kLcDysymtab = 0x0Bu;
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(
        dss::macho::test::findLoadCommand(bytes, kLcDysymtab).has_value())
        << "LC_DYSYMTAB MUST be absent on the chained-fixups path — "
           "D-LK6-14-INTEGRATION-GOT-SLOTS closed: chained pointers "
           "in __got encode the import ordinal directly so the "
           "indirect symbol table is redundant. A regression that "
           "re-emits LC_DYSYMTAB here would produce dyld-rejected "
           "binaries because ncmds/sizeofcmds arithmetic accounts "
           "for the absence.";
}

// D-LK6-14-INTEGRATION-GOT-SLOTS pin: each __got slot must hold a
// valid DYLD_CHAINED_PTR_64 bind bitfield (bit 63 set, ordinal in
// bits [0..23] matching the import index, next field forming a
// valid chain). Without this, dyld cannot resolve extern imports.
TEST(MachOExecWriter, ChainedFixupsGotSlotsHaveBindBitfield) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_a",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_b",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Walk LC_SEGMENT_64s, find __DATA_CONST, read its fileoff so we
    // can read the 8-byte __got slots directly. segment_command_64
    // layout: cmd(4) + cmdsize(4) + segname[16] + vmaddr(8) +
    // vmsize(8) + fileoff(8) → fileoff at lcOff + 40.
    constexpr std::uint32_t kLcSegment64 = 0x19u;
    std::optional<std::size_t> dataConstFileOff;
    std::optional<std::size_t> dataConstLcOff;
    std::size_t lcOff = 32;  // past mach_header_64
    std::uint32_t const ncmds = readU32LE(bytes, 16);
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t const cmd = readU32LE(bytes, lcOff);
        std::uint32_t const cmdsize = readU32LE(bytes, lcOff + 4);
        if (cmd == kLcSegment64) {
            char name[17] = {};
            std::memcpy(name, &bytes[lcOff + 8], 16);
            if (std::string{name} == "__DATA_CONST") {
                dataConstFileOff = static_cast<std::size_t>(
                    readU64LE(bytes, lcOff + 40));
                dataConstLcOff = lcOff;
                break;
            }
        }
        lcOff += cmdsize;
    }
    ASSERT_TRUE(dataConstFileOff.has_value())
        << "__DATA_CONST LC_SEGMENT_64 must be present";
    // D-LK6-14-INTEGRATION-GOT-SLOTS: __got section_64.reserved1
    // MUST be 0 on the chained path (was numExterns on legacy as
    // an indirect-symtab index; on chained the indirect symtab is
    // dropped so the reference becomes invalid). section_64 starts
    // at segment_command_64 + 72 (cmd(4)+cmdsize(4)+segname[16]
    // +vmaddr(8)+vmsize(8)+fileoff(8)+filesize(8)+maxprot(4)
    // +initprot(4)+nsects(4)+flags(4)); reserved1 within
    // section_64 at offset 68 (sectname[16]+segname[16]+addr(8)
    // +size(8)+offset(4)+align(4)+reloff(4)+nreloc(4)+flags(4)
    // = 68). a4464fe audit-fold (dim-2 M1 + test-analyzer MEDIUM):
    // pin sectname == "__got" first so a __const-before-__got
    // reshape doesn't silently read a sibling section's reserved1.
    std::size_t const sect0Off = *dataConstLcOff + 72;
    char gotName[17] = {};
    std::memcpy(gotName, &bytes[sect0Off], 16);
    EXPECT_EQ(std::string{gotName}, "__got")
        << "expected __got at section[0] of __DATA_CONST — a sibling "
           "section reshape would silently shift offsets and read "
           "the wrong section's reserved1.";
    EXPECT_EQ(readU32LE(bytes, sect0Off + 68), 0u)
        << "section_64.__got.reserved1 must be 0 on the chained path "
           "(was numExterns as indirect-symtab index on legacy; the "
           "indirect symtab is gone so the reference would be stale)";
    // Read slot[0] and slot[1] (8 bytes each).
    std::uint64_t const slot0 = readU64LE(bytes, *dataConstFileOff);
    std::uint64_t const slot1 = readU64LE(bytes, *dataConstFileOff + 8);
    // bit 63 = bind (must be 1 on both slots — they're binds, not rebases).
    EXPECT_NE(slot0 & (1ull << 63), 0u)
        << "slot[0] bind bit (63) must be set";
    EXPECT_NE(slot1 & (1ull << 63), 0u)
        << "slot[1] bind bit (63) must be set";
    // bits [0..23] = ordinal: slot[i] → import row i.
    EXPECT_EQ(slot0 & 0xFFFFFFull, 0u)
        << "slot[0] ordinal must be 0 (first import)";
    EXPECT_EQ(slot1 & 0xFFFFFFull, 1u)
        << "slot[1] ordinal must be 1 (second import)";
    // bits [51..62] = next (12 bits, 4-byte units to next chain entry).
    // Slot[0] points at slot[1] (8 bytes away = 2 four-byte units).
    // Slot[1] is the last → next = 0.
    std::uint64_t const next0 = (slot0 >> 51) & 0xFFFu;
    std::uint64_t const next1 = (slot1 >> 51) & 0xFFFu;
    EXPECT_EQ(next0, 2u)
        << "slot[0] next field must be 2 (4-byte units to slot[1])";
    EXPECT_EQ(next1, 0u)
        << "slot[1] next field must be 0 (end of chain)";
}

// D-LK6-14-INTEGRATION-GOT-SLOTS pin: the chained-fixups payload's
// seg_info_offset[0] must be non-zero (= 8) and point at a
// dyld_chained_starts_in_segment struct with pointer_format=6 and
// non-zero segment_offset. A regression dropping the segInfo arg
// would leave seg_info_offset[0] = 0 (substrate behavior) and dyld
// would see "no chains in segment".
TEST(MachOExecWriter, ChainedFixupsPayloadHasStartsInSegment) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff =
        dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, *lcOff + 8);
    // starts_offset is at payload+4 (header field).
    std::uint32_t const startsOff = readU32LE(bytes, dataoff + 4);
    // starts_in_image: seg_count (u32) at startsOff, seg_info_offset[0]
    // (u32) at startsOff+4.
    EXPECT_EQ(readU32LE(bytes, dataoff + startsOff), 1u)
        << "seg_count must be 1 (single __DATA_CONST segment)";
    std::uint32_t const segInfoOffset =
        readU32LE(bytes, dataoff + startsOff + 4);
    EXPECT_EQ(segInfoOffset, 8u)
        << "seg_info_offset[0] must be 8 (immediately after the "
           "starts_in_image header); a regression dropping segInfo "
           "would leave this 0 (substrate behavior) and dyld would "
           "see 'no chains in segment'";
    // dyld_chained_starts_in_segment at startsOff + 8:
    //   [ 0.. 3] size           [ 4.. 5] page_size
    //   [ 6.. 7] pointer_format [ 8..15] segment_offset
    //   [16..19] max_valid_ptr  [20..21] page_count
    std::size_t const segStructOff =
        dataoff + startsOff + segInfoOffset;
    // size field at struct+0: header bytes + 2 bytes per page_start
    // entry. v1 single-page → 22 + 2*1 = 24. Symbolized via
    // kDyldChainedStartsInSegmentHdrSz so a header-size regression
    // (someone adds a field) surfaces in this test alongside the
    // producer. FLIP-MARKER: when D-LK6-14-MULTI-PAGE-GOT closes,
    // expected size becomes `kDyldChainedStartsInSegmentHdrSz +
    // 2u * page_count` with page_count > 1.
    EXPECT_EQ(readU32LE(bytes, segStructOff + 0),
              static_cast<std::uint32_t>(
                  ::dss::macho::detail::kDyldChainedStartsInSegmentHdrSz
                  + 2u * 1u))
        << "dyld_chained_starts_in_segment.size must be header (22) "
           "+ 2*page_count (2) = 24 for the v1 single-page case";
    EXPECT_EQ(readU16LE(bytes, segStructOff + 6),
              6u)
        << "pointer_format must be 6 (DYLD_CHAINED_PTR_64)";
    EXPECT_NE(readU64LE(bytes, segStructOff + 8), 0u)
        << "segment_offset must be non-zero (= gotVa - "
           "__TEXT.vmaddr); a regression that leaves it 0 means dyld "
           "would resolve the chain at the wrong VM address";
    EXPECT_EQ(readU16LE(bytes, segStructOff + 20), 1u)
        << "page_count must be 1 (single-page __DATA_CONST)";
    // page_starts[0] at segStructOff + 22.
    EXPECT_EQ(readU16LE(bytes, segStructOff + 22), 0u)
        << "page_starts[0] must be 0 (first chained pointer at byte "
           "0 of the page — __got starts at __DATA_CONST start)";
}

// D-LK6-14-SIZEOFCMDS-DELTA-PIN: chained path's sizeofcmds must be
// exactly `kDysymtabCommandSize` (80) less than legacy path's (since
// LC_DYSYMTAB is dropped on chained). Pins the ncmds/sizeofcmds
// arithmetic against subtle drift.
TEST(MachOExecWriter, ChainedFixupsSizeofcmdsDelta) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    // Legacy fixture (useChainedFixups absent → defaults to false).
    auto fmtLegacy = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-legacy-for-delta","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })");
    ASSERT_TRUE(fmtLegacy.has_value());
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytesLegacy = macho::encode(mod, **target, **fmtLegacy, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto fmtChained = loadChainedFixupsExecFormat();
    ASSERT_NE(fmtChained, nullptr);
    auto bytesChained = macho::encode(mod, **target, *fmtChained, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // sizeofcmds field is at mach_header_64 offset 20 (u32).
    std::uint32_t const sizeofcmdsLegacy =
        readU32LE(bytesLegacy, 20);
    std::uint32_t const sizeofcmdsChained =
        readU32LE(bytesChained, 20);
    // Delta = LC_DYLD_INFO_ONLY (48) - LC_DYLD_CHAINED_FIXUPS (16)
    //       + LC_DYSYMTAB (80, dropped on chained) = 112.
    EXPECT_EQ(sizeofcmdsLegacy - sizeofcmdsChained, 112u)
        << "sizeofcmds delta: legacy emits LC_DYLD_INFO_ONLY (48) + "
           "LC_DYSYMTAB (80) = 128; chained emits LC_DYLD_CHAINED_"
           "FIXUPS (16) only = 16; delta = 112. Regression in any "
           "arm of the ternary or in the LC sizes would shift this.";
}

// D-LK6-14-MULTI-PAGE-GOT guard pin (2ba0489 audit fold, test-
// analyzer + dim-2 + simplifier convergence): >512 externs needs
// multi-page chains with per-page page_starts[i]. Until that lands,
// v1 fails loud K_NoMatchingObjectFormat. A regression dropping the
// guard would silently emit a single-page chain that dyld walks off
// the end of, producing load failures only at runtime on macOS.
TEST(MachOExecWriter, ChainedFixupsMultiPageGotFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    // Reloc targets the FIRST extern (SymbolId{100}) so symbol
    // resolution passes; the guard we're pinning fires later, in
    // section (l.5) after layout.
    fn.relocations.push_back(
        Relocation{1u, SymbolId{100u}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    // 4 KiB page / 8-byte __got slot = 512 externs fits; 513 spills
    // onto a second page and trips the guard.
    constexpr std::uint32_t kPageSize    = 4096u;
    constexpr std::uint32_t kSlotSize    = 8u;
    constexpr std::uint32_t kSpillCount  = (kPageSize / kSlotSize) + 1u;
    // FIXTURE-INVARIANT (D-TEST-MULTI-PAGE-FIXTURE-INVARIANT): all
    // 513 externs share libSystem (libOrdinal=1 << 127 ceiling) and
    // the symbols-pool stays under 8 MiB (D-LK6-14-NAME-OFFSET-
    // OVERFLOW). The SOLE failure surface this fixture probes is
    // the multi-page __DATA_CONST guard. A future refactor of the
    // fixture (multi-dylib, longer names) MUST re-verify these
    // boundaries or the test silently re-routes.
    for (std::uint32_t i = 0; i < kSpillCount; ++i) {
        mod.externImports.push_back(ExternImport{
            SymbolId{100u + i},
            "_x" + std::to_string(i),
            "/usr/lib/libSystem.B.dylib"});
    }
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "Multi-page __got must emit no bytes (loud-fail path).";
    EXPECT_EQ(rep.errorCount(), 1u)
        << "Exactly one K_NoMatchingObjectFormat must fire.";
    bool found = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat &&
            d.actual.find("D-LK6-14-MULTI-PAGE-GOT") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "Diagnostic must cite D-LK6-14-MULTI-PAGE-GOT for "
           "future-grep navigability";
}

// 8aabc04 audit fold (test-analyzer-dim-2 HIGH): multi-import name
// ordering. Existing tests use N=1 so a regression that swaps the
// order of `push_back(0)` and `nameOffsets.push_back(...)` would
// shift every offset by 1 but pass N=1 (the lone import would still
// land at offset 1). Use N=2 and assert offsets advance correctly.
TEST(MachOExecWriter, ChainedFixupsTwoImportsHaveCorrectSymbolOrdering) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_alpha",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_beta",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const lcOff = dss::macho::test::findLoadCommand(bytes, kLcDyldChainedFixups);
    ASSERT_TRUE(lcOff.has_value());
    std::uint32_t const dataoff =
        readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    // Pool layout for {"_alpha", "_beta"}:
    //   [0]NUL  [1..6]"_alpha"[0]  [8..12]"_beta"[0]
    // Symbols offset in payload from header[12].
    std::uint32_t const symbolsOffset =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 12));
    std::size_t const poolBase =
        static_cast<std::size_t>(dataoff) + symbolsOffset;
    EXPECT_EQ(bytes[poolBase + 0], 0u)         << "leading NUL sentinel";
    EXPECT_EQ(bytes[poolBase + 1], '_');
    EXPECT_EQ(bytes[poolBase + 6], 'a')        << "_alpha tail char";
    EXPECT_EQ(bytes[poolBase + 7], 0u)         << "NUL after _alpha";
    EXPECT_EQ(bytes[poolBase + 8], '_');
    EXPECT_EQ(bytes[poolBase + 12], 'a')       << "_beta tail char";
    EXPECT_EQ(bytes[poolBase + 13], 0u)        << "NUL after _beta";
    // 5ac97ae audit fold (test-analyzer + test-analyzer-dim-2 +
    // code-architect convergence): pin the packed import-row fields
    // for BOTH imports. Pool-position pin catches push-NUL swap;
    // packed-row pin catches libOrdinal miscast + weakImport
    // hardcode regression + dylibOrdinal multi-import mapping.
    std::uint32_t const importsOffset =
        readU32LE(bytes, static_cast<std::size_t>(dataoff + 8));
    std::uint32_t const row0 =
        readU32LE(bytes, dataoff + importsOffset + 0 * 4);
    std::uint32_t const row1 =
        readU32LE(bytes, dataoff + importsOffset + 1 * 4);
    EXPECT_EQ(row0 & 0xFFu, 1u)
        << "_alpha libOrdinal must be 1 (libSystem is loadDylibs[0])";
    EXPECT_EQ(row1 & 0xFFu, 1u)
        << "_beta libOrdinal must also be 1 (same dylib) — catches "
           "dylibOrdinal mis-mapping for multi-import path";
    EXPECT_EQ((row0 >> 8) & 0x1u, 0u)
        << "weakImport=false hardcode (D-LK6-14-MACHO-WEAK-DEF) "
           "must not flip on chained-fixups walker path";
    EXPECT_EQ((row1 >> 8) & 0x1u, 0u);
    EXPECT_EQ(row0 >> 9, 1u)
        << "_alpha name_offset = 1 (NUL sentinel at 0)";
    EXPECT_EQ(row1 >> 9, 8u)
        << "_beta name_offset = 8 (NUL + '_alpha' + NUL = 7 bytes)";
}

// 8aabc04 audit fold (test-analyzer + test-analyzer-dim-2 HIGH):
// pin the D-LK6-14-NAME-OFFSET-OVERFLOW guard. Encode with a
// cumulative symbols pool that exceeds the 23-bit name_offset
// field (8 MiB - 1) and assert the walker fails loud with
// K_SymbolUndefined citing the anchor.
TEST(MachOExecWriter, ChainedFixupsNameOffsetOverflowFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    Relocation rel;
    rel.offset = 1; rel.target = SymbolId{99};
    rel.kind = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));
    // Single extern with a mangledName at the field ceiling. Pool
    // accounting: 1 (NUL sentinel) + name.size() + 1 (NUL terminator).
    // Push the name to exactly (1 << 23) bytes — total pool = 1 + (1<<23) + 1
    // = 8 MiB + 2, which exceeds the (1 << 23) - 1 ceiling.
    std::string longName(static_cast<std::size_t>(1) << 23, 'x');
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, std::move(longName),
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    EXPECT_TRUE(bytes.empty())
        << "name-offset overflow must abort emission";
    bool sawAnchor = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined
         && d.actual.find("D-LK6-14-NAME-OFFSET-OVERFLOW") != std::string::npos) {
            sawAnchor = true;
        }
    }
    EXPECT_TRUE(sawAnchor)
        << "walker MUST emit K_SymbolUndefined citing "
           "D-LK6-14-NAME-OFFSET-OVERFLOW so operators can either "
           "reduce import-name length OR fall back to LC_DYLD_INFO_ONLY";
}

TEST(MachOExecFormatJson, BindNowDefaultsToTrue) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-bindnow-default","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE((*r)->machoImage().bindNow);
}

TEST(MachOExecFormatJson, PageZeroSizeMustBePowerOfTwo) {
    // Walker depends on pageZeroSize being page-aligned (vmaddr =
    // pageZeroSize so mmap-congruence requires it). Validate must
    // reject non-power-of-two values. (pr-test-analyzer Gap 1 fold
    // — LK6 cycle 2c post-fold review.)
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-bad-pagezero","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 12884901888,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":17179869184}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecWriter, TextSegmentFilesizeCoversStubsEnd) {
    // CRITICAL anti-regression for the __TEXT.filesize fix
    // (code-reviewer C1 fold). The buggy formula was
    // `stubsEnd - textFileOff` (would have truncated dyld's mmap
    // before reaching .text); the correct formula is `stubsEnd`
    // since __TEXT.fileoff = 0 by Apple convention.
    // (pr-test-analyzer Gap 3 fold — LK6 cycle 2c post-fold review.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Walk to LC_SEGMENT_64 __TEXT; read filesize + fileoff +
    // section_64{__text/__stubs} to compute the expected
    // (stubsFileOff + stubsFileSize) value, then assert filesize
    // matches.
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    bool foundText = false;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::string segName(
                reinterpret_cast<char const*>(&bytes[off + 8]),
                strnlen(reinterpret_cast<char const*>(
                            &bytes[off + 8]), 16));
            if (segName == "__TEXT") {
                std::uint64_t fileOff = 0, fileSize = 0;
                for (int b = 0; b < 8; ++b) {
                    fileOff |= static_cast<std::uint64_t>(
                                   bytes[off + 40 + b]) << (b * 8);
                    fileSize |= static_cast<std::uint64_t>(
                                    bytes[off + 48 + b]) << (b * 8);
                }
                // __TEXT.fileoff = 0 (Apple convention — mach
                // header lives inside __TEXT).
                EXPECT_EQ(fileOff, 0u);
                // Find __stubs section_64 to compute stubsEnd.
                std::uint32_t nsects =
                    static_cast<std::uint32_t>(bytes[off+64]) |
                    (static_cast<std::uint32_t>(bytes[off+65]) << 8) |
                    (static_cast<std::uint32_t>(bytes[off+66]) << 16) |
                    (static_cast<std::uint32_t>(bytes[off+67]) << 24);
                std::size_t secOff = off + 72;
                std::uint64_t stubsEnd = 0;
                for (std::uint32_t s = 0; s < nsects; ++s) {
                    std::string secName(
                        reinterpret_cast<char const*>(&bytes[secOff]),
                        strnlen(reinterpret_cast<char const*>(
                                    &bytes[secOff]), 16));
                    std::uint64_t secSize = 0;
                    for (int b = 0; b < 8; ++b)
                        secSize |= static_cast<std::uint64_t>(
                                       bytes[secOff + 40 + b]) << (b*8);
                    std::uint32_t secFileOff =
                        static_cast<std::uint32_t>(bytes[secOff + 48]) |
                        (static_cast<std::uint32_t>(bytes[secOff + 49]) << 8) |
                        (static_cast<std::uint32_t>(bytes[secOff + 50]) << 16) |
                        (static_cast<std::uint32_t>(bytes[secOff + 51]) << 24);
                    if (secName == "__stubs") {
                        stubsEnd = static_cast<std::uint64_t>(secFileOff) + secSize;
                    }
                    secOff += 80;
                }
                ASSERT_NE(stubsEnd, 0u);
                EXPECT_EQ(fileSize, stubsEnd)
                    << "__TEXT.filesize must equal stubsEnd "
                       "(buggy formula was stubsEnd - textFileOff "
                       "which would have truncated dyld's mmap)";
                foundText = true;
                break;
            }
        }
        off += cmdsize;
    }
    EXPECT_TRUE(foundText);
}

TEST(MachOExecFormatJson, BindNowTypeCheckRejectsNonBoolean) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-bindnow-wrong","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": ["/usr/lib/libSystem.B.dylib"],
        "bindNow": "true"
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecWriter, MultipleExternsInTwoLibrariesEmitTwoLcLoadDylibRefs) {
    // 2-extern × 2-library smoke test — both dylib paths appear
    // in the file (both LC_LOAD_DYLIB strings + both bind opcode
    // dylib ordinals).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"macho-two-libs","kind":"macho"},
      "entryPoint": "",
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": "execute", "flags": 2097285 },
      "image": {
        "pageZeroSize": 4294967296,
        "dylinkerPath": "/usr/lib/dyld",
        "loadDylibs": [
          "/usr/lib/libSystem.B.dylib",
          "/usr/lib/libobjc.A.dylib"
        ]
      },
      "sections":[
        {"kind":"text","name":"__text","segment":"__TEXT","type":2147484672,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":4294971392}
      ],
      "relocations":[
        {"name":"X86_64_RELOC_BRANCH","kind":1,"nativeId":369098752},
        {"name":"X86_64_RELOC_UNSIGNED_8","kind":2,"nativeId":100663296},
        {"name":"X86_64_RELOC_UNSIGNED_4","kind":3,"nativeId":33554432}
      ]
    })");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        {1, SymbolId{99}, RelocationKind{1}, 0});
    fn.relocations.push_back(
        {6, SymbolId{100}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    mod.externImports.push_back(
        ExternImport{SymbolId{100}, "_objc_msgSend",
                     "/usr/lib/libobjc.A.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::string_view fv{
        reinterpret_cast<char const*>(bytes.data()), bytes.size()};
    EXPECT_NE(fv.find("/usr/lib/libSystem.B.dylib"),
              std::string_view::npos);
    EXPECT_NE(fv.find("/usr/lib/libobjc.A.dylib"),
              std::string_view::npos);
    EXPECT_NE(fv.find("_printf"), std::string_view::npos);
    EXPECT_NE(fv.find("_objc_msgSend"), std::string_view::npos);
}

TEST(MachOExecWriter, BindStreamEmitsExpectedOpcodeShape) {
    // Pin the bind opcode stream byte shape so a future regression
    // in IMM/ULEB threshold, trailing NUL, opcode ordering, or
    // segment-index miswiring shows up as a test failure rather
    // than a dyld load-time crash (pr-test-analyzer FOLD-NOW #1).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Walk to LC_DYLD_INFO_ONLY: header at +32, scan load cmds.
    // Header layout: magic(4) cputype(4) cpusubtype(4) filetype(4)
    //                ncmds(4) sizeofcmds(4) flags(4) reserved(4)
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    std::uint64_t bindOff = 0, bindSize = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x80000022u) {
            // dyld_info_command: cmd(4) cmdsize(4) rebase_off(4)
            // rebase_size(4) bind_off(4) bind_size(4) ...
            bindOff =
                static_cast<std::uint64_t>(bytes[off+16]) |
                (static_cast<std::uint64_t>(bytes[off+17]) << 8) |
                (static_cast<std::uint64_t>(bytes[off+18]) << 16) |
                (static_cast<std::uint64_t>(bytes[off+19]) << 24);
            bindSize =
                static_cast<std::uint64_t>(bytes[off+20]) |
                (static_cast<std::uint64_t>(bytes[off+21]) << 8) |
                (static_cast<std::uint64_t>(bytes[off+22]) << 16) |
                (static_cast<std::uint64_t>(bytes[off+23]) << 24);
            break;
        }
        off += cmdsize;
    }
    ASSERT_GT(bindSize, 0u);
    ASSERT_LE(bindOff + bindSize, bytes.size());
    // Expected stream prefix: SET_DYLIB_ORDINAL_IMM | 1 (lib #1),
    // SET_SYMBOL_TRAILING_FLAGS_IMM | 0, "_printf\0",
    // SET_TYPE_IMM | BIND_TYPE_POINTER (1),
    // SET_SEGMENT_AND_OFFSET_ULEB | 2 (kSegIdxDataConst), ULEB(0),
    // DO_BIND, DONE.
    std::vector<std::uint8_t> expected = {
        0x10u | 1u,               // SET_DYLIB_ORDINAL_IMM | 1
        0x40u | 0u,               // SET_SYMBOL_TRAILING_FLAGS_IMM
        '_','p','r','i','n','t','f', 0,
        0x50u | 1u,               // SET_TYPE_IMM | BIND_TYPE_POINTER
        0x70u | 2u,               // SET_SEGMENT_AND_OFFSET_ULEB | 2
        0x00u,                    // ULEB128(0) — offset 0 into __got
        0x90u,                    // DO_BIND
        0x00u,                    // DONE
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(bytes[bindOff + i], expected[i])
            << "bind-stream byte " << i << " mismatch";
    }
}

TEST(MachOExecWriter, StubDispPointsAtGotSlot) {
    // Pin the FF 25 disp32 → __got slot arithmetic. Cycle-2c
    // correctness depends on disp32 = gotSlotVa - (stubVa + 6).
    // (pr-test-analyzer FOLD-NOW #2.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    // Walk LCs to find __stubs section (in __TEXT segment) and
    // __got section (in __DATA_CONST). Each section_64 carries
    // addr(u64) + size(u64) + offset(u32). For each LC_SEGMENT_64
    // we read nsects from segment_command_64 (at +64 from cmd
    // start). Section_64 records follow each LC_SEGMENT_64 header.
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    std::uint64_t stubsAddr = 0, stubsFileOff = 0;
    std::uint64_t gotAddr = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::uint32_t nsects =
                static_cast<std::uint32_t>(bytes[off+64]) |
                (static_cast<std::uint32_t>(bytes[off+65]) << 8) |
                (static_cast<std::uint32_t>(bytes[off+66]) << 16) |
                (static_cast<std::uint32_t>(bytes[off+67]) << 24);
            std::size_t secOff = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s) {
                std::string secName(
                    reinterpret_cast<char const*>(&bytes[secOff]),
                    strnlen(reinterpret_cast<char const*>(
                                &bytes[secOff]), 16));
                std::uint64_t addr = 0;
                for (int b = 0; b < 8; ++b)
                    addr |= static_cast<std::uint64_t>(
                                bytes[secOff + 32 + b]) << (b*8);
                std::uint32_t fileOff =
                    static_cast<std::uint32_t>(bytes[secOff + 48]) |
                    (static_cast<std::uint32_t>(bytes[secOff + 49]) << 8) |
                    (static_cast<std::uint32_t>(bytes[secOff + 50]) << 16) |
                    (static_cast<std::uint32_t>(bytes[secOff + 51]) << 24);
                if (secName == "__stubs") {
                    stubsAddr = addr; stubsFileOff = fileOff;
                } else if (secName == "__got") {
                    gotAddr = addr;
                }
                secOff += 80;
            }
        }
        off += cmdsize;
    }
    ASSERT_NE(stubsAddr, 0u);
    ASSERT_NE(gotAddr, 0u);
    ASSERT_NE(stubsFileOff, 0u);
    // Stub byte 0..1 = FF 25; bytes 2..5 = disp32 (LE).
    EXPECT_EQ(bytes[stubsFileOff], 0xFFu);
    EXPECT_EQ(bytes[stubsFileOff + 1], 0x25u);
    std::int32_t disp =
        static_cast<std::int32_t>(
            static_cast<std::uint32_t>(bytes[stubsFileOff + 2]) |
            (static_cast<std::uint32_t>(bytes[stubsFileOff + 3]) << 8) |
            (static_cast<std::uint32_t>(bytes[stubsFileOff + 4]) << 16) |
            (static_cast<std::uint32_t>(bytes[stubsFileOff + 5]) << 24));
    // (stubAddr + 6) + disp = gotAddr (slot #0).
    EXPECT_EQ(
        static_cast<std::uint64_t>(
            static_cast<std::int64_t>(stubsAddr) + 6 + disp),
        gotAddr);
}

TEST(MachOExecWriter, DysymtabIundefsymNundefsymCorrect) {
    // LC_DYSYMTAB.iundefsym / nundefsym must agree with nlist
    // layout: defined externs first, undefined externs next. If
    // these drift, dyld binds the wrong symbol slots.
    // (pr-test-analyzer FOLD-NOW #3.)
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back({1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_printf",
                     "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::uint32_t ncmds =
        static_cast<std::uint32_t>(bytes[16]) |
        (static_cast<std::uint32_t>(bytes[17]) << 8) |
        (static_cast<std::uint32_t>(bytes[18]) << 16) |
        (static_cast<std::uint32_t>(bytes[19]) << 24);
    std::size_t off = 32;
    std::uint32_t iundefsym = 0xFFFFFFFFu, nundefsym = 0;
    for (std::uint32_t i = 0; i < ncmds; ++i) {
        std::uint32_t cmd =
            static_cast<std::uint32_t>(bytes[off]) |
            (static_cast<std::uint32_t>(bytes[off+1]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+2]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+3]) << 24);
        std::uint32_t cmdsize =
            static_cast<std::uint32_t>(bytes[off+4]) |
            (static_cast<std::uint32_t>(bytes[off+5]) << 8) |
            (static_cast<std::uint32_t>(bytes[off+6]) << 16) |
            (static_cast<std::uint32_t>(bytes[off+7]) << 24);
        if (cmd == 0x0Bu) {  // LC_DYSYMTAB
            // Field layout: cmd(4) cmdsize(4) ilocalsym(4)
            // nlocalsym(4) iextdefsym(4) nextdefsym(4)
            // iundefsym(4) nundefsym(4) ...
            iundefsym =
                static_cast<std::uint32_t>(bytes[off+24]) |
                (static_cast<std::uint32_t>(bytes[off+25]) << 8) |
                (static_cast<std::uint32_t>(bytes[off+26]) << 16) |
                (static_cast<std::uint32_t>(bytes[off+27]) << 24);
            nundefsym =
                static_cast<std::uint32_t>(bytes[off+28]) |
                (static_cast<std::uint32_t>(bytes[off+29]) << 8) |
                (static_cast<std::uint32_t>(bytes[off+30]) << 16) |
                (static_cast<std::uint32_t>(bytes[off+31]) << 24);
            break;
        }
        off += cmdsize;
    }
    // 1 defined function + 1 undefined extern.
    EXPECT_EQ(iundefsym, 1u);
    EXPECT_EQ(nundefsym, 1u);
}

TEST(MachOExecWriter, UndeclaredDylibInExternImportFailsLoud) {
    // Defense-in-depth: extern.libraryPath must be present in
    // image.loadDylibs — dyld rejects bind opcodes whose ordinals
    // point at an undeclared dylib.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped(
        "macho64-x86_64-darwin-exec");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0, 0, 0, 0, 0xC3};
    fn.relocations.push_back(
        {1, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_undeclared",
                     "/usr/lib/libNotDeclared.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    ASSERT_GE(rep.errorCount(), 1u);
}

// ── D-CSUBSET-THREAD-LOCAL (TLS C4): Mach-O TLV structural pins ──────────
//
// THE HOST CANNOT EXECUTE MACH-O, so these byte-level structural pins are the
// SOLE automated guard on the TLV writer (the runtime witness is the user's
// Apple-Silicon Mac + the macos-latest CI leg). They decode a produced
// arm64-macho-exec image and assert the descriptor ABI (word0 bind / word1 key
// / word2 block offset), the 3 __thread_* sections + flags, the
// __tlv_bootstrap bind-per-descriptor, and the CRIT-1/M-3 fail-loud gates —
// each RED-ON-DISABLE (routing thread-locals through ordinary data, or dropping
// the bind, makes a pin fail while a single-thread runtime would still pass).
namespace {

struct MachoSecInfo {
    std::string name;
    std::uint64_t addr = 0, size = 0;
    std::uint32_t off = 0, flags = 0, segidx = 0;
};
struct MachoBind { std::string sym; std::uint32_t seg = 0; std::uint64_t off = 0; };
struct MachoDecoded {
    std::vector<std::uint64_t> segVmaddr;
    std::vector<MachoSecInfo>  sections;
    std::vector<MachoBind>     binds;
    [[nodiscard]] MachoSecInfo const* sec(std::string_view n) const {
        for (auto const& s : sections) if (s.name == n) return &s;
        return nullptr;
    }
};

[[nodiscard]] std::uint64_t readUleb(std::vector<std::uint8_t> const& b,
                                     std::size_t& i) {
    std::uint64_t r = 0; int s = 0;
    while (i < b.size()) {
        std::uint8_t c = b[i++]; r |= std::uint64_t(c & 0x7f) << s;
        if (!(c & 0x80)) break; s += 7;
    }
    return r;
}

[[nodiscard]] MachoDecoded decodeMacho(std::vector<std::uint8_t> const& d) {
    MachoDecoded out;
    std::uint32_t const ncmds = readU32LE(d, 16);
    std::size_t off = 32;
    for (std::uint32_t c = 0; c < ncmds; ++c) {
        std::uint32_t const cmd = readU32LE(d, off);
        std::uint32_t const cmdsize = readU32LE(d, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            std::uint64_t const vmaddr = readU64LE(d, off + 24);
            std::uint32_t const nsects = readU32LE(d, off + 64);
            std::uint32_t const segidx =
                static_cast<std::uint32_t>(out.segVmaddr.size());
            out.segVmaddr.push_back(vmaddr);
            std::size_t so = off + 72;
            for (std::uint32_t s = 0; s < nsects; ++s) {
                MachoSecInfo si;
                char nm[17] = {0};
                std::memcpy(nm, d.data() + so, 16);
                si.name = nm;
                si.addr = readU64LE(d, so + 32);
                si.size = readU64LE(d, so + 40);
                si.off = readU32LE(d, so + 48);
                si.flags = readU32LE(d, so + 64);
                si.segidx = segidx;
                out.sections.push_back(std::move(si));
                so += 80;
            }
        } else if (cmd == 0x80000022u) {  // LC_DYLD_INFO_ONLY
            // dyld_info_command: cmd/cmdsize/rebase(8,12)/bind_off(16)/bind_size(20).
            std::uint32_t const bindOff = readU32LE(d, off + 16);
            std::uint32_t const bindSize = readU32LE(d, off + 20);
            std::size_t i = bindOff;
            std::string sym; std::uint32_t seg = 0; std::uint64_t o = 0;
            while (i < std::size_t(bindOff) + bindSize) {
                std::uint8_t const op = d[i++]; std::uint8_t const hi = op & 0xF0u;
                if (hi == 0x10u) { /* ORD_IMM */ }
                else if (hi == 0x20u) { (void)readUleb(d, i); }
                else if (hi == 0x40u) {
                    sym.clear();
                    while (i < d.size() && d[i] != 0) sym.push_back(char(d[i++]));
                    ++i;  // NUL
                } else if (hi == 0x50u) { /* SET_TYPE */ }
                else if (hi == 0x70u) { seg = op & 0x0Fu; o = readUleb(d, i); }
                else if (hi == 0x90u) { out.binds.push_back({sym, seg, o}); }
                else if (op == 0x00u) { /* DONE / pad */ }
            }
        }
        off += cmdsize;
    }
    return out;
}

// Extern (_exit) forces encodeExecDynamic (the only __DATA/TLV path). Optional
// tbss int + a configurable tdata alignment for the M-3 over-align pin.
[[nodiscard]] AssembledModule buildMachoTlvModule(bool withTbss,
                                                  std::uint64_t tdataAlign = 4) {
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};  // BL _exit ; RET
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_exit", "/usr/lib/libSystem.B.dylib"});
    AssembledData g;  // thread_local int g = 7;  → tdata, word2 = 0
    g.symbol = SymbolId{50};
    g.section = DataSectionKind::Tdata;
    g.bytes = {7, 0, 0, 0};
    g.alignment = Alignment::ofRuntimePow2(tdataAlign);
    mod.dataItems.push_back(std::move(g));
    if (withTbss) {
        AssembledData c;  // static thread_local int c; → tbss, word2 = tbssBlockBase
        c.symbol = SymbolId{51};
        c.section = DataSectionKind::Tbss;
        c.reservedSize = 4;
        c.alignment = Alignment::ofRuntimePow2(4);
        mod.dataItems.push_back(std::move(c));
    }
    return mod;
}

struct MachoTlvLoaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};
[[nodiscard]] MachoTlvLoaded loadArm64MachoExec() {
    MachoTlvLoaded out;
    auto t = TargetSchema::loadShipped("arm64");
    if (t.has_value()) out.target = std::move(t).value(); else ADD_FAILURE();
    auto f = ObjectFormatSchema::loadShipped("macho64-arm64-darwin-exec");
    if (f.has_value()) out.format = std::move(f).value(); else ADD_FAILURE();
    return out;
}

[[nodiscard]] bool sawDiag(DiagnosticReporter const& rep, DiagnosticCode code) {
    for (auto const& d : rep.all()) if (d.code == code) return true;
    return false;
}

}  // namespace

TEST(MachOTlvWriter, ThreeThreadSectionsCarryStdThreadLocalFlags) {
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto dec = decodeMacho(bytes);
    auto const* tv = dec.sec("__thread_vars");
    auto const* td = dec.sec("__thread_data");
    auto const* tb = dec.sec("__thread_bss");
    ASSERT_NE(tv, nullptr); ASSERT_NE(td, nullptr); ASSERT_NE(tb, nullptr);
    // dyld keys the TLV template on these EXACT section types.
    EXPECT_EQ(tv->flags & 0xffu, 0x13u);  // S_THREAD_LOCAL_VARIABLES
    EXPECT_EQ(td->flags & 0xffu, 0x11u);  // S_THREAD_LOCAL_REGULAR
    EXPECT_EQ(tb->flags & 0xffu, 0x12u);  // S_THREAD_LOCAL_ZEROFILL
    EXPECT_EQ(tb->off, 0u) << "__thread_bss is zero-fill: no file bytes";
    // __thread_data and __thread_bss must be CONTIGUOUS (one dyld TLV region).
    EXPECT_LE(td->addr + td->size, tb->addr);
    EXPECT_EQ(tb->addr, td->addr + 4u)
        << "tbss must sit at tdataVA + tbssBlockBase (CRIT-2 contiguity)";
}

TEST(MachOTlvWriter, DescriptorWord2IsZeroBasedBlockOffset) {
    // ★ CRIT-2 byte pin: a 2-var block (tdata int @0 + tbss int @4). word0/word1
    // are 0 (dyld binds/fills them); word2 is the 0-BASED block offset — NOT the
    // arm64-ELF Variant-I 16+off (which would put every word2 at +16 → garbage).
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto dec = decodeMacho(bytes);
    auto const* tv = dec.sec("__thread_vars");
    ASSERT_NE(tv, nullptr);
    ASSERT_EQ(tv->size, 2u * 24u) << "2 descriptors × 24 bytes";
    // desc[0] = tdata g → word2 0; desc[1] = tbss c → word2 4 (tbssBlockBase).
    for (int i = 0; i < 2; ++i) {
        std::uint64_t const w0 = readU64LE(bytes, tv->off + std::size_t(i) * 24 + 0);
        std::uint64_t const w1 = readU64LE(bytes, tv->off + std::size_t(i) * 24 + 8);
        EXPECT_EQ(w0, 0u) << "desc[" << i << "].word0 must be 0 (dyld binds thunk)";
        EXPECT_EQ(w1, 0u) << "desc[" << i << "].word1 must be 0 (key)";
    }
    std::uint64_t const w2a = readU64LE(bytes, tv->off + 16);
    std::uint64_t const w2b = readU64LE(bytes, tv->off + 24 + 16);
    EXPECT_EQ(w2a, 0u) << "tdata var's block offset is 0";
    EXPECT_EQ(w2b, 4u) << "tbss var's block offset is tbssBlockBase=4, not 16+off";
}

TEST(MachOTlvWriter, BootstrapBindPerDescriptorTargetsWord0) {
    // ★ CRIT-3: exactly one bind of `__tlv_bootstrap` (TWO underscores — the
    // on-disk libSystem symbol) per descriptor, at that descriptor's word0
    // (offset i*24 in the __thread_vars-bearing __DATA segment). A missing bind
    // = a `blr` through garbage on the Mac. The `bind.seg == __thread_vars
    // segment index` check below is the TEST-side guard on the segment index;
    // the writer ALSO fails loud (audit FOLD-1: it cross-checks the ACTUAL
    // emitted __DATA index against the value the binds use) so a segment-layout
    // change can never SILENTLY bind to the wrong segment on this
    // can't-run-macho host.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto dec = decodeMacho(bytes);
    auto const* tv = dec.sec("__thread_vars");
    ASSERT_NE(tv, nullptr);
    std::vector<MachoBind> tlv;
    for (auto const& b : dec.binds)
        if (b.sym == "__tlv_bootstrap") tlv.push_back(b);
    ASSERT_EQ(tlv.size(), 2u) << "one __tlv_bootstrap bind per descriptor";
    std::uint64_t const segVa = dec.segVmaddr[tv->segidx];
    std::uint64_t const tvOffInSeg = tv->addr - segVa;
    for (std::size_t i = 0; i < tlv.size(); ++i) {
        EXPECT_EQ(tlv[i].seg, tv->segidx)
            << "bind targets the __thread_vars-bearing segment";
        EXPECT_EQ(tlv[i].off, tvOffInSeg + i * 24u)
            << "bind offset is descriptor i's word0 (i*24)";
    }
}

TEST(MachOTlvWriter, TlsImageHeaderAdvertisesTlvDescriptorsFlagRuntimeClosure) {
    // ★ TLS C4 runtime-closure red-on-disable (the host-independent guard for the
    // gap the arm64 witness caught): a TLS-bearing image's mach_header_64.flags
    // MUST carry MH_HAS_TLV_DESCRIPTORS (0x00800000). The correct S_THREAD_LOCAL_*
    // section types + per-descriptor __tlv_bootstrap binds (pinned above) are
    // NECESSARY BUT NOT SUFFICIENT — dyld runs the TLV setup that rewrites each
    // descriptor's word0 thunk to the real tlv_get_addr ONLY when the HEADER
    // advertises this bit. Without it, dyld leaves word0 = __tlv_bootstrap, whose
    // thunk aborts the instant a thread_local is read → runtime SIGABRT
    // (`_tlv_bootstrap_error`). That is invisible to a byte-pin over sections/binds
    // and to any non-Mac host; this pin fails on EVERY leg if the writer drops the
    // flag, so the regression can never re-hide behind green byte-pins again.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 28u);
    // mach_header_64.flags @ +24 (magic/cputype/cpusubtype/filetype/ncmds/sizeofcmds).
    std::uint32_t const flags = readU32LE(bytes, 24);
    EXPECT_NE(flags & 0x00800000u, 0u)
        << "TLS image must set MH_HAS_TLV_DESCRIPTORS or dyld skips TLV setup "
           "→ runtime SIGABRT via _tlv_bootstrap_error on native execution";
    // The base format flags (NOUNDEFS|DYLDLINK|TWOLEVEL|PIE = 0x200085) survive —
    // the TLV bit is ADDED to them, never replaces them.
    EXPECT_EQ(flags & 0x00200085u, 0x00200085u)
        << "base MH flags preserved alongside the TLV bit";
}

TEST(MachOTlvWriter, AddressOfThreadLocalInDataItemFailsLoudCrit1) {
    // ★ CRIT-1 arm-(a) red-on-disable: a DATA-item reloc targeting a thread-local
    // symbol embeds the descriptor VA as a process-shared pointer. The macho
    // walker backstop rejects it (the semantic tier's 0xE048 is the front line;
    // this is the writer belt). Disabling the tlsDataSymbols backstop makes this
    // link clean with a garbage pointer.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/false);
    RelocationKind abs64{0}; bool found = false;
    for (auto const& r : L.target->relocations())
        if (r.widthBytes == 8 && !r.pcRelative) { abs64 = r.kind; found = true; break; }
    ASSERT_TRUE(found);
    // `int *p = &g;` — a __data pointer with an abs64 reloc → the tls sym 50.
    AssembledData p;
    p.symbol = SymbolId{60};
    p.section = DataSectionKind::Data;
    p.bytes.assign(8, 0);
    p.alignment = Alignment::ofRuntimePow2(8);
    p.relocations.push_back(Relocation{0u, SymbolId{50}, abs64, 0});
    mod.dataItems.push_back(std::move(p));
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_TRUE(sawDiag(rep, DiagnosticCode::K_RelocationKindMismatch))
        << "a data-item reloc targeting a thread-local must fail loud (CRIT-1)";
}

TEST(MachOTlvWriter, PlainThreadLocalLinksCleanCrit1Positive) {
    // ★ CRIT-1 positive: the ordinary access reloc (adrp/add vs a TLS sym, kinds
    // 2/3) must NOT trip the backstop — a plain thread_local links clean. macho
    // does NOT register tls syms in a tlsSymbols set / run the reloc-XOR arm (b);
    // if it did, this legitimate access reloc would be false-rejected.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/true);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(bytes.empty());
}

TEST(MachOTlvWriter, OverAlignedThreadLocalFailsLoudM3) {
    // ★ M-3 red-on-disable: a thread-local needing > 16-byte alignment cannot be
    // guaranteed by dyld's malloc'd TLV block base; fail loud
    // (K_ThreadLocalOveralignedForFormat) rather than silently under-align.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/false, /*tdataAlign=*/32);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    EXPECT_TRUE(bytes.empty());
    EXPECT_TRUE(sawDiag(rep, DiagnosticCode::K_ThreadLocalOveralignedForFormat));
}

TEST(MachOTlvWriter, SixteenByteAlignedThreadLocalLinksCleanM3) {
    // _Alignas(16) (== the block-base guarantee) must PASS — the gate bites only
    // strictly-over-16 alignment.
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    auto mod = buildMachoTlvModule(/*withTbss=*/false, /*tdataAlign=*/16);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    EXPECT_FALSE(bytes.empty());
}

TEST(MachOTlvWriter, NoThreadLocalEmitsNoThreadSectionsSqliteDormant) {
    // The disable-and-red control: a module with NO thread-local items emits
    // ZERO __thread_* sections + ZERO __tlv_bootstrap binds — the pre-TLS image
    // is byte-untouched (sqlite / ordinary output unaffected).
    auto L = loadArm64MachoExec();
    ASSERT_TRUE(L.target && L.format);
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0x00, 0x00, 0x00, 0x94, 0xC0, 0x03, 0x5F, 0xD6};
    fn.relocations.push_back(Relocation{0u, SymbolId{99}, RelocationKind{1}, 0});
    mod.functions.push_back(std::move(fn));
    mod.externImports.push_back(
        ExternImport{SymbolId{99}, "_exit", "/usr/lib/libSystem.B.dylib"});
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *L.target, *L.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto dec = decodeMacho(bytes);
    EXPECT_EQ(dec.sec("__thread_vars"), nullptr);
    EXPECT_EQ(dec.sec("__thread_data"), nullptr);
    EXPECT_EQ(dec.sec("__thread_bss"), nullptr);
    for (auto const& b : dec.binds)
        EXPECT_NE(b.sym, "__tlv_bootstrap");
    // MH_HAS_TLV_DESCRIPTORS is content-conditional (set iff the module carries
    // thread-locals — clang's rule), NOT a blanket format-JSON flag: a no-TLS
    // image must NOT advertise it. This is the negative twin of the runtime-
    // closure pin — it goes red if the TLV bit is ever made unconditional.
    EXPECT_EQ(readU32LE(bytes, 24) & 0x00800000u, 0u)
        << "no-TLS image must NOT set MH_HAS_TLV_DESCRIPTORS";
}
