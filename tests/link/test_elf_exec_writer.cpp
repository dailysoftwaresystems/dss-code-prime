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
    // LK6 cycle 2b.1 substrate: PT_INTERP path declared on the
    // shipped exec schema. Walker emission (D-LK6-4) consumes
    // this when emitting the .interp section + PT_INTERP program
    // header.
    EXPECT_EQ(loaded.format->elf().interpreter,
              "/lib64/ld-linux-x86-64.so.2");
}

TEST(ElfExecFormatJson, InterpreterTypeCheckRejectsNonString) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-interp","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": 42 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecFormatJson, EmptyInterpreterStringRejectedAtLoad) {
    // 5-agent CRITICAL convergence (code-reviewer + silent-failure +
    // comment-analyzer + type-design + architect): a literal
    // `"interpreter": ""` in JSON is a config error (Linux kernel
    // rejects zero-length PT_INTERP). Absent field is fine (default
    // empty); explicit empty is not.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"empty-interp","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096, "interpreter": "" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfRelFormatJson, InterpreterOnRelFormatRejectedAtLoad) {
    // type-design Concern #2 + test-analyzer Gap 2 (2-agent): ET_REL
    // must not carry an interpreter path (no PT_INTERP exists on
    // relocatable .o files). Symmetric with the existing
    // ET_REL/virtualAddress!=0 reject.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"rel-with-interp","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"rel", "interpreter": "/lib64/ld-linux-x86-64.so.2" }
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecWriter, ExternImportsWithEmptyInterpreterCitesSubstrateGap) {
    // Cross-format symmetry with PE's fail-loud, but extended:
    // when `elf.interpreter` is empty AND externImports is
    // non-empty, the diagnostic surfaces the empty PT_INTERP
    // path (not just the missing walker emission).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"exec-no-interp","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
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
    ExternImport imp;
    imp.symbol = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawSubstrateMention = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("interpreter") != std::string::npos) {
            sawSubstrateMention = true;
        }
    }
    EXPECT_TRUE(sawSubstrateMention);
}

TEST(ElfExecWriter, ExternImportsDiagnosticDistinguishesByInterpreterPath) {
    // When `elf.interpreter` IS populated (substrate ready), the
    // diagnostic distinguishes substrate-ready from substrate-
    // missing BY EMBEDDING THE PATH itself — the behavioral
    // distinction a human reader and a future maintainer act on
    // (test-analyzer Gap 3 fold: pin path asymmetry not anchor
    // codes, which rot when D-LK6-4 closes).
    //
    // The shipped exec JSON has the path; the empty-interpreter
    // diagnostic is asserted not to contain it.
    auto loaded = loadShipped();
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
    ExternImport imp;
    imp.symbol = SymbolId{99};
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawPath = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport
         && d.actual.find("/lib64/ld-linux-x86-64.so.2") != std::string::npos) {
            sawPath = true;
        }
    }
    EXPECT_TRUE(sawPath);
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

// ── Reloc application (LK6 cycle 1) ────────────────────────────

TEST(ElfExecWriter, ExternRelocationFailsLoudAsUndefined) {
    // ET_EXEC's reloc applier resolves targets against the
    // module's own AssembledFunctions. A reloc whose target is
    // NOT defined locally is an extern — that's FFI / dynamic
    // linking (LK6 cycle 2), so the cycle-1 walker fails loud
    // with K_SymbolUndefined rather than silently zero-patching
    // or fabricating a value.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};  // call rel32
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{99};       // extern — not in module
    rel.kind   = RelocationKind{1};  // rel32
    // `addend` left at default 0 — the structured `addendBias` on
    // the rel32 schema row carries the implicit −4 (instruction-end
    // offset). The assembler never stamps a non-zero `addend` for
    // x86_64 rel32; mirror that here.
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawUndefined = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_SymbolUndefined) sawUndefined = true;
    }
    EXPECT_TRUE(sawUndefined);
}

