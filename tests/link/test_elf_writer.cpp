// ELF64 .o writer tests — plan 14 LK1 cycle 1.
//
// Pins golden byte-level invariants of the emitted ELF64 relocatable
// object:
//   * Elf64_Ehdr identity bytes match the gABI spec for ET_REL x86_64.
//   * Section count = 7 (SHT_NULL + .text + .rela.text + .symtab +
//     .strtab + .shstrtab + .note.GNU-stack appended last).
//   * e_shstrndx points at .shstrtab (index 5) — unchanged by the
//     appended-last .note.GNU-stack (D-LK-OBJECT-EXTERN-SYMBOL-NAMES).
//   * .symtab sh_info equals the index of the first non-LOCAL symbol
//     (mandatory by gABI 4.18 — local-then-global ordering).
//   * Function symbol's st_value matches its offset within .text.
//   * Relocation r_info high 32 bits = symtab index, low 32 = ELF
//     reloc type number from format JSON's `nativeId`.
//   * Empty module produces an empty `bytes` vector (walker skipped
//     because expectedFuncCount==0 makes `ok()` false up front;
//     dispatch shell still runs but the walker tolerates no funcs).
//
// Also pins that the shipped `elf64-x86_64-linux.format.json` file
// loads cleanly via `loadShipped`.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "link/format/elf.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"
// TF-C52 (D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT): the ELF-writer GOT pin drives a
// hand-built MIR through the REAL back-half (the same stage order as
// compile_pipeline.cpp's lowerMirModuleToAssembly) so the emitted `.o`'s GOT
// reloc nativeIds are asserted — mirrors test_asm_arm64.cpp's runFullPipeline.
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_regalloc.hpp"
#include "lir/lir_rewrite.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// D-TEST-LE-READ-HELPERS CLOSED at 8aabc04 audit fold; complete-
// hoist at 5ac97ae audit fold per code-architect Q1 (the partial
// hoist of just u16 was strictly worse than either consistent
// choice — 4 consumers across ELF/PE/Mach-O writer tests).
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
    auto f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux");
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(elf64-x86_64-linux) failed";
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

// Read the NUL-terminated symbol name at `strtabOff + stName` in `.strtab`.
[[nodiscard]] std::string readStrtabName(std::vector<std::uint8_t> const& bytes,
                                          std::uint64_t strtabOff,
                                          std::uint32_t stName) {
    std::string out;
    for (std::size_t p = strtabOff + stName; p < bytes.size() && bytes[p] != 0; ++p) {
        out.push_back(static_cast<char>(bytes[p]));
    }
    return out;
}

// Find a section header index by its `.shstrtab` name (robust to layout
// reordering — the c145 relro tests key on names, not golden indices).
// Returns -1 when absent.
[[nodiscard]] int findSectionByName(std::vector<std::uint8_t> const& bytes,
                                    std::string const& name) {
    std::uint64_t const shoff    = readU64LE(bytes, 40);
    std::uint16_t const shnum    = readU16LE(bytes, 60);
    std::uint16_t const shstrndx = readU16LE(bytes, 62);
    std::uint64_t const shstrOff = readU64LE(bytes, shoff + shstrndx * 64 + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint32_t const nameOff = readU32LE(bytes, shoff + i * 64 + 0);
        if (readStrtabName(bytes, shstrOff, nameOff) == name)
            return static_cast<int>(i);
    }
    return -1;
}

} // namespace

// ── Shipped JSON loads ───────────────────────────────────────────

TEST(ElfFormatJson, ShippedFileLoadsCleanly) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.format);
    EXPECT_EQ(loaded.format->kind(), ObjectFormatKind::Elf);
    EXPECT_EQ(loaded.format->name(), "elf64-x86_64-linux");
    EXPECT_EQ(loaded.format->elf().fileClass, 2u);    // ELFCLASS64
    EXPECT_EQ(loaded.format->elf().dataEncoding, 1u); // ELFDATA2LSB
    EXPECT_EQ(loaded.format->elf().machine, 62u);     // EM_X86_64
    // sections[] contains text, reloc, symtab, strtab, shstrtab.
    EXPECT_NE(loaded.format->sectionByKind(SectionKind::Text), nullptr);
    EXPECT_NE(loaded.format->sectionByKind(SectionKind::RelocTable), nullptr);
    EXPECT_NE(loaded.format->sectionByKind(SectionKind::Symtab), nullptr);
    EXPECT_NE(loaded.format->sectionByKind(SectionKind::Strtab), nullptr);
    EXPECT_NE(loaded.format->sectionByKind(SectionKind::ShStrtab), nullptr);
    // Reloc rows carry nativeId.
    auto const* pc32 = loaded.format->relocationByKind(RelocationKind{1});
    ASSERT_NE(pc32, nullptr);
    EXPECT_EQ(pc32->name, "R_X86_64_PC32");
    EXPECT_EQ(pc32->nativeId, 2u);
}

// ── Elf64_Ehdr golden bytes ──────────────────────────────────────

TEST(ElfWriter, Elf64EhdrIdentityBytesMatchGabiSpec) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    // 1-byte ret (0xC3) — minimal valid x86_64 .text payload.
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_GT(bytes.size(), 64u) << "must at least contain Elf64_Ehdr";
    EXPECT_EQ(rep.errorCount(), 0u);

    // e_ident magic.
    EXPECT_EQ(bytes[0], 0x7F);
    EXPECT_EQ(bytes[1], 'E');
    EXPECT_EQ(bytes[2], 'L');
    EXPECT_EQ(bytes[3], 'F');
    EXPECT_EQ(bytes[4], 2u);   // ELFCLASS64
    EXPECT_EQ(bytes[5], 1u);   // ELFDATA2LSB
    EXPECT_EQ(bytes[6], 1u);   // EV_CURRENT
    EXPECT_EQ(bytes[7], 0u);   // ELFOSABI_NONE (sysv)
    EXPECT_EQ(bytes[8], 0u);   // ABI version

    // e_type = ET_REL (1)
    EXPECT_EQ(readU16LE(bytes, 16), 1u);
    // e_machine = EM_X86_64 (62)
    EXPECT_EQ(readU16LE(bytes, 18), 62u);
    // e_version = EV_CURRENT (1)
    EXPECT_EQ(readU32LE(bytes, 20), 1u);
    // e_entry / e_phoff = 0 in ET_REL
    EXPECT_EQ(readU64LE(bytes, 24), 0u);
    EXPECT_EQ(readU64LE(bytes, 32), 0u);
    // e_ehsize = 64
    EXPECT_EQ(readU16LE(bytes, 52), 64u);
    // e_shentsize = 64 (sizeof Elf64_Shdr)
    EXPECT_EQ(readU16LE(bytes, 58), 64u);
    // e_shnum = 7 (…+ .note.GNU-stack appended last)
    EXPECT_EQ(readU16LE(bytes, 60), 7u);
    // e_shstrndx = 5 (.shstrtab; unchanged — .note.GNU-stack is index 6)
    EXPECT_EQ(readU16LE(bytes, 62), 5u);
}

