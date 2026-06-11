// ELF64 .o writer tests — plan 14 LK1 cycle 1.
//
// Pins golden byte-level invariants of the emitted ELF64 relocatable
// object:
//   * Elf64_Ehdr identity bytes match the gABI spec for ET_REL x86_64.
//   * Section count = 6 (SHT_NULL + .text + .rela.text + .symtab +
//     .strtab + .shstrtab).
//   * e_shstrndx points at .shstrtab (index 5).
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
#include "link/format/elf.hpp"
#include "link/linker.hpp"
#include "link/object_format_schema.hpp"
#include "link_test_support.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
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
    // e_shnum = 6
    EXPECT_EQ(readU16LE(bytes, 60), 6u);
    // e_shstrndx = 5
    EXPECT_EQ(readU16LE(bytes, 62), 5u);
}

// ── Section header table layout ─────────────────────────────────

TEST(ElfWriter, SectionHeaderTableHasSixEntriesNullThenTextThenRelocAtEnd) {
    auto loaded = loadShipped();
    AssembledModule mod = makeTrivialModule({0xC3}, 42);
    DiagnosticReporter rep;
    auto bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    // e_shoff is the file offset of the section header table.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    ASSERT_GT(shoff, 64u);
    ASSERT_LE(shoff + 6 * 64, bytes.size())
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
    rel.addend = -4;
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
    // r_addend == -4 (assembler's rel32 bias).
    std::int64_t const addend = static_cast<std::int64_t>(readU64LE(bytes, relaOff + 16));
    EXPECT_EQ(addend, -4);
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
