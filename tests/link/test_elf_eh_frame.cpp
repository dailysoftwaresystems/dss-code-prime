// ELF `.eh_frame` / `.eh_frame_hdr` / `PT_GNU_EH_FRAME` writer pins —
// D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO.
//
// ✔MEASURED 2026-08-13, the defect these pins close: `readelf -S` on a DSS
// elf64 build of ordinary C found **ZERO** `.eh_frame` sections where gcc's
// build of the same source has one — so nothing, not a debugger and not the
// running process, could walk a DSS stack frame.
//
// ★★ THE ONE ASSERTION THAT MATTERS MOST IS THE PERMUTATION.
//    DSS's physical ordinal for `%rsp` is 4 and its x86 hardware encoding is
//    also 4 — but its DWARF number is **7**. Four registers move
//    (`rdx`/`rcx` swap, `rsi`/`rdi` become 4/5, `rbp`/`rsp` become 6/7), so an
//    encoder that reached for `hwEncoding` — the field sitting right beside
//    `dwarfNumber` in the same struct — would emit a table that every reader
//    ACCEPTS and that names the WRONG registers. There is no crash, no
//    warning, and no wrong byte anywhere else in the image: just a backtrace
//    pointing at the wrong frame. `CieDefCfaNamesTheDwarfNumberNotTheHardwareEncoding`
//    is the assertion that fails if that ever happens, and it is written as a
//    byte equality against 7 with the value 4 named in the failure message so
//    the mistake is self-diagnosing.
//
// ★ The pins here are STRUCTURAL (the writer's own bytes). The behavioural
//   witness — `gdb` unwinding a real 4-frame DSS stack, and the emitted CIE
//   matching gcc's field-for-field — was run by hand on WSL and is recorded in
//   the cycle report; it cannot run in ctest on a Windows host.

#include "asm/asm.hpp"
#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "link/format/dwarf_cfi.hpp"
#include "link/format/elf.hpp"
#include "link/object_format_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] std::uint16_t readU16LE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(b[off])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[off + 1]) << 8));
}
[[nodiscard]] std::uint32_t readU32LE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b[off + i]) << (i * 8);
    return v;
}
[[nodiscard]] std::uint64_t readU64LE(std::span<std::uint8_t const> b,
                                      std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    return v;
}

struct Loaded {
    std::shared_ptr<TargetSchema>       target;
    std::shared_ptr<ObjectFormatSchema> format;
};