// ── Section header table layout ─────────────────────────────────

TEST(ElfWriter, SectionHeaderTableHasSevenEntriesNullThenTextThenRelocThenNoteLast) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // e_shoff is the file offset of the section header table.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    ASSERT_GT(shoff, 64u);
    ASSERT_LE(shoff + 7 * 64, bytes.size())
        << "section header table must fit within file";

    // Section header 0 (SHT_NULL) is all zero.
    for (std::size_t i = 0; i < 64; ++i) {
        EXPECT_EQ(bytes[shoff + i], 0u)
            << "section[0] (SHT_NULL) byte " << i;
    }

    // Section header 1 (.text): sh_type=SHT_PROGBITS(1), sh_flags=6.
    EXPECT_EQ(readU32LE(bytes, shoff + 64 + 4), 1u);
    EXPECT_EQ(readU64LE(bytes, shoff + 64 + 8), 6u);
    // sh_addralign=16.
    EXPECT_EQ(readU64LE(bytes, shoff + 64 + 48), 16u);

    // Section header 2 (.rela.text): sh_type=SHT_RELA(4), sh_entsize=24.
    EXPECT_EQ(readU32LE(bytes, shoff + 128 + 4), 4u);
    EXPECT_EQ(readU64LE(bytes, shoff + 128 + 56), 24u);

    // Section header 6 (.note.GNU-stack, appended LAST — D-LK-OBJECT-EXTERN-
    // SYMBOL-NAMES): SHT_PROGBITS(1), sh_flags=0 (NO SHF_EXECINSTR — the
    // non-executable-stack marker), sh_size=0 (empty).
    std::uint64_t const noteShdr = shoff + 6 * 64;
    EXPECT_EQ(readU32LE(bytes, noteShdr + 4), 1u)  // sh_type
        << ".note.GNU-stack must be SHT_PROGBITS";
    EXPECT_EQ(readU64LE(bytes, noteShdr + 8), 0u)  // sh_flags
        << ".note.GNU-stack sh_flags must be 0 (no SHF_EXECINSTR)";
    EXPECT_EQ(readU64LE(bytes, noteShdr + 32), 0u) // sh_size
        << ".note.GNU-stack is an empty marker section";
}

// ── .symtab local-then-global ordering ──────────────────────────

TEST(ElfWriter, SymtabFirstNonLocalEqualsTwoBecauseSectionSymbolIsLocal) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 7);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    std::uint64_t const shoff = readU64LE(bytes, 40);
    // Section 3 is .symtab. sh_info @ offset 44 within Shdr.
    std::uint64_t const symtabShdr = shoff + 3 * 64;
    EXPECT_EQ(readU32LE(bytes, symtabShdr + 44), 2u)
        << "first non-LOCAL symbol must be at index 2 (after STN_UNDEF + "
           "STT_SECTION local for .text)";
    // sh_link points at .strtab (index 4).
    EXPECT_EQ(readU32LE(bytes, symtabShdr + 40), 4u);
    // sh_entsize = 24 (Elf64_Sym).
    EXPECT_EQ(readU64LE(bytes, symtabShdr + 56), 24u);
}

// ── Function symbol carries the correct st_value ────────────────

TEST(ElfWriter, FunctionSymbolStValueMatchesTextOffset) {
    auto loaded = loadShipped();
    // Two functions back-to-back; the second's symbol must have
    // st_value = len(first.bytes).
    AssembledModule mod;
    mod.expectedFuncCount = 2;
    AssembledFunction a;
    a.symbol = SymbolId{1};
    a.bytes = {0x90, 0x90, 0xC3};   // nop nop ret
    mod.functions.push_back(std::move(a));
    AssembledFunction b;
    b.symbol = SymbolId{2};
    b.bytes = {0xC3};
    mod.functions.push_back(std::move(b));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + 3 * 64 + 24);
    // Indices: 0=STN_UNDEF, 1=section, 2=symbol#1, 3=symbol#2.
    std::uint64_t const sym1StValue = readU64LE(bytes, symtabOff + 2 * 24 + 8);
    std::uint64_t const sym2StValue = readU64LE(bytes, symtabOff + 3 * 24 + 8);
    EXPECT_EQ(sym1StValue, 0u);
    EXPECT_EQ(sym2StValue, 3u);
    // sizes match function byte counts.
    EXPECT_EQ(readU64LE(bytes, symtabOff + 2 * 24 + 16), 3u);
    EXPECT_EQ(readU64LE(bytes, symtabOff + 3 * 24 + 16), 1u);
}

// ── D-LK-OBJECT-EXTERN-SYMBOL-NAMES: real names in the .o symtab ──

TEST(ElfWriter, ObjectSymtabCarriesRealNameForExternalDefButStaticStaysInternal) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);

    // Two defined functions covering both externally-visible forms:
    //   * fn A (SymbolId 10) — externally-visible (Global) → REAL name.
    //   * fn B (SymbolId 11) — static (Local) → stays internal `sym_11`.
    // (The IMPORT side — naming an undefined extern + its PLT32 reloc — is
    // covered by ObjectExternCallEmitsUndefImportNameAndPlt32Reloc below.)
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    AssembledFunction a;
    a.symbol = SymbolId{10};
    a.bytes  = {0xC3};
    mod.functions.push_back(std::move(a));

    AssembledFunction b;
    b.symbol = SymbolId{11};
    b.bytes  = {0xC3};
    mod.functions.push_back(std::move(b));

    // The name/binding table the compile pipeline populates (LK11a), carrying
    // the already-mangled on-binary name (identity on ELF).
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "public_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{11}, "static_fn",
                                       SymbolBinding::Local,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    std::uint64_t const shoff     = readU64LE(bytes, 40);
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + 3 * 64 + 24);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + 4 * 64 + 24);

    // Symtab order: 0=UNDEF, 1=STT_SECTION, 2=fnA, 3=fnB.
    auto nameAt = [&](std::uint64_t i) {
        return readStrtabName(bytes, strtabOff,
                              readU32LE(bytes, symtabOff + i * 24 + 0));
    };
    auto infoAt = [&](std::uint64_t i) { return bytes[symtabOff + i * 24 + 4]; };

    // EXPORT — externally-visible fn A carries its real C name (STB_GLOBAL|STT_FUNC).
    EXPECT_EQ(nameAt(2), "public_fn")
        << "externally-visible defined function must carry its real name";
    EXPECT_EQ(infoAt(2), 0x12u);   // (STB_GLOBAL<<4)|STT_FUNC

    // CARVE-OUT — static fn B stays internal `sym_11` (isExternallyVisible=false).
    EXPECT_EQ(nameAt(3), "sym_11")
        << "a static (Local-binding) function must stay internal, not leak its "
           "real name into the object symtab";
    EXPECT_EQ(infoAt(3), 0x12u);   // still STB_GLOBAL|STT_FUNC (name-only carve-out)
}