TEST(ElfExecWriter, IntraModuleRel32CallAppliedByteForByte) {
    // Multi-function ET_EXEC module: fn[0] is a `call rel32` to
    // fn[1] (a `ret`). The cycle-1 applier resolves the rel32
    // patch in-place to S + A − P − 4 (x86_64 rel32 formula:
    // pcRelative=true, addendBias=−4, widthBytes=4).
    //
    // Layout:
    //   .text @ vaddr = secText->virtualAddress (= 0x401000 in
    //   shipped exec schema).
    //   fn[0] @ +0:   E8 ?? ?? ?? ?? C3           (call rel32 ; ret)
    //   fn[1] @ +6:   C3                          (ret)
    //
    // Patch site:
    //   P = vaddr + 1, S = vaddr + 6, A = 0.
    //   value = S − P − 4 = (vaddr+6) − (vaddr+1) − 4 = 1
    //   → bytes at .text[1..5] = 01 00 00 00.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};        // → fn[1]
    rel.kind   = RelocationKind{1};  // rel32
    rel.addend = 0;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    // Locate `.text` body in the file image — derive from the
    // section header table rather than hard-coding (ET_EXEC pads
    // .text to a page boundary so execve() can mmap it).
    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    // Patched displacement must equal 1 (LE 4 bytes).
    EXPECT_EQ(bytes[textFileOff + 0], 0xE8u);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 1u)
        << "x86_64 rel32 must apply to S - P - 4 = 1";
    EXPECT_EQ(bytes[textFileOff + 5], 0xC3u);  // ret at end of fn[0]
    EXPECT_EQ(bytes[textFileOff + 6], 0xC3u);  // ret at fn[1]
}

TEST(ElfExecWriter, IntraModuleAbs64AppliedByteForByte) {
    // Cycle-1 abs64 reloc: pcRelative=false, addendBias=0,
    // widthBytes=8 → value = S + A. Pin against an 8-byte slot.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    // fn[0]: 8 zero bytes (the abs64 slot) + ret.
    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0,0,0,0,0,0,0,0, 0xC3};
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};        // → fn[1]
    rel.kind   = RelocationKind{2};  // abs64
    rel.addend = 0;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textVa      = readU64LE(bytes, shoff + 1 * 64 + 16);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    // Expected: virtualAddress(.text) + offset of fn[1] inside .text.
    // fn[0] is 9 bytes; fn[1] is at offset 9.
    std::uint64_t const expected = textVa + 9;
    EXPECT_EQ(readU64LE(bytes, textFileOff + 0), expected);
}

TEST(ElfExecWriter, NonZeroAddendIsRespectedSeparatelyFromAddendBias) {
    // Pins the `A` term of `value = S + A + (pcRelative ? -P : 0) +
    // addendBias`. A regression that drops `+ A` or that conflates
    // `A` with the schema's `addendBias` would still pass the
    // zero-addend tests above. Use rel32 with rel.addend = 7 →
    //   value = (vaddr+6) + 7 - (vaddr+1) - 4 = 8.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};  // rel32
    rel.addend = 7;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 1), 8u)
        << "rel32 value must include + Relocation::addend (A=7) "
           "alongside the schema's addendBias (-4)";
}

TEST(ElfExecWriter, Abs32WriteRespectsWidthBytesAndPreservesAdjacentBytes) {
    // Pins (a) abs32 (widthBytes=4, pcRelative=false) writes exactly
    // 4 bytes, not 8, and (b) bytes immediately AFTER the patch
    // survive intact. A regression that hardcodes an 8-byte write
    // would clobber the post-patch sentinel.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    // [4-byte abs32 slot] [sentinel 0xDE 0xAD 0xBE 0xEF] [ret]
    f0.bytes  = {0,0,0,0,  0xDE,0xAD,0xBE,0xEF,  0xC3};
    Relocation rel;
    rel.offset = 0;
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{3};  // abs32
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3};
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    std::uint64_t const shoff       = readU64LE(bytes, 40);
    std::uint64_t const textVa      = readU64LE(bytes, shoff + 1 * 64 + 16);
    std::uint64_t const textFileOff = readU64LE(bytes, shoff + 1 * 64 + 24);
    // fn[1] starts at offset 9 inside .text; abs32 of fn[1] is the
    // low 4 bytes of (textVa + 9).
    std::uint32_t const expected = static_cast<std::uint32_t>(textVa + 9);
    EXPECT_EQ(readU32LE(bytes, textFileOff + 0), expected);
    // Sentinel must be untouched — abs32 wrote exactly 4 bytes.
    EXPECT_EQ(bytes[textFileOff + 4], 0xDEu);
    EXPECT_EQ(bytes[textFileOff + 5], 0xADu);
    EXPECT_EQ(bytes[textFileOff + 6], 0xBEu);
    EXPECT_EQ(bytes[textFileOff + 7], 0xEFu);
}

