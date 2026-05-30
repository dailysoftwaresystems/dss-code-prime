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
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
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
[[nodiscard]] std::uint64_t readU64LE(std::span<std::uint8_t const> b,
                                       std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
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
    auto f = ObjectFormatSchema::loadShipped("macho64-x86_64-darwin");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(macho64-x86_64-darwin) failed";
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
      "format": {"name":"bad-macho","kind":"macho"},
      "macho": { "cputype": 0, "filetype": 1 }
    })");
    ASSERT_FALSE(r.has_value());
}

// ── Mach-O section row missing `segment` rejected ──────────────

TEST(MachOFormatJson, EmptySegmentRejectedByValidate) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
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
      "format": {"name":"bad-macho-va","kind":"macho"},
      "macho": { "cputype": 16777223, "filetype": 1 },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":4,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
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

// ── End-to-end via the format-blind link() dispatch ────────────

TEST(LinkerEndToEnd, MachODispatchProducesNonEmptyBytes) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
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
      "format": {"name":"obj-with-image","kind":"macho"},
      "macho": { "cputype": 16777223, "cpusubtype": 3, "filetype": 1, "flags": 0 },
      "image": { "pageZeroSize": 4294967296, "dylinkerPath": "/usr/lib/dyld", "loadDylibs": ["/usr/lib/libSystem.B.dylib"] },
      "sections":[{"kind":"text","name":"__text","segment":"__TEXT","type":0,"flags":0,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(MachOExecFormatJsonValidate, ExecMissingLoadDylibsRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
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

// ── LK6 cycle 2a: extern imports fail loud (D-LK6-5 pending) ───

TEST(MachOExecWriter, ExternImportsFailLoudPendingD_LK6_5) {
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
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}