// ── D-LK-OBJECT-EXTERN-CALL-RELOCATABLE: undefined extern + PLT32 ──

TEST(ElfWriter, ObjectExternCallEmitsUndefImportNameAndPlt32Reloc) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    // The relocatable format now declares externCallDispatch, so an ET_REL
    // object CAN carry an extern import (a `call` to a libc function the FINAL
    // linker resolves). The extern must appear as an SHN_UNDEF symbol with its
    // real name, and its rel32 call reloc must be the PLT-capable PLT32 (type
    // 4) — a bare PC32 against an undefined symbol errors under a foreign -pie.
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{10};
    caller.bytes  = {0xE8, 0x00, 0x00, 0x00, 0x00};   // call rel32 → extern
    caller.relocations.push_back(Relocation{/*offset=*/1u, /*target=*/SymbolId{20},
                                            /*kind=*/RelocationKind{1},  // rel32
                                            /*addend=*/0});
    mod.functions.push_back(std::move(caller));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "caller",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "libc_fn";
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u) << "ET_REL with externCallDispatch must "
                                       "accept an extern import";

    std::uint64_t const shoff     = readU64LE(bytes, 40);
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + 3 * 64 + 24);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + 4 * 64 + 24);

    // Symtab: 0=UNDEF, 1=STT_SECTION, 2=caller, 3=extern.
    std::uint32_t const extStName = readU32LE(bytes, symtabOff + 3 * 24 + 0);
    EXPECT_EQ(readStrtabName(bytes, strtabOff, extStName), "libc_fn")
        << "undefined extern must carry its real import name";
    EXPECT_EQ(readU16LE(bytes, symtabOff + 3 * 24 + 6), 0u)      // st_shndx
        << "extern must be SHN_UNDEF";

    // .rela.text (section index 2): one entry, type = PLT32 (4), addend -4.
    std::uint64_t const relaOff = readU64LE(bytes, shoff + 2 * 64 + 24);
    std::uint64_t const rInfo   = readU64LE(bytes, relaOff + 8);
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo), 4u)
        << "an extern call reloc must be R_X86_64_PLT32 (4), not PC32 (2), so a "
           "foreign PIE link routes it through a linker-built PLT";
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo >> 32), 3u)  // symtab idx = extern
        << "reloc must target the undefined extern symbol";
    std::int64_t const addend = static_cast<std::int64_t>(readU64LE(bytes, relaOff + 16));
    EXPECT_EQ(addend, -4) << "psABI rel32 addend (baked bias)";
}

// ── D-LK-ARM64-ELF-RELOC-EXTERN-DISPATCH: arm64 undefined extern + CALL26 (no PLT variant) ──

TEST(ElfWriter, ObjectExternCallEmitsUndefImportNameAndCall26RelocOnAarch64) {
    // The arm64 relocatable format now declares externCallDispatch=direct-plt — the
    // sibling of x86_64's ObjectExternCallEmitsUndefImportNameAndPlt32Reloc — so an
    // arm64 ET_REL object CAN carry an extern import (`bl printf` in a `.o` the FINAL
    // linker resolves). The extern must appear as an SHN_UNDEF symbol with its real
    // name; its BL call reloc must be R_AARCH64_CALL26 (283) — NOT a PLT-variant.
    // AArch64 has no distinct PLT26 reloc: CALL26 against an undefined symbol is
    // exactly what gcc/clang emit and the foreign linker inserts the veneer/PLT
    // transparently — so the CALL26 row carries NO pltNativeId, and the writer emits
    // its plain nativeId 283 (the pltNativeId==0 branch of the reloc-type selection).
    // This locks in the "no pltNativeId is correct on arm64" decision against drift.
    // RED-ON-DISABLE: remove externCallDispatch from elf64-aarch64-linux.format.json
    // -> elf::encode rejects the ET_REL extern (K_FormatLacksImportSupport, errors>0).
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(fmt.has_value());

    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{10};
    caller.bytes  = {0x00, 0x00, 0x00, 0x94};   // BL #0 (0x94000000 LE) → extern
    caller.relocations.push_back(Relocation{/*offset=*/0u, /*target=*/SymbolId{20},
                                            /*kind=*/RelocationKind{1},  // call26
                                            /*addend=*/0});
    mod.functions.push_back(std::move(caller));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "caller",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport ext;
    ext.symbol      = SymbolId{20};
    ext.mangledName = "libc_fn";
    ext.isData      = false;
    mod.externImports.push_back(std::move(ext));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u) << "arm64 ET_REL with externCallDispatch must "
                                       "accept an extern import";

    std::uint64_t const shoff     = readU64LE(bytes, 40);
    int const symtabIdx = findSectionByName(bytes, ".symtab");
    int const strtabIdx = findSectionByName(bytes, ".strtab");
    int const relaIdx   = findSectionByName(bytes, ".rela.text");
    ASSERT_GE(symtabIdx, 0);
    ASSERT_GE(strtabIdx, 0);
    ASSERT_GE(relaIdx, 0) << "arm64 .o with an extern call must emit .rela.text";
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + symtabIdx * 64 + 24);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + strtabIdx * 64 + 24);
    std::uint64_t const relaOff   = readU64LE(bytes, shoff + relaIdx * 64 + 24);

    // The .rela.text reloc: r_offset = 0 (the BL word), type = R_AARCH64_CALL26 (283),
    // addend = 0 (arm64 branch is instruction-relative — no baked bias, unlike x86_64
    // rel32's -4). Type 283, NOT a PLT variant, is the crux.
    EXPECT_EQ(readU64LE(bytes, relaOff + 0), 0u) << "reloc r_offset = the BL word";
    std::uint64_t const rInfo = readU64LE(bytes, relaOff + 8);
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo), 283u)
        << "arm64 extern-call reloc must be R_AARCH64_CALL26 (283), not a PLT variant "
           "(AArch64 has none — the CALL26 row carries no pltNativeId)";
    std::int64_t const addend = static_cast<std::int64_t>(readU64LE(bytes, relaOff + 16));
    EXPECT_EQ(addend, 0) << "arm64 CALL26 addend = 0 (no baked bias)";

    // That reloc names the SHN_UNDEF extern carrying its real import name.
    std::uint32_t const relSymIdx = static_cast<std::uint32_t>(rInfo >> 32);
    std::uint32_t const extStName = readU32LE(bytes, symtabOff + relSymIdx * 24 + 0);
    EXPECT_EQ(readStrtabName(bytes, strtabOff, extStName), "libc_fn")
        << "undefined extern must carry its real import name";
    EXPECT_EQ(readU16LE(bytes, symtabOff + relSymIdx * 24 + 6), 0u)   // st_shndx
        << "extern must be SHN_UNDEF";
}