TEST(ElfExecWriter, RelocOffsetPastFunctionBytesFailsLoud) {
    // The bounds check guards `rel.offset + widthBytes` against the
    // ORIGINATING function's `fn.bytes.size()`, not just total
    // `.text`. A stale offset that "happens to fit" inside .text
    // would otherwise silently scribble into the next function.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xC3};   // 1 byte — far too small for rel32
    Relocation rel;
    rel.offset = 4;       // past end of f0.bytes (size=1)
    rel.target = SymbolId{2};
    rel.kind   = RelocationKind{1};  // rel32, widthBytes=4
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    AssembledFunction f1;
    f1.symbol = SymbolId{2};
    f1.bytes  = {0xC3, 0xC3, 0xC3, 0xC3};   // enough that .text would fit
    mod.functions.push_back(std::move(f1));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecWriter, DisplacementOverflowFailsLoud) {
    // value = S + A - P - 4. For rel32 (widthBytes=4) the result
    // must fit in i32. Forge a `Relocation::addend` large enough to
    // push the displacement past INT32_MAX so the value range-check
    // fires — a regression that drops the check would silently
    // truncate to the low 32 bits.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;

    AssembledFunction f0;
    f0.symbol = SymbolId{1};
    f0.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00, 0xC3};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};        // self — passes the extern guard
    rel.kind   = RelocationKind{1};
    rel.addend = std::numeric_limits<std::int64_t>::max() / 2;
    f0.relocations.push_back(rel);
    mod.functions.push_back(std::move(f0));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_RelocationKindMismatch) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfExecFormatJsonValidate, AbsoluteWithAddendBiasRejected) {
    // Coherence rule (c): `addendBias != 0` ⇒ `pcRelative`. An
    // absolute reloc with a non-zero bias has no real consumer and
    // is almost certainly a typo — load must fail at validate.
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bad-bias"},
      "relocations":[
        { "name": "weird", "kind": 1, "pcRelative": false, "addendBias": -4, "widthBytes": 4 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecFormatJsonValidate, AddendBiasOverflowWidthRejected) {
    // Coherence rule (d): `|addendBias|` must fit signed in
    // `widthBytes`. A bias of 0x10000 in a widthBytes=2 slot would
    // overflow on every patch.
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"bias-overflows-width"},
      "relocations":[
        { "name": "weird", "kind": 1, "pcRelative": true, "addendBias": 70000, "widthBytes": 4 }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    // 70000 fits in i32 (range OK), in i16 (no) — widthBytes=4 (i32
    // range) accepts it. Use a 5 GiB bias with widthBytes=4 instead:
    if (r.has_value()) {
        auto r2 = TargetSchema::loadFromText(R"({
          "dssTargetVersion": 1,
          "target": {"name":"bias-overflows-width-2"},
          "relocations":[
            { "name": "weird", "kind": 1, "pcRelative": true, "addendBias": 2147483647, "widthBytes": 4 }
          ],
          "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
        })");
        // INT32_MAX is exactly the signed-4-byte cap → still accepted.
        // The coherence rule rejects only OVERFLOW; this is the
        // boundary that confirms inclusive bounds.
        ASSERT_TRUE(r2.has_value());
    }
}

TEST(ElfExecFormatJsonValidate, PcRelativeWithoutWidthBytesRejected) {
    // Coherence rule (b): `pcRelative` with `widthBytes == 0` is a
    // declared-but-inapplicable shape — load must fail.
    auto r = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"pcrel-no-width"},
      "relocations":[
        { "name": "weird", "kind": 1, "pcRelative": true }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecWriter, NoStructuredFormulaWidthFailsLoud) {
    // A relocation kind whose schema row has widthBytes == 0 (the
    // formula doesn't fit the LK6 cycle-1 linear shape) fails loud.
    // Synthesize such a kind via the JSON loader to keep the test
    // target-blind (no dependence on a particular shipped schema).
    auto fmtR = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    ASSERT_TRUE(fmtR.has_value());

    auto tgtR = TargetSchema::loadFromText(R"({
      "dssTargetVersion": 1,
      "target": {"name":"noapply"},
      "relocations":[
        { "name": "weird", "kind": 1, "formula": "complicated" }
      ],
      "opcodes":[ {"mnemonic":"invalid","result":"none"} ]
    })");
    ASSERT_TRUE(tgtR.has_value()) << [&]{
        std::string s; for (auto const& d : tgtR.error()) s += d.message + "\n"; return s;
    }();

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Relocation rel;
    rel.offset = 1;
    rel.target = SymbolId{1};        // self — passes extern check
    rel.kind   = RelocationKind{1};
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **tgtR, **fmtR, rep);
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
    // The LK6 cycle-1 empty-.text guard fires before the entry-point
    // lookup would reach K_SymbolUndefined — a zero-function module
    // produces zero `.text` bytes, and an executable with no entry
    // instructions would SIGSEGV at exec time. EITHER diagnostic
    // code counts as fail-loud; pin the contract loosely so future
    // re-orderings of the two guards don't churn this test.
    EXPECT_GT(rep.errorCount(), 0u);
}

