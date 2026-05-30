// ELF ET_EXEC writer tests — plan 14 LK1 cycle 2.
//
// Pins golden byte-level invariants of the emitted ELF64 executable
// (ET_EXEC):
//   * e_type = ET_EXEC (2).
//   * e_entry = secText.virtualAddress + entry-function offset
//     (cycle 2: first function at offset 0 in .text).
//   * e_phoff = sizeof(Ehdr) = 64; e_phentsize = 56; e_phnum = 1.
//   * PT_LOAD program header: p_type=1, p_flags=5 (R|X), p_align=0x1000,
//     p_vaddr/p_paddr = secText.virtualAddress, p_filesz/p_memsz =
//     .text length.
//   * section header `.text`: sh_addr = virtualAddress (vs 0 in ET_REL).
//   * Section ordering preserved (IDX_TEXT=1) so the symtab's
//     STT_SECTION entry's st_shndx=1 still resolves.
//   * Cycle-2 fail-loud guard: modules with non-empty relocations[]
//     emit `K_RelocationKindMismatch` and return empty bytes.
//   * Cycle-2 fail-loud guard: zero-function module emits
//     `K_SymbolUndefined`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/elf.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
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
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux-exec) failed";
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

// ── Shipped JSON loads with new fields ──────────────────────────

TEST(ElfExecFormatJson, ShippedFileLoadsCleanlyWithExecFields) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Elf);
    EXPECT_EQ(loaded.format->name(), "elf64-x86_64-linux-exec");
    EXPECT_EQ(loaded.format->elf().objectType, ElfObjectType::Exec);
    // Empty entryPoint until D-LK1-1 closes (real symbol-name
    // thread); walker defaults to module.functions[0].
    EXPECT_EQ(loaded.format->entryPoint(), "");
    auto const* secText = loaded.format->sectionByKind(SectionKind::Text);
    ASSERT_NE(secText, nullptr);
    EXPECT_EQ(secText->virtualAddress, 0x401000u);
}

// ── ET_EXEC golden header bytes ────────────────────────────────

TEST(ElfExecWriter, Elf64EhdrTypeIsETEXEC) {
    auto loaded = loadShipped();
    // Single function with no calls — a `ret` (0xC3).
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 64u);
    // e_type @ +16 = ET_EXEC (2)
    EXPECT_EQ(readU16LE(bytes, 16), 2u);
}

TEST(ElfExecWriter, EntryAddressDerivesFromVirtualAddress) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_entry @ +24 = secText.virtualAddress (0x401000) + 0 (first
    // function lives at .text offset 0 in cycle 2).
    EXPECT_EQ(readU64LE(bytes, 24), 0x401000u);
}

TEST(ElfExecWriter, ProgramHeaderTableImmediatelyFollowsEhdr) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_phoff @ +32 = 64 (right after Ehdr)
    EXPECT_EQ(readU64LE(bytes, 32), 64u);
    // e_phentsize @ +54 = 56 (sizeof Elf64_Phdr)
    EXPECT_EQ(readU16LE(bytes, 54), 56u);
    // e_phnum @ +56 = 1 (single PT_LOAD)
    EXPECT_EQ(readU16LE(bytes, 56), 1u);
}

TEST(ElfExecWriter, PtLoadHeaderHasReadExecutePermsAndPageAlign) {
    auto loaded = loadShipped();
    // 3-byte function: nop nop ret
    AssembledModule mod = makeTrivialModule({0x90, 0x90, 0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // Program header table at byte 64
    std::uint32_t const pType = readU32LE(bytes, 64);
    EXPECT_EQ(pType, 1u);  // PT_LOAD
    std::uint32_t const pFlags = readU32LE(bytes, 64 + 4);
    EXPECT_EQ(pFlags, 5u);  // PF_X | PF_R
    // p_offset @ +8 = file offset of .text (depends on layout, > 64+56)
    std::uint64_t const pOffset = readU64LE(bytes, 64 + 8);
    EXPECT_GE(pOffset, 64u + 56u);
    // p_vaddr / p_paddr @ +16, +24 = 0x401000
    EXPECT_EQ(readU64LE(bytes, 64 + 16), 0x401000u);
    EXPECT_EQ(readU64LE(bytes, 64 + 24), 0x401000u);
    // p_filesz / p_memsz @ +32, +40 = 3 (text length)
    EXPECT_EQ(readU64LE(bytes, 64 + 32), 3u);
    EXPECT_EQ(readU64LE(bytes, 64 + 40), 3u);
    // p_align @ +48 = 0x1000 (4KB page, kernel-required)
    EXPECT_EQ(readU64LE(bytes, 64 + 48), 0x1000u);
}

TEST(ElfExecWriter, TextSectionHeaderShAddrEqualsVirtualAddress) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_shoff @ +40
    std::uint64_t const shoff = readU64LE(bytes, 40);
    // Section header 1 is .text (header 0 is SHT_NULL). Each header
    // is 64 bytes. sh_addr @ +16 within the header.
    std::uint64_t const textShAddr = readU64LE(bytes, shoff + 64 + 16);
    EXPECT_EQ(textShAddr, 0x401000u);
}

// ── Section ordering preserved (.rela.text slot is SHT_NULL) ───

TEST(ElfExecWriter, RelaTextSlotDroppedForExec) {
    // ET_REL: 6 sections (NULL, .text, .rela.text, .symtab, .strtab,
    //                     .shstrtab).
    // ET_EXEC: 5 sections (NULL, .text, .symtab, .strtab, .shstrtab)
    //                     — no .rela.text slot at all (architect
    //                     convergence — pre-fix had a phantom
    //                     SHT_NULL placeholder).
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_shnum @ +60 = 5 in ET_EXEC mode.
    EXPECT_EQ(readU16LE(bytes, 60), 5u);
    // e_shstrndx @ +62 = 4 in ET_EXEC mode.
    EXPECT_EQ(readU16LE(bytes, 62), 4u);
    // Section index 2 in ET_EXEC is .symtab (SHT_SYMTAB = 2), not
    // SHT_NULL — verify by reading sh_type @ +4 within the header.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    EXPECT_EQ(readU32LE(bytes, shoff + 2 * 64 + 4), 2u)
        << "ET_EXEC section index 2 must be .symtab (SHT_SYMTAB=2), "
           "not a phantom SHT_NULL placeholder";
}

// ── Cycle-2 fail-loud guards ───────────────────────────────────

TEST(ElfExecWriter, NonEmptyRelocationsFailLoud) {
    // Cycle 2 scope: ET_EXEC accepts only self-contained modules
    // (zero relocations). Modules with relocations fail loud —
    // reloc application is anchored at LK6 (D-LK1-3).
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};  // call rel32
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};       // extern
    rel.kind   = RelocationKind{1};  // rel32
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecWriter, ZeroFunctionModuleFailLoud) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 0;
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── Schema validate rules pinned ───────────────────────────────