// ── D-LK-ARM64-EXTERN-DATA-ADDR-PIE-GOT (TF-C52): the emitted-.o GOT-reloc pin ──
//
// The SIBLING of the CALL26 test above, for the GOT-ADDRESS case: taking the
// ADDRESS of an undefined extern as a live VALUE (`return &ext;`) under the
// `externAddrBinding=got` arm64 relocatable format must, in the emitted ELF
// `.o`, carry R_AARCH64_ADR_GOT_PAGE (nativeId 311) on the adrp word +
// R_AARCH64_LD64_GOT_LO12_NC (312) on the ldr word against the SHN_UNDEF
// extern (real name, addend 0), and NOT the absolute R_AARCH64_ADR_PREL_PG_HI21
// (275) / ADD_ABS_LO12_NC (277). This is the ONLY pin that drives the GOT
// materialization through the ELF WRITER — the LIR pin covers routing, the
// schema pins cover kind-coherence, but a wire→nativeId regression in the
// emitted object flips only THIS test (otherwise only the qemu witness would
// catch it). Drives a hand-built MIR through the REAL back-half (MIR→LIR under
// the got format → regalloc → rewrite → 2-addr → callconv → assemble), the
// exact stage order of compile_pipeline.cpp's lowerMirModuleToAssembly, then
// elf::encode.
// RED-ON-DISABLE: revert the externAddrGotSymbols_ routing arm in
// mir_to_lir.cpp (or drop externAddrBinding from the format) → the plain
// absolute ADRP+ADD lea is emitted → the .o carries 275/277, not 311/312 →
// all four EXPECTs flip.
TEST(ElfWriter, GotExternAddrValueEmitsAdrGotPageAndLd64GotLo12OnAarch64) {
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(fmt.has_value());
    ASSERT_TRUE((*fmt)->externAddrBinding().has_value())
        << "the arm64 relocatable format must declare externAddrBinding=got";

    // MIR: `void* f(void){ return &ext; }` — GlobalAddr(ext) used as a VALUE
    // (its sole use is the Return → not a callee, not a foldable load → the
    // value-form GOT arm is reached).
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const ptrT = interner.primitive(TypeKind::Ptr);
    TypeId const sig  = interner.fnSig(std::span<TypeId const>{}, ptrT,
                                       CallConv::CcAAPCS64);
    SymbolId const kCaller{10};
    SymbolId const kExtern{20};
    MirBuilder mb;
    mb.addFunction(sig, kCaller);
    MirBlockId const bb = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(bb);
    MirInstId const ga = mb.addGlobalAddr(kExtern, ptrT);
    mb.addReturn(ga);
    Mir mir = std::move(mb).finish();

    ExternImport ext;
    ext.symbol      = kExtern;
    ext.mangledName = "got_extern_fn";
    ext.libraryPath = "libc.so.6";
    ext.isData      = false;  // a FUNCTION extern whose ADDRESS is taken

    DiagnosticReporter rep;
    std::vector<ExternImport> externs{ext};
    // FULL back-half WITH the got extern-address binding threaded in — the same
    // stage order as compile_pipeline.cpp's lowerMirModuleToAssembly.
    auto lir = lowerToLir(mir, **target, interner, rep, externs,
                          ExternCallDispatch::DirectPlt,
                          /*dataImportBinding=*/std::nullopt,
                          /*tlsAccess=*/std::nullopt,
                          /*sehScopes=*/{},
                          /*wideFloatSoftcallLibrary=*/std::nullopt,
                          /*externAddrBinding=*/ExternAddrBinding::Got);
    ASSERT_TRUE(lir.ok);
    auto const liveness = analyzeLiveness(lir.lir);
    auto const alloc = allocateRegisters(lir.lir, **target, liveness, 0, rep);
    ASSERT_TRUE(alloc.ok());
    auto rewritten = rewriteWithAllocation(lir.lir, **target, alloc, rep);
    ASSERT_TRUE(rewritten.ok);
    auto legal = legalizeTwoAddress(rewritten.lir, **target, rep);
    ASSERT_TRUE(legal.ok());
    auto cc = materializeCallingConvention(legal.lir, **target, alloc, rep);
    ASSERT_TRUE(cc.ok());
    std::vector<MirInstId> lirToMir(cc.lir.instCount(), InvalidMirInst);
    auto assembled = assemble(cc.lir, **target, lirToMir, rep);
    ASSERT_TRUE(assembled.ok());
    // Complete the module the way the pipeline's LK11a stage would (`assemble`
    // sets functions + expectedFuncCount; the defined-symbol table + the extern
    // imports are populated downstream — supplied here, mirroring the sibling).
    assembled.symbols.push_back(ModuleSymbol{kCaller, "f",
                                             SymbolBinding::Global,
                                             SymbolVisibility::Default});
    assembled.externImports.push_back(ext);

    auto bytes = elf::encode(assembled, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u) << "arm64 ET_REL GOT-address emit must be clean";

    std::uint64_t const shoff = readU64LE(bytes, 40);
    int const symtabIdx = findSectionByName(bytes, ".symtab");
    int const strtabIdx = findSectionByName(bytes, ".strtab");
    int const relaIdx   = findSectionByName(bytes, ".rela.text");
    ASSERT_GE(symtabIdx, 0);
    ASSERT_GE(strtabIdx, 0);
    ASSERT_GE(relaIdx, 0) << "an arm64 .o with a GOT-address extern must emit .rela.text";
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + symtabIdx * 64 + 24);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + strtabIdx * 64 + 24);
    std::uint64_t const relaOff   = readU64LE(bytes, shoff + relaIdx * 64 + 24);
    std::uint64_t const relaSize  = readU64LE(bytes, shoff + relaIdx * 64 + 32);
    std::size_t   const relaCount = static_cast<std::size_t>(relaSize / 24);

    // Scan .rela.text for the GOT pair (311/312) + assert the absolute
    // page-pair (275/277) is ABSENT for this symbol.
    bool sawGotPage = false, sawGotLo12 = false, sawAbsPage = false, sawAbsLo12 = false;
    std::uint64_t gotPageOff = 0, gotLo12Off = 0;
    std::uint32_t gotPageSym = 0, gotLo12Sym = 0;
    std::int64_t  gotPageAddend = -1, gotLo12Addend = -1;
    for (std::size_t i = 0; i < relaCount; ++i) {
        std::uint64_t const off  = readU64LE(bytes, relaOff + i * 24 + 0);
        std::uint64_t const info = readU64LE(bytes, relaOff + i * 24 + 8);
        std::int64_t const  add  = static_cast<std::int64_t>(readU64LE(bytes, relaOff + i * 24 + 16));
        std::uint32_t const type = static_cast<std::uint32_t>(info);
        std::uint32_t const sym  = static_cast<std::uint32_t>(info >> 32);
        if (type == 311u) { sawGotPage = true; gotPageOff = off; gotPageSym = sym; gotPageAddend = add; }
        if (type == 312u) { sawGotLo12 = true; gotLo12Off = off; gotLo12Sym = sym; gotLo12Addend = add; }
        if (type == 275u) sawAbsPage = true;
        if (type == 277u) sawAbsLo12 = true;
    }
    EXPECT_TRUE(sawGotPage)
        << "the adrp word must emit R_AARCH64_ADR_GOT_PAGE (nativeId 311).";
    EXPECT_TRUE(sawGotLo12)
        << "the ldr word must emit R_AARCH64_LD64_GOT_LO12_NC (nativeId 312).";
    EXPECT_FALSE(sawAbsPage)
        << "NO R_AARCH64_ADR_PREL_PG_HI21 (275) — a foreign default-PIE link "
           "rejects the absolute page-pair against a preemptible extern.";
    EXPECT_FALSE(sawAbsLo12)
        << "NO R_AARCH64_ADD_ABS_LO12_NC (277) for the GOT-address extern.";
    EXPECT_EQ(gotLo12Off, gotPageOff + 4)
        << "the ldr (312) sits exactly one word after the adrp (311).";
    EXPECT_EQ(gotPageAddend, 0) << "GOT-page addend = 0";
    EXPECT_EQ(gotLo12Addend, 0) << "GOT-lo12 addend = 0";
    EXPECT_EQ(gotPageSym, gotLo12Sym) << "both GOT relocs name the SAME symbol";

    // Both relocs name the SHN_UNDEF extern carrying its real import name.
    std::uint32_t const stName = readU32LE(bytes, symtabOff + gotPageSym * 24 + 0);
    EXPECT_EQ(readStrtabName(bytes, strtabOff, stName), "got_extern_fn")
        << "the address-taken extern must carry its real import name";
    EXPECT_EQ(readU16LE(bytes, symtabOff + gotPageSym * 24 + 6), 0u)   // st_shndx
        << "the address-taken extern must be SHN_UNDEF";
}