// The SHIPPED schemas, never a hand-written stand-in: the whole subject is
// whether `x86_64.target.json`'s DWARF numbering reaches the writer, and a
// fixture that typed its own numbering would be testing the fixture.
[[nodiscard]] Loaded loadShipped(std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped("x86_64");
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(x86_64) failed";
        for (auto const& d : t.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
        for (auto const& d : f.error()) ADD_FAILURE() << "  " << d.message;
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

struct Shdr {
    std::string   name;
    std::uint32_t type = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0;
};

[[nodiscard]] std::string readCStr(std::vector<std::uint8_t> const& b,
                                   std::uint64_t off) {
    std::string s;
    for (std::uint64_t p = off; p < b.size() && b[p] != 0; ++p) {
        s.push_back(static_cast<char>(b[p]));
    }
    return s;
}

[[nodiscard]] std::vector<Shdr> readSections(std::vector<std::uint8_t> const& b) {
    std::vector<Shdr> out;
    std::uint64_t const shoff = readU64LE(b, 40);
    std::uint16_t const shnum = readU16LE(b, 60);
    std::uint16_t const shstrndx = readU16LE(b, 62);
    if (shoff == 0 || shnum == 0) return out;
    std::uint64_t const strOff = readU64LE(b, shoff + shstrndx * 64 + 24);
    for (std::uint16_t i = 0; i < shnum; ++i) {
        std::uint64_t const o = shoff + i * 64;
        Shdr s;
        s.name   = readCStr(b, strOff + readU32LE(b, o + 0));
        s.type   = readU32LE(b, o + 4);
        s.flags  = readU64LE(b, o + 8);
        s.addr   = readU64LE(b, o + 16);
        s.offset = readU64LE(b, o + 24);
        s.size   = readU64LE(b, o + 32);
        out.push_back(std::move(s));
    }
    return out;
}

[[nodiscard]] Shdr const* findSection(std::vector<Shdr> const& secs,
                                      std::string const& name) {
    for (auto const& s : secs) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

struct Phdr {
    std::uint32_t type = 0, flags = 0;
    std::uint64_t offset = 0, vaddr = 0, filesz = 0, memsz = 0, align = 0;
};

[[nodiscard]] std::vector<Phdr> readPhdrs(std::vector<std::uint8_t> const& b) {
    std::vector<Phdr> out;
    std::uint64_t const phoff = readU64LE(b, 32);
    std::uint16_t const phnum = readU16LE(b, 56);
    for (std::uint16_t i = 0; i < phnum; ++i) {
        std::uint64_t const o = phoff + i * 56;
        Phdr p;
        p.type   = readU32LE(b, o + 0);
        p.flags  = readU32LE(b, o + 4);
        p.offset = readU64LE(b, o + 8);
        p.vaddr  = readU64LE(b, o + 16);
        p.filesz = readU64LE(b, o + 32);
        p.memsz  = readU64LE(b, o + 40);
        p.align  = readU64LE(b, o + 48);
        out.push_back(p);
    }
    return out;
}

constexpr std::uint32_t kPtGnuEhFrame = 0x6474e550;

// ── The module under test ───────────────────────────────────────
//
// `withCfi` is the ONLY difference between the two arms — same symbols, same
// bytes, same relocations — so every assertion about presence/absence is
// attributable to the call-frame information and to nothing else.
[[nodiscard]] AssembledModule makeModule(bool withCfi) {
    AssembledModule mod;
    mod.expectedFuncCount = 2;

    // DSS ordinals: rsp = 4, r14 = 14, r15 = 15.
    auto entryState = [] {
        CfiInitialState s;
        s.cfaRegister = 4;                   // rsp -> DWARF 7 (the permutation)
        s.cfaOffset   = 8;
        s.returnAddressAtCfaOffset = -8;
        return s;
    };

    AssembledFunction fn;
    fn.symbol = SymbolId{1};
    fn.bytes  = {0x48, 0x83, 0xEC, 0x20,     // sub rsp, 0x20
                 0x4C, 0x89, 0x34, 0x24,     // mov [rsp], r14
                 0x48, 0x83, 0xC4, 0x20,     // add rsp, 0x20
                 0xC3};                      // ret
    if (withCfi) {
        CfiFunction c;
        c.codeLength    = static_cast<std::uint32_t>(fn.bytes.size());
        c.initial       = entryState();
        c.prologueEndPc = 8;
        c.ops = {
            CfiOp{4,  CfiOpKind::DefCfaOffset,   CfiRegRef{},             CfiRegRef{}, 40},
            CfiOp{8,  CfiOpKind::RegAtCfaOffset, CfiRegRef::physical(14), CfiRegRef{}, -40},
            CfiOp{12, CfiOpKind::DefCfaOffset,   CfiRegRef{},             CfiRegRef{},  8},
        };
        fn.cfi = std::move(c);
    }
    mod.functions.push_back(std::move(fn));

    AssembledFunction fn2;
    fn2.symbol = SymbolId{2};
    fn2.bytes  = {0x48, 0x83, 0xEC, 0x10, 0x48, 0x83, 0xC4, 0x10, 0xC3};
    if (withCfi) {
        CfiFunction c;
        c.codeLength    = static_cast<std::uint32_t>(fn2.bytes.size());
        c.initial       = entryState();
        c.prologueEndPc = 4;
        c.ops = {
            CfiOp{4, CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{}, 24},
            CfiOp{8, CfiOpKind::DefCfaOffset, CfiRegRef{}, CfiRegRef{},  8},
        };
        fn2.cfi = std::move(c);
    }
    mod.functions.push_back(std::move(fn2));

    mod.symbols.push_back(ModuleSymbol{SymbolId{1}, "dss_a",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    mod.symbols.push_back(ModuleSymbol{SymbolId{2}, "dss_b",
                                       SymbolBinding::Global,
                                       SymbolVisibility::Default});
    return mod;
}

[[nodiscard]] std::vector<std::uint8_t> encodeWith(bool withCfi,
                                                   DiagnosticReporter& rep) {
    auto loaded = loadShipped("elf64-x86_64-linux-dyn");
    if (!loaded.target || !loaded.format) return {};
    AssembledModule mod = makeModule(withCfi);
    return elf::encode(mod, *loaded.target, *loaded.format, rep);
}

} // namespace

TEST(ElfEhFrame, BothSectionsAndTheSegmentAppearWhenFunctionsCarryFrameInfo) {
    DiagnosticReporter rep;
    auto const bytes = encodeWith(/*withCfi=*/true, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const secs = readSections(bytes);
    Shdr const* eh  = findSection(secs, ".eh_frame");
    Shdr const* hdr = findSection(secs, ".eh_frame_hdr");
    ASSERT_NE(eh, nullptr)
        << "the defect this closes IS the absent section: readelf -S found "
           "ZERO .eh_frame on a DSS elf64 image where gcc emits one";
    ASSERT_NE(hdr, nullptr);
    // SHF_ALLOC (2): these are MAPPED, not debug-only. A `.eh_frame` the
    // loader does not map is readable by a debugger and invisible to the
    // running process's own unwinder.
    EXPECT_EQ(eh->flags & 2u, 2u);
    EXPECT_EQ(hdr->flags & 2u, 2u);
    EXPECT_EQ(eh->type, 1u) << "SHT_PROGBITS";
    EXPECT_GT(eh->size, 0u);

    // PT_GNU_EH_FRAME must point at `.eh_frame_hdr` — NOT at `.eh_frame`.
    // Aiming it one section over yields a segment every reader accepts and
    // every unwinder mis-parses (it reads a CIE length word as a version byte).
    auto const phs = readPhdrs(bytes);
    Phdr const* ehSeg = nullptr;
    for (auto const& p : phs) {
        if (p.type == kPtGnuEhFrame) ehSeg = &p;
    }
    ASSERT_NE(ehSeg, nullptr) << "no PT_GNU_EH_FRAME — the runtime unwinder "
                                 "finds the tables through dl_iterate_phdr and "
                                 "would find nothing";
    EXPECT_EQ(ehSeg->vaddr, hdr->addr);
    EXPECT_EQ(ehSeg->offset, hdr->offset);
    EXPECT_EQ(ehSeg->filesz, hdr->size);
    EXPECT_EQ(ehSeg->memsz, hdr->size);
    EXPECT_NE(ehSeg->vaddr, eh->addr)
        << "the segment must name .eh_frame_hdr, not .eh_frame";
}

TEST(ElfEhFrame, CieDefCfaNamesTheDwarfNumberNotTheHardwareEncoding) {
    DiagnosticReporter rep;
    auto const bytes = encodeWith(/*withCfi=*/true, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const secs = readSections(bytes);
    Shdr const* eh = findSection(secs, ".eh_frame");
    ASSERT_NE(eh, nullptr);

    // CIE layout: length(4) id(4) version(1) "zR\0"(3) code_align(uleb)
    // data_align(sleb) ra_column(uleb) aug_len(uleb) fde_encoding(1),
    // then the initial instructions.
    std::uint64_t p = eh->offset;
    EXPECT_EQ(readU32LE(bytes, p + 4), 0u) << "CIE_id is 0 in .eh_frame";
    EXPECT_EQ(bytes[p + 8], 1u) << "CIE version";
    EXPECT_EQ(bytes[p + 9],  static_cast<std::uint8_t>('z'));
    EXPECT_EQ(bytes[p + 10], static_cast<std::uint8_t>('R'));
    EXPECT_EQ(bytes[p + 11], 0u);
    EXPECT_EQ(bytes[p + 12], 1u)    << "code alignment factor";
    EXPECT_EQ(bytes[p + 13], 0x7Fu) << "data alignment factor = -1 (sleb)";
    EXPECT_EQ(bytes[p + 14], 16u)
        << "return address column MUST be 16 on x86_64 SysV — a SYNTHETIC "
           "column that is not a register, which is exactly why it is a "
           "declared /target/dwarfReturnAddressColumn and not derived";
    EXPECT_EQ(bytes[p + 15], 1u)    << "augmentation data length";
    EXPECT_EQ(bytes[p + 16], 0x1Bu) << "FDE pointer encoding = pcrel|sdata4";

    // ★★ THE PERMUTATION ASSERTION.
    EXPECT_EQ(bytes[p + 17], 0x0Cu) << "DW_CFA_def_cfa";
    EXPECT_EQ(bytes[p + 18], 7u)
        << "the CFA base register must be DWARF **7** (%rsp). DSS's physical "
           "ordinal for %rsp is 4 and its x86 hwEncoding is ALSO 4 — reading "
           "either of those here emits a table naming a DIFFERENT register "
           "that every reader accepts without complaint, and an unwinder "
           "follows it into the wrong frame. A 4 in this byte means the "
           "writer reached for hwEncoding instead of dwarfNumber.";
    EXPECT_EQ(bytes[p + 19], 8u) << "CFA offset at entry = callPushBytes";
    // The RA rule: DW_CFA_offset|16 is 0x90 (0x80 | 16), then the unfactored
    // -(-8)/-1 = 8.
    EXPECT_EQ(bytes[p + 20], 0x90u)
        << "DW_CFA_offset for the return-address column (0x80 | 16)";
}

TEST(ElfEhFrame, EhFrameHdrIsTheSearchTableAndCountsEveryFde) {
    DiagnosticReporter rep;
    auto const bytes = encodeWith(/*withCfi=*/true, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const secs = readSections(bytes);
    Shdr const* hdr = findSection(secs, ".eh_frame_hdr");
    Shdr const* eh  = findSection(secs, ".eh_frame");
    ASSERT_NE(hdr, nullptr);
    ASSERT_NE(eh, nullptr);

    std::uint64_t const p = hdr->offset;
    // ✔MEASURED 2026-08-13: gcc 13.3.0's own `.eh_frame_hdr` on the same
    // machine begins with these four bytes, byte for byte.
    EXPECT_EQ(bytes[p + 0], 1u)    << "version — glibc rejects anything else";
    EXPECT_EQ(bytes[p + 1], 0x1Bu) << "eh_frame_ptr encoding = pcrel|sdata4";
    EXPECT_EQ(bytes[p + 2], 0x03u) << "fde_count encoding = udata4";
    EXPECT_EQ(bytes[p + 3], 0x3Bu) << "table encoding = datarel|sdata4";

    // eh_frame_ptr is pcrel from its OWN field, and must resolve to `.eh_frame`.
    auto const rel = static_cast<std::int32_t>(readU32LE(bytes, p + 4));
    EXPECT_EQ(hdr->addr + 4 + static_cast<std::int64_t>(rel), eh->addr)
        << "the header's eh_frame_ptr must resolve to .eh_frame's VA";

    // Both functions carry CFI, so both get an FDE and both get a table row.
    EXPECT_EQ(readU32LE(bytes, p + 8), 2u) << "fde_count";
    EXPECT_EQ(hdr->size, 12u + 8u * 2u)
        << "12-byte header + one 8-byte {pc, fde} row per FDE";

    // The table MUST be sorted by initial_location: the unwinder binary-
    // searches it, so an unsorted table silently returns the WRONG FDE for
    // some addresses instead of failing.
    auto const pc0 = static_cast<std::int32_t>(readU32LE(bytes, p + 12));
    auto const pc1 = static_cast<std::int32_t>(readU32LE(bytes, p + 20));
    EXPECT_LT(pc0, pc1) << "the .eh_frame_hdr search table must be sorted";
}

TEST(ElfEhFrame, NoFrameInfoMeansNoSectionsNoSegmentAndAStillValidImage) {
    // The complementary arm: the sections are scoped to actual information,
    // not to the format. A zero-size `.eh_frame` would be a record every
    // reader parses as an immediate end-of-chain sitting in front of live
    // data, and an empty PT_GNU_EH_FRAME would send the unwinder there.
    DiagnosticReporter rep;
    auto const bytes = encodeWith(/*withCfi=*/false, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const secs = readSections(bytes);
    EXPECT_EQ(findSection(secs, ".eh_frame"), nullptr);
    EXPECT_EQ(findSection(secs, ".eh_frame_hdr"), nullptr);
    for (auto const& p : readPhdrs(bytes)) {
        EXPECT_NE(p.type, kPtGnuEhFrame);
    }
    // And the name never enters `.shstrtab` either — an unconditional add
    // would grow the string table on every image and shift every offset
    // after it, breaking the byte-identity guarantee the writer maintains.
    Shdr const* shstr = findSection(secs, ".shstrtab");
    ASSERT_NE(shstr, nullptr);
    std::string blob;
    for (std::uint64_t i = 0; i < shstr->size; ++i) {
        blob.push_back(static_cast<char>(bytes[shstr->offset + i]));
    }
    EXPECT_EQ(blob.find(".eh_frame"), std::string::npos);
}

TEST(ElfEhFrame, RefusesAFunctionWhoseCfiExtentDisagreesWithItsCode) {
    // A CfiFunction describing a different length than the function's real
    // machine code produces an FDE whose address_range does not cover the
    // whole function: the unwinder finds no FDE for a PC the function really
    // occupies and stops the walk with no error. Invisible in any dump.
    auto loaded = loadShipped("elf64-x86_64-linux-dyn");
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeModule(/*withCfi=*/true);
    ASSERT_TRUE(mod.functions[0].cfi.has_value());
    mod.functions[0].cfi->codeLength += 4;      // claim 4 bytes that do not exist

    DiagnosticReporter rep;
    auto const bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    ASSERT_GT(rep.errorCount(), 0u);
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("address_range would not cover the whole function")
            != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the refusal must say what is wrong, not merely fail";
}

// ═══════════════════════════════════════════════════════════════════════
// RELOCATABLE OBJECTS — D-UNWIND-NO-EH-FRAME-IN-RELOCATABLE-OBJECTS
//
// ✔MEASURED 2026-08-13, the defect these pins close: every `eh_frame` token
// in `elf.cpp` sat inside `encodeElfExecDynamic`, so a DSS `.o` carried NO
// unwind table at all — silently. DSS `.o` files are a shipped capability
// (they link and run under gcc), and the final image cannot contain an FDE
// the object never stated.
//
// ★★ THE ASSERTION THAT MATTERS MOST HERE IS THE ADDEND.
//    In a linked image the FDE pointer is a pcrel patch the writer computes;
//    in a `.o` it is a RELOCATION, and the obvious wrong move is to reuse the
//    call-site `rel32` row — same ELF wire type (R_X86_64_PC32 = 2), same
//    formula — whose declared `addendBias` is **-4**, because a call's
//    displacement is relative to the instruction END. Baked into a DATA field
//    that bias points every FDE 4 bytes past its function. `ld` applies it
//    without complaint, `readelf` renders a well-formed object, and the
//    unwinder attributes each frame to whatever begins 4 bytes in.
//    `FdeRelocationAddendIsTheFunctionOffsetWithNoInstructionEndBias` is the
//    pin that fails if that ever happens.
//
// ★ The behavioural witness is a ROUND TRIP, run by hand on WSL and recorded
//   in the cycle report: a DSS `.o` linked by the SYSTEM gcc, executed, and
//   `_Unwind_Backtrace` walking OUT of three DSS frames — 9 frames with these
//   sections, 2 with the identical object stripped of them. It cannot run in
//   ctest on a Windows host; these pins are the writer-side structure.
// ═══════════════════════════════════════════════════════════════════════

namespace {

struct Rela {
    std::uint64_t offset = 0;
    std::uint32_t symIdx = 0;
    std::uint32_t type   = 0;
    std::int64_t  addend = 0;
};

[[nodiscard]] std::vector<Rela> readRelas(std::vector<std::uint8_t> const& b,
                                          Shdr const& s) {
    std::vector<Rela> out;
    for (std::uint64_t o = 0; o + 24 <= s.size; o += 24) {
        std::uint64_t const info = readU64LE(b, s.offset + o + 8);
        out.push_back(Rela{readU64LE(b, s.offset + o),
                           static_cast<std::uint32_t>(info >> 32),
                           static_cast<std::uint32_t>(info & 0xFFFFFFFFu),
                           static_cast<std::int64_t>(
                               readU64LE(b, s.offset + o + 16))});
    }
    return out;
}

// The SHIPPED relocatable schemas, both machines — the point of the second
// arm is that nothing in the writer knows which machine it is encoding for.
[[nodiscard]] Loaded loadRelocatable(std::string_view targetName,
                                     std::string_view formatName) {
    Loaded out;
    auto t = TargetSchema::loadShipped(targetName);
    if (!t.has_value()) {
        ADD_FAILURE() << "loadShipped(" << targetName << ") failed";
    } else {
        out.target = std::move(t).value();
    }
    auto f = ObjectFormatSchema::loadShipped(formatName);
    if (!f.has_value()) {
        ADD_FAILURE() << "loadShipped(" << formatName << ") failed";
    } else {
        out.format = std::move(f).value();
    }
    return out;
}

// `.symtab` section index, resolved by name rather than assumed.
[[nodiscard]] std::uint16_t sectionIndexOf(std::vector<Shdr> const& secs,
                                           std::string const& name) {
    for (std::uint16_t i = 0; i < secs.size(); ++i) {
        if (secs[i].name == name) return i;
    }
    return 0;
}

} // namespace

TEST(ElfEhFrameRelocatable, ObjectCarriesEhFrameAndItsRelocationTable) {
    auto loaded = loadRelocatable("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    AssembledModule mod = makeModule(/*withCfi=*/true);
    auto const bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());

    auto const secs = readSections(bytes);
    Shdr const* eh   = findSection(secs, ".eh_frame");
    Shdr const* rela = findSection(secs, ".rela.eh_frame");
    ASSERT_NE(eh, nullptr)
        << "the defect this closes IS the absent section: a DSS `.o` carried "
           "no unwind table at all, silently";
    ASSERT_NE(rela, nullptr)
        << "a `.eh_frame` with no relocation table has every FDE pointing at "
           "offset 0 of the output section — worse than no table, because the "
           "unwinder trusts it";
    EXPECT_EQ(eh->type, 1u) << "SHT_PROGBITS";
    EXPECT_EQ(eh->flags & 2u, 2u)
        << "SHF_ALLOC — these tables are MAPPED, the running process's own "
           "unwinder reads them; a debug-only section is invisible to it";
    EXPECT_EQ(eh->addr, 0u) << "ET_REL sections are unbound";
    EXPECT_EQ(rela->type, 4u) << "SHT_RELA";

    // sh_link / sh_info must name `.symtab` and `.eh_frame` respectively.
    // Aimed one section over, `ld` applies these patches to somebody else's
    // bytes — and the object still loads.
    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint16_t const idxEh  = sectionIndexOf(secs, ".eh_frame");
    std::uint16_t const idxSym = sectionIndexOf(secs, ".symtab");
    std::uint16_t const relaIdx = sectionIndexOf(secs, ".rela.eh_frame");
    EXPECT_EQ(readU32LE(bytes, shoff + relaIdx * 64 + 40), idxSym) << "sh_link";
    EXPECT_EQ(readU32LE(bytes, shoff + relaIdx * 64 + 44), idxEh)  << "sh_info";
}

TEST(ElfEhFrameRelocatable,
     FdeRelocationAddendIsTheFunctionOffsetWithNoInstructionEndBias) {
    auto loaded = loadRelocatable("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    AssembledModule mod = makeModule(/*withCfi=*/true);
    // Function 0 is 13 bytes, so function 1 starts at .text+13. The addends
    // must be exactly {0, 13} — NOT {-4, 9}, which is what reusing the
    // call-site rel32 row's -4 bias would produce.
    auto const fn1Start =
        static_cast<std::int64_t>(mod.functions[0].bytes.size());
    auto const bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);

    auto const secs = readSections(bytes);
    Shdr const* eh   = findSection(secs, ".eh_frame");
    Shdr const* rela = findSection(secs, ".rela.eh_frame");
    ASSERT_NE(eh, nullptr);
    ASSERT_NE(rela, nullptr);
    auto const relas = readRelas(bytes, *rela);
    ASSERT_EQ(relas.size(), 2u) << "one relocation per FDE, no more and no less";

    std::uint64_t const shoff = readU64LE(bytes, 40);
    std::uint16_t const idxSym = sectionIndexOf(secs, ".symtab");
    std::uint64_t const symOff = readU64LE(bytes, shoff + idxSym * 64 + 24);

    std::vector<std::int64_t> addends;
    for (auto const& r : relas) {
        addends.push_back(r.addend);
        // The relocated field itself must ship ZERO. A writer that also
        // pre-filled it would have `ld` add the relocation result on top of a
        // stale pcrel value — the classic double-apply.
        EXPECT_EQ(readU32LE(bytes, eh->offset + r.offset), 0u)
            << "the initial_location field must be zero; the addend carries "
               "the offset";
        // The target must be the `.text` STT_SECTION symbol, not a function
        // symbol: `ld --gc-sections` associates an FDE with the code it
        // describes through this section, and a GLOBAL function symbol is
        // interposable.
        EXPECT_EQ(r.symIdx, 1u);
        EXPECT_EQ(bytes[symOff + 24 + 4] & 0x0Fu, 3u)
            << "st_info type of symbol #1 must be STT_SECTION (3)";
        EXPECT_EQ(readU16LE(bytes, symOff + 24 + 6), 1u)
            << "and its st_shndx must be .text (1)";
        // The wire type is the one `gcc -c` emits.
        EXPECT_EQ(r.type, 2u) << "R_X86_64_PC32 = 2";
    }
    std::sort(addends.begin(), addends.end());
    EXPECT_EQ(addends[0], 0)
        << "first function starts at .text+0 — a -4 here is the call-site "
           "rel32 bias leaking into a DATA field";
    EXPECT_EQ(addends[1], fn1Start)
        << "second function's offset within .text, unbiased";
}

TEST(ElfEhFrameRelocatable, Aarch64ObjectEmitsPrel32NotAnInstructionFieldReloc) {
    // The agnosticism witness: the SAME writer, a different machine, and the
    // relocation type comes entirely from config. aarch64 declares NO
    // PC-relative 32-bit DATA relocation among its instruction rows —
    // CALL26 / ADR_PREL_PG_HI21 / ADD_ABS_LO12 are all instruction-field
    // patches — so a writer that reached for whatever was already there would
    // emit one of those and corrupt a data word.
    auto loaded = loadRelocatable("arm64", "elf64-aarch64-linux");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    AssembledModule mod = makeModule(/*withCfi=*/true);
    auto const bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    auto const secs = readSections(bytes);
    Shdr const* rela = findSection(secs, ".rela.eh_frame");
    ASSERT_NE(rela, nullptr);
    auto const relas = readRelas(bytes, *rela);
    ASSERT_EQ(relas.size(), 2u);
    for (auto const& r : relas) {
        EXPECT_EQ(r.type, 261u)
            << "R_AARCH64_PREL32 = 261, the type `aarch64-linux-gnu-gcc -c` "
               "emits for an FDE pointer (✔MEASURED). 283 = CALL26 and "
               "275 = ADR_PREL_PG_HI21 are instruction-field patches";
        EXPECT_EQ(r.symIdx, 1u);
    }
}

TEST(ElfEhFrameRelocatable, ObjectWithoutFrameInfoCarriesNeitherSection) {
    // The complementary arm: the sections are scoped to actual information,
    // not to the output form. A zero-size `.eh_frame` is a record every reader
    // parses as an immediate end-of-chain sitting in front of live data.
    auto loaded = loadRelocatable("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(loaded.target && loaded.format);
    DiagnosticReporter rep;
    AssembledModule mod = makeModule(/*withCfi=*/false);
    auto const bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(bytes.empty());
    auto const secs = readSections(bytes);
    EXPECT_EQ(findSection(secs, ".eh_frame"), nullptr);
    EXPECT_EQ(findSection(secs, ".rela.eh_frame"), nullptr);
}

TEST(ElfEhFrameRelocatable, TheTargetRowTheWriterSelectsIsUnbiasedAndFourBytes) {
    // The lookup is DERIVED from the target's own semantic fields rather than
    // switched on the machine, so this pins the derivation's answer on both
    // shipped targets. If a future edit gave `rel32` a zero bias — or added a
    // second unbiased pcrel32 row — the writer would silently start choosing
    // differently. `fdePointerRelocationOf` refuses on ambiguity; this asserts
    // the unambiguous answer is still the right one.
    for (auto const& tgt : {std::string_view{"x86_64"}, std::string_view{"arm64"}}) {
        auto t = TargetSchema::loadShipped(tgt);
        ASSERT_TRUE(t.has_value()) << tgt;
        std::string err;
        auto const* row = link::format::fdePointerRelocationOf(**t, err);
        ASSERT_NE(row, nullptr) << tgt << ": " << err;
        EXPECT_EQ(row->name, "pcrel32") << tgt;
        EXPECT_EQ(row->addendBias, 0) << tgt;
        EXPECT_EQ(row->widthBytes, 4) << tgt;
        EXPECT_TRUE(row->pcRelative) << tgt;
        EXPECT_FALSE(row->tls) << tgt;
    }
}

TEST(ElfEhFrameRelocatable, RefusesAFunctionWhoseCfiExtentDisagreesWithItsCode) {
    // The relocatable arm needs its own copy of the exec arm's extent check:
    // an FDE whose address_range is shorter than the function leaves a PC the
    // function really occupies with no FDE, and the unwinder stops the walk
    // with no error. The two arms are different code and a green exec pin
    // says nothing about this one.
    auto loaded = loadRelocatable("x86_64", "elf64-x86_64-linux");
    ASSERT_TRUE(loaded.target && loaded.format);
    AssembledModule mod = makeModule(/*withCfi=*/true);
    ASSERT_TRUE(mod.functions[0].cfi.has_value());
    mod.functions[0].cfi->codeLength += 4;   // claim 4 bytes that do not exist

    DiagnosticReporter rep;
    auto const bytes = elf::encode(mod, *loaded.target, *loaded.format, rep);
    EXPECT_TRUE(bytes.empty());
    ASSERT_GT(rep.errorCount(), 0u);
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("address_range would not cover the whole function")
            != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the refusal must say what is wrong, not merely fail";
}