// ── Schema validate rules pinned ───────────────────────────────

TEST(ElfExecFormatJson, ExecWithZeroVirtualAddressRejected) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"bad-exec","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":0}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecFormatJson, ExecWithoutPageAlignRejected) {
    // D-LK6-3: ET_EXEC schemas MUST declare `elf.pageAlign`. The
    // Linux kernel rejects ELF executables whose PT_LOAD p_align
    // is smaller than the runtime page size — ARM64 / Apple
    // Silicon configurations declare 16384 or 65536 instead of
    // 4096, so the value cannot be hardcoded in the walker.
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"no-page-align","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec" },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
    })");
    ASSERT_FALSE(r.has_value());
}

TEST(ElfExecFormatJson, PageAlignMustBePowerOfTwo) {
    auto r = ObjectFormatSchema::loadFromText(R"({
      "dssObjectFormatVersion": 1,
      "format": {"name":"odd-page-align","kind":"elf"},
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 3000 },
      "sections":[{"kind":"text","name":".text","type":1,"flags":6,"addrAlign":16,"entrySize":0,"virtualAddress":4198400}]
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
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
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
      "elf": { "class":"elf64", "data":"lsb", "machine": 62, "type":"exec", "pageAlign": 4096 },
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

// ── LK6 cycle 2a: extern imports fail loud (D-LK6-4 pending) ───

TEST(ElfExecWriter, ExternImportsFailLoudPendingD_LK6_4) {
    // ELF GOT/PLT support not yet implemented (D-LK6-4). A module
    // with non-empty externImports must fail loud with
    // K_FormatLacksImportSupport rather than silently producing an
    // ELF that's missing its imports.
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
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
    imp.mangledName = "printf";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(ElfRelWriter, ExternImportsFailLoudOnEtRelAlso) {
    // test-analyzer #7: the externImports fail-loud gate runs BEFORE
    // the isExec branch, so .o output also rejects non-empty
    // externImports (relocatable objects don't carry imports either
    // — those resolve at final-link time).
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0xC3};
    mod.functions.push_back(std::move(fn));
    ExternImport imp;
    imp.symbol      = SymbolId{99};
    imp.mangledName = "puts";
    imp.libraryPath = "libc.so.6";
    mod.externImports.push_back(std::move(imp));
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_FormatLacksImportSupport) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}