// ── D-LK-OBJECT-DATA-EXTERN-RELOCATABLE: data extern → PC32, not PLT32 ──

TEST(ElfWriter, ObjectDataExternRefEmitsUndefNameAndPc32NotPlt32) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    // A relocatable object references BOTH a libc DATA object (`stdout`, an
    // extern `isData` import) and a libc FUNCTION (`fputs`). The DATA
    // reference must emit a plain PC32 (type 2) — exactly what gcc emits for
    // `extern FILE *stdout` in a `.o`, which the final executable link
    // resolves by copy-relocation — while the CALL keeps the PLT-capable
    // PLT32 (type 4). This is the correctness pin for the isData exclusion in
    // externCallTargets: were the data ref PLT32, the final linker would bind
    // `stdout` to a PLT stub and the code would read jump-stub bytes as the
    // FILE* value (the silent miscompile the image-path reject guards). Both
    // externs are SHN_UNDEF NOTYPE (matching gcc's stdout/fputs symbols).
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{10};
    // Synthetic opcodes; only the two relocations drive the ELF Rela under
    // test. Byte 3: rel32 slot of a `mov rax,[rip+stdout]`; byte 8: rel32
    // slot of a `call fputs`.
    fn.bytes  = {0x48, 0x8B, 0x05, 0, 0, 0, 0,   // mov rax,[rip+disp32]  (7 bytes)
                 0xE8, 0, 0, 0, 0};              // call rel32            (5 bytes)
    fn.relocations.push_back(Relocation{/*offset=*/3u, /*target=*/SymbolId{20},
                                        /*kind=*/RelocationKind{1}, /*addend=*/0});
    fn.relocations.push_back(Relocation{/*offset=*/8u, /*target=*/SymbolId{30},
                                        /*kind=*/RelocationKind{1}, /*addend=*/0});
    mod.functions.push_back(std::move(fn));
    mod.symbols.push_back(ModuleSymbol{SymbolId{10}, "user_fn",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    ExternImport dataExt;
    dataExt.symbol      = SymbolId{20};
    dataExt.mangledName = "stdout";
    dataExt.libraryPath = "libc.so.6";
    dataExt.isData      = true;                       // the DATA extern
    mod.externImports.push_back(std::move(dataExt));
    ExternImport fnExt;
    fnExt.symbol      = SymbolId{30};
    fnExt.mangledName = "fputs";
    fnExt.libraryPath = "libc.so.6";
    fnExt.isData      = false;                        // the FUNCTION extern
    mod.externImports.push_back(std::move(fnExt));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "ET_REL must accept a data extern (D-LK-OBJECT-DATA-EXTERN-RELOCATABLE)";

    std::uint64_t const shoff     = readU64LE(bytes, 40);
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + 3 * 64 + 24);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + 4 * 64 + 24);

    // Symtab: 0=UNDEF, 1=STT_SECTION, 2=user_fn, then externs in first-
    // reference order → 3=stdout (reloc[0]), 4=fputs (reloc[1]).
    EXPECT_EQ(readStrtabName(bytes, strtabOff, readU32LE(bytes, symtabOff + 3 * 24 + 0)),
              "stdout") << "data extern carries its real libc name";
    EXPECT_EQ(bytes[symtabOff + 3 * 24 + 4], 0x10u)             // STB_GLOBAL|STT_NOTYPE
        << "data extern is NOTYPE (matches gcc's `stdout` symbol), not STT_OBJECT";
    EXPECT_EQ(readU16LE(bytes, symtabOff + 3 * 24 + 6), 0u)     // st_shndx = SHN_UNDEF
        << "data extern must be SHN_UNDEF";
    EXPECT_EQ(readStrtabName(bytes, strtabOff, readU32LE(bytes, symtabOff + 4 * 24 + 0)),
              "fputs") << "function extern follows in reference order";

    // .rela.text (section index 2): entry 0 = data ref, entry 1 = the call.
    std::uint64_t const relaOff = readU64LE(bytes, shoff + 2 * 64 + 24);
    std::uint64_t const rInfo0  = readU64LE(bytes, relaOff + 0 * 24 + 8);
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo0), 2u)
        << "a DATA extern reference must be R_X86_64_PC32 (2), NOT PLT32 (4)";
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo0 >> 32), 3u)
        << "reloc 0 targets the stdout symbol (symtab idx 3)";
    std::uint64_t const rInfo1  = readU64LE(bytes, relaOff + 1 * 24 + 8);
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo1), 4u)
        << "the FUNCTION call keeps R_X86_64_PLT32 (4) — the isData exclusion "
           "must not weaken function-call dispatch";
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo1 >> 32), 4u)
        << "reloc 1 targets the fputs symbol (symtab idx 4)";
}

// ── D-LK-OBJECT-DATA-SECTION-RELOCATABLE: .data + section-relative sym ──