TEST(ElfExecFormatJson, ExecWithZeroVirtualAddressRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-exec","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfRelFormatJson, RelWithNonZeroVirtualAddressRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-rel","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfFormatJson, UnknownTypeStringRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-type","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"executable" }
    })");
    ASSERT_FALSE(r.has_value());
}

// PE NonZeroVirtualAddressRejected test relocated to
// tests/link/test_pe_writer.cpp (convention: per-format tests
// live with their format).

// ── entryPoint resolution (when set) ──────────────────────────

TEST(ElfExecWriter, EntryPointResolvesSecondFunctionByName) {
    // When `entryPoint` is non-empty, the walker looks up the named
    // function (matched against the synthesized `sym_<id>` form
    // today; real names land with D-LK1-1). Verify that a 2-function
    // module with `entryPoint = "sym_42"` (second function's id)
    // computes e_entry from the SECOND function's offset in .text,
    // not the first.
    auto execShared = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(execShared.has_value());
    // Forge a schema with entryPoint = "sym_42" by re-loading from text
    // (the shipped file has entryPoint=""; we override).
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"forge-exec","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec" },
      "entryPoint": "sym_42",
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ]
    })");
    ASSERT_TRUE(r.has_value());
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());

    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes = {0x90, 0x90, 0xC3};  // 3 bytes
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{42};       // matches entryPoint = "sym_42"
    b.bytes = {0xC3};
    mod.functions.push_back(std::move(b));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **r, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    // e_entry = virtualAddress + funcTextStart[1] = 0x401000 + 3.
    EXPECT_EQ(readU64LE(bytes, 24), 0x401000u + 3u);
}

TEST(ElfExecWriter, UnknownEntryPointFailsLoud) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-entry","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec" },
      "entryPoint": "sym_99",
      "sections":[
        {"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400},
        {"kind":"symtab","name":".symtab","type":2,"flags":0,"addrAlign":8,"entrySize":24,"virtualAddress":0},
        {"kind":"strtab","name":".strtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0},
        {"kind":"shstrtab","name":".shstrtab","type":3,"flags":0,"addrAlign":1,"entrySize":0,"virtualAddress":0}
      ]
    })");
    ASSERT_TRUE(r.has_value());

    AssembledModule mod = makeTrivialModule({0xC3}, 1);  // sym 1, not 99
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **r, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawK = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawK = true;
    }
    EXPECT_TRUE(sawK);
}

// ── PT_LOAD page alignment (kernel ENOEXEC guard) ──────────────

TEST(ElfExecWriter, PtLoadFileOffsetIsPageAligned) {
    // Silent-failure-hunter CRITICAL-1: p_offset must satisfy
    //   p_offset % p_align == p_vaddr % p_align
    // The kernel rejects mismatched ET_EXEC with ENOEXEC. p_vaddr
    // is 0x401000 (page-aligned); p_offset must therefore be a
    // multiple of 0x1000.
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const pOffset = readU64LE(bytes, 64 + 8);
    std::uint64_t const pVaddr = readU64LE(bytes, 64 + 16);
    std::uint64_t const pAlign = readU64LE(bytes, 64 + 48);
    EXPECT_EQ(pAlign, 0x1000u);
    EXPECT_EQ(pOffset % pAlign, pVaddr % pAlign)
        << "p_offset (" << pOffset << ") and p_vaddr (" << pVaddr
        << ") must be congruent modulo p_align (" << pAlign << ") "
           "or the Linux kernel will reject the executable with ENOEXEC";
}

// ── End-to-end via the format-blind link() dispatch ────────────

TEST(LinkerEndToEnd, ElfExecDispatchProducesValidExecutable) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::Elf);
    EXPECT_FALSE(image.bytes.empty());
    EXPECT_EQ(rep.errorCount(), 0u);
    // ELF magic
    EXPECT_EQ(image.bytes[0], 0x7Fu);
    EXPECT_EQ(image.bytes[1], 'E');
    EXPECT_EQ(image.bytes[2], 'L');
    EXPECT_EQ(image.bytes[3], 'F');
    // e_type = ET_EXEC
    EXPECT_EQ(readU16LE(image.bytes, 16), 2u);
}
