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
#include "link_test_support.hpp"
#include "macho_test_support.hpp"

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
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 0), 0xFEEDFACFu);
    // cputype = CPU_TYPE_X86_64 = 0x01000007
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 4), 0x01000007u);
    // cpusubtype = CPU_SUBTYPE_X86_64_ALL = 3
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 8), 3u);
    // filetype = MH_OBJECT = 1
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 12), 1u);
    // ncmds = 2 (LC_SEGMENT_64 + LC_SYMTAB)
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 16), 2u);
    // sizeofcmds = 72 + 80*1 (segment+section) + 24 (symtab) = 176
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 20), 176u);
    // flags = 0 (MH_SUBSECTIONS_VIA_SYMBOLS deliberately NOT set
    // because cycle 1 emits a flat __text without subsection markers;
    // anchored as D-LK3-2 for the future subsection-emit cycle)
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 24), 0u);
    // reserved = 0
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 28), 0u);
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
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 32), 0x19u);  // LC_SEGMENT_64
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 36), 72u + 80u);  // cmdsize
    // segname (16 bytes) starts at 40 — empty for MH_OBJECT
    for (std::size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(bytes[40 + i], 0u) << "LC_SEGMENT_64 segname must be empty";
    }
    // nsects @ +72+64 = +136 (segment_command_64 fields:
    // cmd(4)+cmdsize(4)+segname(16)+vmaddr(8)+vmsize(8)+fileoff(8)+
    // filesize(8)+maxprot(4)+initprot(4)+nsects(4)+flags(4)).
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 32 + 64), 1u);  // nsects = 1

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
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 168), 0x80000400u);
}

// ── LC_SYMTAB locates symbol + string tables ───────────────────

TEST(MachOWriter, LcSymtabReferencesNlist64AndStringTable) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // LC_SYMTAB starts at byte 32 + 72 + 80 = 184
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 184), 0x02u);  // LC_SYMTAB
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 188), 24u);    // cmdsize
    std::uint32_t const symoff = dss::link_format::test::readU32LE(bytes, 192);
    std::uint32_t const nsyms = dss::link_format::test::readU32LE(bytes, 196);
    std::uint32_t const stroff = dss::link_format::test::readU32LE(bytes, 200);
    std::uint32_t const strsize = dss::link_format::test::readU32LE(bytes, 204);
    EXPECT_EQ(nsyms, 1u);
    EXPECT_GT(symoff, 0u);
    EXPECT_LE(symoff + 16u, bytes.size());

    // nlist_64 record: n_strx(u32) + n_type(u8) + n_sect(u8) +
    // n_desc(u16) + n_value(u64). Total 16 bytes.
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, symoff + 0), 1u)
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
    std::uint32_t const relocOff = dss::link_format::test::readU32LE(bytes, 160);
    std::uint32_t const relocCount = dss::link_format::test::readU32LE(bytes, 164);
    ASSERT_EQ(relocCount, 1u);
    ASSERT_GT(relocOff, 0u);
    ASSERT_LE(relocOff + 8u, bytes.size());

    // r_address = 1 (patch site within .text)
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, relocOff + 0), 1u);
    // r_info bits: type(28..31)=2(BRANCH); extern(27)=1; length(25..26)=2;
    // pcrel(24)=1; symbolnum(0..23) = symtab index of target.
    std::uint32_t const rInfo = dss::link_format::test::readU32LE(bytes, relocOff + 4);
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
    std::uint32_t const symoff = dss::link_format::test::readU32LE(bytes, 184 + 8);
    std::uint32_t const nsyms  = dss::link_format::test::readU32LE(bytes, 184 + 12);
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
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 12), 2u);
    // flags @ +24 contains MH_PIE (0x200000) bit.
    EXPECT_NE(dss::link_format::test::readU32LE(bytes, 24) & 0x200000u, 0u);
}

TEST(MachOExecWriter, PageZeroSegmentEmittedFirst) {
    auto loaded = loadShippedExec();
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // First load command at offset 32 is LC_SEGMENT_64 (0x19).
    ASSERT_GE(bytes.size(), 32u + 72u);
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, 32), 0x19u);
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
        std::uint32_t const cmd     = dss::link_format::test::readU32LE(bytes, off);
        std::uint32_t const cmdsize = dss::link_format::test::readU32LE(bytes, off + 4);
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
        std::uint32_t const cmd     = dss::link_format::test::readU32LE(bytes, off);
        std::uint32_t const cmdsize = dss::link_format::test::readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x19u) {  // LC_SEGMENT_64
            ++segIdx;
            if (segIdx == 2) {
                // section_64 starts at off + 72; section.offset @ +96.
                textFileOff = dss::link_format::test::readU32LE(bytes, off + 72 + 48);
                break;
            }
        }
        off += cmdsize;
    }
    ASSERT_NE(textFileOff, 0u);
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    EXPECT_EQ(dss::link_format::test::readU32LE(bytes, textFileOff + 1), 1u);
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