TEST(ElfWriter, ObjectEmitsDataSectionAndSectionRelativeDataSymbol) {
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);   // one `ret` function
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Data;
    d.bytes     = {7, 0, 0, 0};                            // int counter = 7
    d.alignment = Alignment::of<4>();
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{42}, "counter",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "ET_REL must now accept a Data item (D-LK-OBJECT-DATA-SECTION-RELOCATABLE)";

    // Section table: NULL(0), .text(1), .data(2), .rela.text(3), .symtab(4),
    // .strtab(5), .shstrtab(6), .note.GNU-stack(7).
    EXPECT_EQ(readU16LE(bytes, 60), 8u) << "e_shnum grows to 8 with .data";
    std::uint64_t const shoff = readU64LE(bytes, 40);
    // .data (index 2): SHT_PROGBITS(1); SHF_ALLOC|SHF_WRITE(3); sh_addr=0 (unbound).
    EXPECT_EQ(readU32LE(bytes, shoff + 2 * 64 + 4), 1u)  << ".data SHT_PROGBITS";
    EXPECT_EQ(readU64LE(bytes, shoff + 2 * 64 + 8), 3u)  << ".data SHF_ALLOC|SHF_WRITE";
    EXPECT_EQ(readU64LE(bytes, shoff + 2 * 64 + 16), 0u) << ".data sh_addr=0 in a .o";

    // .symtab at index 4; the data symbol at symtab idx 3
    // (0=UNDEF, 1=.text section, 2=func, 3=data).
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + 4 * 64 + 24);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + 5 * 64 + 24);
    std::uint32_t const stName = readU32LE(bytes, symtabOff + 3 * 24 + 0);
    EXPECT_EQ(readStrtabName(bytes, strtabOff, stName), "counter")
        << "data global carries its real name (red-on-disable vs sym_42)";
    EXPECT_EQ(bytes[symtabOff + 3 * 24 + 4], 0x11u)             // STB_GLOBAL|STT_OBJECT
        << "data symbol is STB_GLOBAL|STT_OBJECT";
    EXPECT_EQ(readU16LE(bytes, symtabOff + 3 * 24 + 6), 2u)     // st_shndx = .data
        << "data symbol st_shndx names .data (index 2)";
    EXPECT_EQ(readU64LE(bytes, symtabOff + 3 * 24 + 8), 0u)     // st_value
        << "section-relative st_value (offset 0 in .data)";
    EXPECT_EQ(readU64LE(bytes, symtabOff + 3 * 24 + 16), 4u)    // st_size
        << "st_size = sizeof(int)";
}

// ── Rela r_info encodes (sym << 32) | nativeId ──────────────────

TEST(ElfWriter, RelaRecordsNativeRelocTypeAndSymtabIndex) {
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction caller;
    caller.symbol = SymbolId{1};
    // Synthetic 5-byte 'call rel32' with the displacement zeroed
    // — the assembler did its job; we just verify the ELF Rela.
    caller.bytes = {0xE8, 0x00, 0x00, 0x00, 0x00};
    Relocation rel;
    rel.offset = 1;                  // patch site = byte 1 (the rel32)
    rel.target = SymbolId{2};        // undefined extern
    rel.kind   = RelocationKind{1};  // matches `rel32` (PC32 in elf JSON)
    rel.addend = 0;                  // real codegen stamps 0; the psABI -4 for a
                                     // rel32 field lives in the target schema's
                                     // addendBias, baked into r_addend at emit
                                     // (D-LK-OBJECT-RELOC-ADDEND-CROSSTOOLCHAIN)
    caller.relocations.push_back(rel);
    mod.functions.push_back(std::move(caller));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint64_t const relaOff = readU64LE(bytes, shoff + 2 * 64 + 24);
    std::uint64_t const relaSize = readU64LE(bytes, shoff + 2 * 64 + 32);
    ASSERT_EQ(relaSize, 24u);

    // r_offset = patch site within .text = 1 (caller starts at 0).
    EXPECT_EQ(readU64LE(bytes, relaOff + 0), 1u);
    std::uint64_t const rInfo = readU64LE(bytes, relaOff + 8);
    std::uint32_t const symIdx = static_cast<std::uint32_t>(rInfo >> 32);
    std::uint32_t const type   = static_cast<std::uint32_t>(rInfo);
    EXPECT_EQ(type, 2u) << "R_X86_64_PC32 = 2";
    // sym 0 = STN_UNDEF, sym 1 = section, sym 2 = caller, sym 3 = target.
    EXPECT_EQ(symIdx, 3u);
    // r_addend == -4 = rel.addend(0) + the rel32 addendBias(-4), baked in for a
    // FOREIGN linker (D-LK-OBJECT-RELOC-ADDEND-CROSSTOOLCHAIN). A rel32 field is
    // relative to the instruction END (4 bytes past the reloc offset), so gcc's
    // ld — applying the psABI S + A - P with no knowledge of DSS's addendBias —
    // needs A = -4 to land on the symbol. Red-on-disable: emitting rel.addend
    // verbatim (0) makes gcc resolve a call to sym+4 → SIGSEGV.
    std::int64_t const addend = static_cast<std::int64_t>(readU64LE(bytes, relaOff + 16));
    EXPECT_EQ(addend, -4)
        << "psABI r_addend for a rel32 call must be -4 (the addendBias baked in)";
}

// ── c145: reloc-bearing const data → .data.rel.ro + .rela.data.rel.ro ──

TEST(ElfWriter, RelRoConstDataItemEmitsDataRelRoSectionAndRelaDataRelRo) {
    // D-LK-RELRO-CONST-DATA-RELOCATABLE (c145): a CONST global that carries a
    // LOAD-TIME relocation (a const function-pointer table / `int (*const p)() =
    // f;` — sqlite's VFS method tables + aSyscall[]) must land in `.data.rel.ro`
    // (gcc's contract: SHT_PROGBITS + SHF_ALLOC|SHF_WRITE) and the ET_REL writer
    // must emit its OWN relocation into `.rela.data.rel.ro` as R_X86_64_64 (abs64,
    // addend 0), sh_info → the target data section. RED-on-disable: before c145 a
    // reloc-bearing data item fail-louded (D-LK1-ELF-RODATA-DATAITEM-RELOC).
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);   // one `ret` fn, sym 1
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::RelRoConst;
    d.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};   // 8-byte pointer slot
    d.alignment = Alignment::of<8>();
    Relocation r;
    r.offset = 0;
    r.target = SymbolId{1};          // the slot points at the function
    r.kind   = RelocationKind{2};    // abs64 → R_X86_64_64 (elf JSON kind 2)
    r.addend = 0;
    d.relocations.push_back(r);
    mod.dataItems.push_back(std::move(d));
    mod.symbols.push_back(ModuleSymbol{SymbolId{42}, "vtable",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "reloc-bearing CONST data must now compile to relro, not fail loud";
    ASSERT_FALSE(bytes.empty());

    std::uint64_t const shoff = readU64LE(bytes, 40);

    // `.data.rel.ro`: SHT_PROGBITS(1), SHF_ALLOC|SHF_WRITE(3), sh_addr=0, size 8.
    int const relroIdx = findSectionByName(bytes, ".data.rel.ro");
    ASSERT_GE(relroIdx, 0) << ".data.rel.ro section must be emitted";
    EXPECT_EQ(readU32LE(bytes, shoff + relroIdx * 64 + 4), 1u) << "SHT_PROGBITS";
    EXPECT_EQ(readU64LE(bytes, shoff + relroIdx * 64 + 8), 3u)
        << "SHF_ALLOC|SHF_WRITE (gcc's .data.rel.ro flags — NOT read-only .rodata)";
    EXPECT_EQ(readU64LE(bytes, shoff + relroIdx * 64 + 16), 0u) << "sh_addr=0 in .o";
    EXPECT_EQ(readU64LE(bytes, shoff + relroIdx * 64 + 32), 8u)
        << "sh_size = one 8-byte pointer slot";

    // `.rela.data.rel.ro`: SHT_RELA(4), sh_link=.symtab, sh_info=.data.rel.ro.
    int const symtabIdx = findSectionByName(bytes, ".symtab");
    int const relaIdx   = findSectionByName(bytes, ".rela.data.rel.ro");
    ASSERT_GE(symtabIdx, 0);
    ASSERT_GE(relaIdx, 0) << ".rela.data.rel.ro section must be emitted";
    EXPECT_EQ(readU32LE(bytes, shoff + relaIdx * 64 + 4), 4u) << "SHT_RELA";
    EXPECT_EQ(static_cast<int>(readU32LE(bytes, shoff + relaIdx * 64 + 40)), symtabIdx)
        << "sh_link names .symtab";
    EXPECT_EQ(static_cast<int>(readU32LE(bytes, shoff + relaIdx * 64 + 44)), relroIdx)
        << "sh_info names the target .data.rel.ro section";

    // The single Elf64_Rela: r_offset = itemOffset(0)+rel.offset(0) = 0;
    // r_info low32 = R_X86_64_64 (1, NOT the PLT variant — a data reloc is never a
    // call); r_addend = 0 (abs64 addendBias 0).
    std::uint64_t const relaOff  = readU64LE(bytes, shoff + relaIdx * 64 + 24);
    std::uint64_t const relaSize = readU64LE(bytes, shoff + relaIdx * 64 + 32);
    ASSERT_EQ(relaSize, 24u) << "exactly one Elf64_Rela";
    EXPECT_EQ(readU64LE(bytes, relaOff + 0), 0u) << "r_offset = 0";
    std::uint64_t const rInfo = readU64LE(bytes, relaOff + 8);
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo), 1u)
        << "R_X86_64_64 = 1 (abs64) — the native reloc id from the format JSON";
    EXPECT_EQ(static_cast<std::int64_t>(readU64LE(bytes, relaOff + 16)), 0)
        << "r_addend = 0";

    // The reloc's symIdx names the FUNCTION symbol (st_shndx = .text = idx 1).
    std::uint32_t const relSymIdx = static_cast<std::uint32_t>(rInfo >> 32);
    std::uint64_t const symtabOff = readU64LE(bytes, shoff + symtabIdx * 64 + 24);
    EXPECT_EQ(readU16LE(bytes, symtabOff + relSymIdx * 24 + 6), 1u)
        << "reloc target resolves to the function symbol (st_shndx = .text)";

    // The relro DATA global itself: STT_OBJECT, st_shndx = .data.rel.ro.
    int const strtabIdx = findSectionByName(bytes, ".strtab");
    ASSERT_GE(strtabIdx, 0);
    std::uint64_t const strtabOff = readU64LE(bytes, shoff + strtabIdx * 64 + 24);
    std::uint64_t const symtabSize = readU64LE(bytes, shoff + symtabIdx * 64 + 32);
    bool sawVtable = false;
    for (std::uint64_t o = 0; o + 24 <= symtabSize; o += 24) {
        std::uint32_t const nm = readU32LE(bytes, symtabOff + o + 0);
        if (readStrtabName(bytes, strtabOff, nm) == "vtable") {
            sawVtable = true;
            EXPECT_EQ(bytes[symtabOff + o + 4], 0x11u) << "STB_GLOBAL|STT_OBJECT";
            EXPECT_EQ(readU16LE(bytes, symtabOff + o + 6),
                      static_cast<std::uint16_t>(relroIdx))
                << "relro data symbol st_shndx names .data.rel.ro";
            EXPECT_EQ(readU64LE(bytes, symtabOff + o + 16), 8u) << "st_size = 8";
        }
    }
    EXPECT_TRUE(sawVtable) << "the relro data global must have a .symtab entry";
}

TEST(ElfWriter, RelRoConstDataRelocIsMachineAgnosticOnAarch64) {
    // AGNOSTIC pin: the SAME machine-neutral ELF ET_REL writer emits the relro
    // reloc for arm64 with the arm64 native id — R_AARCH64_ABS64 (257), read from
    // the arm64 format JSON, NOT hardcoded. RED-on-disable: a machine branch in
    // the writer (or a missing aarch64 `.data.rel.ro`/`relro` row) breaks this.
    auto target = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(target.has_value());
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux");
    ASSERT_TRUE(fmt.has_value());
    AssembledModule mod = makeTrivialModule({0xC0, 0x03, 0x5F, 0xD6}, 1);  // `ret`
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::RelRoConst;
    d.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    d.alignment = Alignment::of<8>();
    Relocation r;
    r.offset = 0;
    r.target = SymbolId{1};
    r.kind   = RelocationKind{4};    // arm64 abs64 → R_AARCH64_ABS64 (JSON kind 4)
    r.addend = 0;
    d.relocations.push_back(r);
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, **target, **fmt, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    std::uint64_t const shoff = readU64LE(bytes, 40);
    int const relaIdx = findSectionByName(bytes, ".rela.data.rel.ro");
    ASSERT_GE(relaIdx, 0) << "arm64 .o must also emit .rela.data.rel.ro";
    std::uint64_t const relaOff = readU64LE(bytes, shoff + relaIdx * 64 + 24);
    std::uint64_t const rInfo   = readU64LE(bytes, relaOff + 8);
    EXPECT_EQ(static_cast<std::uint32_t>(rInfo), 257u)
        << "R_AARCH64_ABS64 = 257 — the arm64 native id from the format JSON";
}