TEST(MachOExecFormatJsonValidate, ObjWithBindNowFalseRejected) {
    // Symmetric reject: MH_OBJECT must not set image.bindNow=false.
    // Eager-vs-lazy is an exec-image concept; .o files do not bind
    // at all (the linker resolves at exec build time). Without this
    // rule a JSON typo would silently load. (Type-design HIGH fold,
    // LK6 cycle 2c post-fold review.)
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
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
        std::uint32_t const cmd     = dss::link_format::test::readU32LE(bytes, off);
        std::uint32_t const cmdsize = dss::link_format::test::readU32LE(bytes, off + 4);
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
                EXPECT_EQ(dss::link_format::test::readU32LE(bytes, off + 64), 1u);
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
        std::uint32_t const cmd     = dss::link_format::test::readU32LE(bytes, off);
        std::uint32_t const cmdsize = dss::link_format::test::readU32LE(bytes, off + 4);
        if (cmdsize == 0) break;
        if (cmd == 0x0Cu) {  // LC_LOAD_DYLIB
            // name offset @ +8 must be 24 (cmd+24)
            EXPECT_EQ(dss::link_format::test::readU32LE(bytes, off + 8), 24u);
            // timestamp / current_version / compat_version @ +12/+16/+20
            // are 0 today (reserved for future cycle).
            EXPECT_EQ(dss::link_format::test::readU32LE(bytes, off + 12), 0u);
            EXPECT_EQ(dss::link_format::test::readU32LE(bytes, off + 16), 0u);
            EXPECT_EQ(dss::link_format::test::readU32LE(bytes, off + 20), 0u);
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
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(*lcOff + 4));
    EXPECT_EQ(cmdsize, 16u)
        << "linkedit_data_command shape: cmd+cmdsize+dataoff+datasize "
           "= 4+4+4+4 = 16 bytes";
    // dataoff must lie within the buffer + datasize must be non-zero
    // (we have 1 import → at least 1 byte name + NUL + header + starts
    // + import row).
    std::uint32_t const dataoff =
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    std::uint32_t const datasize =
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(*lcOff + 12));
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
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    // dyld_chained_fixups_header layout:
    //   [ 0.. 3] fixups_version
    //   [ 4.. 7] starts_offset
    //   [ 8..11] imports_offset
    //   [12..15] symbols_offset
    //   [16..19] imports_count
    std::uint32_t const importsCount =
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(dataoff + 16));
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
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    std::uint32_t const symbolsOffset =
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(dataoff + 12));
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
TEST(MachOExecWriter, ChainedFixupsKeepsLcDysymtab) {
    constexpr std::uint32_t kLcDysymtab = 0x0Bu;
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = loadChainedFixupsExecFormat();
    ASSERT_NE(fmt, nullptr);
    auto mod = chainedFixupsTestModule();
    DiagnosticReporter rep;
    auto bytes = macho::encode(mod, **target, *fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    EXPECT_TRUE(dss::macho::test::findLoadCommand(bytes, kLcDysymtab).has_value())
        << "LC_DYSYMTAB must STAY emitted on the chained path until "
           "D-LK6-14-INTEGRATION-GOT-SLOTS drops it together with "
           "__got slot bitfield population — a premature drop would "
           "leave the binary structurally broken without a failing "
           "test signal.";
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
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(*lcOff + 8));
    // Pool layout for {"_alpha", "_beta"}:
    //   [0]NUL  [1..6]"_alpha"[0]  [8..12]"_beta"[0]
    // Symbols offset in payload from header[12].
    std::uint32_t const symbolsOffset =
        dss::link_format::test::readU32LE(bytes, static_cast<std::size_t>(dataoff + 12));
    std::size_t const poolBase =
        static_cast<std::size_t>(dataoff) + symbolsOffset;
    EXPECT_EQ(bytes[poolBase + 0], 0u)         << "leading NUL sentinel";
    EXPECT_EQ(bytes[poolBase + 1], '_');
    EXPECT_EQ(bytes[poolBase + 6], 'a')        << "_alpha tail char";
    EXPECT_EQ(bytes[poolBase + 7], 0u)         << "NUL after _alpha";
    EXPECT_EQ(bytes[poolBase + 8], '_');
    EXPECT_EQ(bytes[poolBase + 12], 'a')       << "_beta tail char";
    EXPECT_EQ(bytes[poolBase + 13], 0u)        << "NUL after _beta";
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