TEST(ElfWriter, MutablePointerDataItemEmitsRelaData) {
    // c145: a reloc-bearing MUTABLE pointer global (`int *p = &x;`) lands in
    // `.data` (writable) and gets `.rela.data` — the same mechanism as relro but
    // NOT read-only-after. Pins that the generalized `.rela.<section>` covers
    // `.data` too, not only relro.
    auto loaded = loadShipped();
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    AssembledData d;
    d.symbol    = SymbolId{42};
    d.section   = DataSectionKind::Data;   // MUTABLE
    d.bytes     = {0, 0, 0, 0, 0, 0, 0, 0};
    d.alignment = Alignment::of<8>();
    Relocation r;
    r.offset = 0;
    r.target = SymbolId{1};
    r.kind   = RelocationKind{2};    // abs64
    r.addend = 0;
    d.relocations.push_back(r);
    mod.dataItems.push_back(std::move(d));

    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u)
        << "reloc-bearing MUTABLE data must compile (was rejected pre-c145)";
    ASSERT_FALSE(bytes.empty());
    std::uint64_t const shoff = readU64LE(bytes, 40);
    int const dataIdx = findSectionByName(bytes, ".data");
    int const relaIdx = findSectionByName(bytes, ".rela.data");
    ASSERT_GE(dataIdx, 0);
    ASSERT_GE(relaIdx, 0) << ".rela.data must be emitted for a reloc-bearing .data item";
    EXPECT_EQ(static_cast<int>(readU32LE(bytes, shoff + relaIdx * 64 + 44)), dataIdx)
        << ".rela.data sh_info names .data";
    std::uint64_t const relaOff = readU64LE(bytes, shoff + relaIdx * 64 + 24);
    EXPECT_EQ(static_cast<std::uint32_t>(readU64LE(bytes, relaOff + 8)), 1u)
        << "R_X86_64_64 abs64";
    // No `.data.rel.ro` for a mutable-only module.
    EXPECT_LT(findSectionByName(bytes, ".data.rel.ro"), 0)
        << "a mutable pointer must NOT create a .data.rel.ro section";
}

// ── Non-ELF schema → fail-loud ──────────────────────────────────

TEST(ElfWriter, NonElfFormatKindEmitsK_NoMatchingObjectFormat) {
    auto loaded = loadShipped();
    // Construct a non-ELF schema inline (kind = "wasm" — substrate
    // accepts the load but the ELF walker rejects).
    auto wasmJson = R"({
      "dssObjectFormatVersion": 1,
  "dataModel": "LP64",
      "format": {"name":"wasm-test","kind":"wasm"}
    })";
    auto wasm = ObjectFormatSchema::loadFromText(wasmJson);
    ASSERT_TRUE(wasm.has_value());

    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, **wasm, rep);
    EXPECT_TRUE(bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_NoMatchingObjectFormat) sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── linkagePassed gate: walker skipped on cross-reference failure ──

TEST(LinkerEndToEnd, LinkagePassedGateSkipsWalkerOnSymbolUndefined) {
    // When the cross-reference unifier emits K_SymbolUndefined, the
    // format walker must NOT run — emitting partial bytes from a
    // known-invalid module is exactly the silent-failure class the
    // substrate discipline rejects. Pins architect's Decision 4
    // gap on `resolvedFuncCount` reset.
    auto loaded = loadShipped();
    AssembledModule mod;
    mod.expectedFuncCount = 1;
    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    Relocation rel;
    rel.target = SymbolId{99};       // undefined
    rel.kind   = RelocationKind{1};  // valid kind
    fn.relocations.push_back(rel);
    mod.functions.push_back(std::move(fn));

    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.bytes.empty())
        << "walker must not have run when linkagePassed=false";
    EXPECT_FALSE(image.ok())
        << "ok() must reflect failure (resolvedFuncCount reset to 0)";
    EXPECT_EQ(image.resolvedFuncCount, 0u);
}

// ── Dispatch arms for non-ELF formats fire K_NoMatchingObjectFormat ──

namespace {

[[nodiscard]] std::shared_ptr<ObjectFormatSchema>
loadStubFormat(std::string_view kindName) {
    std::string const json = std::string{"{\"dssObjectFormatVersion\":1,"
                                         "\"dataModel\":\"LP64\","
                                         "\"format\":{\"name\":\"stub-"}
        + std::string{kindName} + "\",\"kind\":\"" + std::string{kindName}
        + "\"}}";
    auto r = ObjectFormatSchema::loadFromText(json);
    EXPECT_TRUE(r.has_value());
    return r.has_value() ? std::move(r).value() : nullptr;
}

} // namespace

// Note: ALL five `ObjectFormatKind` arms now have real walkers
// (Pe: LK2 cycle 1; MachO: LK3 cycle 1; Wasm: LK8 skeleton; Spirv:
// LK9 skeleton — both skeletons emit format-spec module headers
// and route walker-input-contract violations to
// `K_WalkerInputContractViolation`). The closed-enum dispatch in
// `linker.cpp` has no no-walker-registered path other than the
// `Unknown` sentinel. PE end-to-end coverage lives in
// `tests/link/test_pe_writer.cpp`; Mach-O in `test_macho_writer.cpp`;
// Wasm in `test_wasm_writer.cpp`; Spirv in `test_spirv_writer.cpp`.

TEST(LinkerEndToEnd, WasmFormatDispatchRoutesToWalker) {
    // LK8 (landed 2026-05-30): Wasm arm now dispatches to the
    // wasm::encode walker (8-byte module preamble per WASM spec
    // §5.5). A native-ISA-bytes-bearing AssembledModule routed
    // here triggers K_WalkerInputContractViolation (0x8005) —
    // the LK8 skeleton's input-contract guard for
    // !functions.empty(). The previous K_NoMatchingObjectFormat
    // is reserved for the still-unimplemented Spirv arm.
    auto loaded = loadShipped();
    auto w = loadStubFormat("wasm");
    ASSERT_TRUE(w);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *w, rep);
    EXPECT_TRUE(image.bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_WalkerInputContractViolation)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

TEST(LinkerEndToEnd, SpirvFormatDispatchRoutesToWalker) {
    // LK9 (landed 2026-05-30): Spirv arm now dispatches to the
    // spirv::encode walker (20-byte module header per SPIR-V Spec
    // §2.3). A native-ISA-bytes-bearing AssembledModule routed
    // here triggers K_WalkerInputContractViolation (0x8005) —
    // the LK9 skeleton's input-contract guard for
    // !functions.empty(). Symmetric with LK8's WASM gate.
    auto loaded = loadShipped();
    auto s = loadStubFormat("spirv");
    ASSERT_TRUE(s);
    AssembledModule mod = makeTrivialModule({0xC3}, 1);
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *s, rep);
    EXPECT_TRUE(image.bytes.empty());
    bool sawCode = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_WalkerInputContractViolation)
            sawCode = true;
    }
    EXPECT_TRUE(sawCode);
}

// ── End-to-end via the format-blind `link()` dispatch ───────────

TEST(LinkerEndToEnd, ElfDispatchProducesNonEmptyBytes) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 99);
    DiagnosticReporter rep;
    auto image = linker::link(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(image.ok());
    EXPECT_EQ(image.format, ObjectFormatKind::Elf);
    EXPECT_FALSE(image.bytes.empty()) << "linker dispatch should fire walker";
    EXPECT_EQ(rep.errorCount(), 0u);
    // The bytes ARE the ELF .o — sanity-check the magic.
    ASSERT_GE(image.bytes.size(), 4u);
    EXPECT_EQ(image.bytes[0], 0x7F);
    EXPECT_EQ(image.bytes[1], 'E');
}
