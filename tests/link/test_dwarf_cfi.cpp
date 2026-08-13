// DWARF `.eh_frame` encoder — plan 15 DB4.
//
// Closes the ENCODER half of D-UNWIND-NO-EH-FRAME-ANY-LANGUAGE-ON-ELF-OR-MACHO.
// ✔MEASURED 2026-08-13: `readelf -S` on a DSS elf64 build of ordinary C finds
// ZERO `.eh_frame` sections; gcc on the same source emits one.
//
// ★ THE WITNESS IS AN OBJECT-LEVEL ONE, NOT "the bytes matched a golden".
//   `EhFrameIsReadableByAnExternalDwarfReader` writes the encoder's output to
//   `dss_eh_frame_witness.bin` in the test's working directory so an external
//   oracle (`objcopy --add-section .eh_frame=… ` then
//   `readelf --debug-dump=frames`, or `llvm-dwarfdump --eh-frame`) can decode
//   it — the plan-15 DB11 "oracle, not golden text" discipline. The in-test
//   assertions below pin the bytes so a regression is caught in CI without the
//   oracle; the file exists so a human can confirm the bytes MEAN what the
//   assertions claim.

#include "core/types/cfi.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "link/format/dwarf_cfi.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <vector>

using namespace dss;
using namespace dss::link::format;

namespace {

// The System V AMD64 psABI DWARF register-number mapping (Fig. 3.36),
// expressed against DSS PHYSICAL ORDINALS.
//
// ★ THIS TABLE IS THE ENTIRE POINT OF THE REFUSAL BELOW. DSS ordinals run
//   rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi (matching the x86 hardware encoding);
//   the DWARF numbering runs rax,rdx,rcx,rbx,rsi,rdi,rbp,rsp. Four registers
//   swap places. Handing a debugger the hardware number for `%rbp` names
//   `%rsp` instead, and the resulting backtrace is wrong WITHOUT any
//   complaint from the reader.
// The mapping is a view over the target's OWN register rows, so the fixture
// builds rows — the same `TargetRegisterInfo` shape `x86_64.target.json`
// loads into. A fixture that invented a parallel number array would be
// testing a shape the shipped path never produces.
struct MappingHolder {
    std::vector<TargetRegisterInfo> registers;
    DwarfRegisterMapping            mapping{};

    // Re-seat the span after any mutation: `registers` owns the storage.
    void rebind() {
        mapping.registers = registers;
        mapping.returnAddressColumn = 16;   // the synthetic RA column
        mapping.targetName = "x86_64";
    }
};

MappingHolder sysvMapping() {
    MappingHolder h;
    // ordinals 0..15 = rax rcx rdx rbx rsp rbp rsi rdi r8..r15, then xmm0..15.
    // ✔MEASURED 2026-08-13 (binutils 2.42): `as` + `readelf --debug-dump=frames`
    // on a `.cfi_offset` probe prints `r7 (rsp)`, `r6 (rbp)`, `r4 (rsi)`,
    // `r5 (rdi)` — i.e. GAS's own translation, not a table typed from memory.
    static constexpr char const* kNames[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    static constexpr std::uint16_t kDwarf[] = {
        0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
    for (std::size_t i = 0; i < 16; ++i) {
        TargetRegisterInfo r;
        r.name        = kNames[i];
        r.regClass    = TargetRegClass::GPR;
        r.widthBytes  = 8;
        r.hwEncoding  = static_cast<std::uint16_t>(i);
        r.dwarfNumber = kDwarf[i];
        h.registers.push_back(std::move(r));
    }
    for (std::uint16_t x = 0; x < 16; ++x) {
        TargetRegisterInfo r;
        r.name        = "xmm" + std::to_string(x);
        r.regClass    = TargetRegClass::FPR;
        r.widthBytes  = 16;
        r.hwEncoding  = x;
        r.dwarfNumber = static_cast<std::uint16_t>(17 + x);
        h.registers.push_back(std::move(r));
    }
    h.rebind();
    return h;
}

CfiInitialState sysvEntry() {
    CfiInitialState s;
    s.cfaRegister = 4;      // rsp (DSS ordinal) -> DWARF 7
    s.cfaOffset   = 8;
    s.returnAddressAtCfaOffset = -8;
    return s;
}

// The shape a real DSS x86_64 leaf produces: `sub rsp,0x20` (7 bytes) then two
// 8-byte callee-save stores; teardown mirrored.
CfiFunction dssLeaf() {
    CfiFunction f;
    f.codeLength    = 0x3F;
    f.initial       = sysvEntry();
    f.prologueEndPc = 23;
    f.ops = {
        CfiOp{7,  CfiOpKind::DefCfaOffset,      CfiRegRef{},             CfiRegRef{},  40},
        CfiOp{15, CfiOpKind::RegAtCfaOffset,    CfiRegRef::physical(14), CfiRegRef{}, -40},
        CfiOp{23, CfiOpKind::RegAtCfaOffset,    CfiRegRef::physical(15), CfiRegRef{}, -24},
        CfiOp{47, CfiOpKind::RegRestoreInitial, CfiRegRef::physical(14), CfiRegRef{},   0},
        CfiOp{55, CfiOpKind::RegRestoreInitial, CfiRegRef::physical(15), CfiRegRef{},   0},
        CfiOp{62, CfiOpKind::DefCfaOffset,      CfiRegRef{},             CfiRegRef{},   8},
    };
    return f;
}

} // namespace

TEST(DwarfCfi, RefusesWhenTheTargetDeclaresNoDwarfRegisterNumbering) {
    // ★ THE LOAD-BEARING REFUSAL. Without the psABI table there is no honest
    //   way to name a register in `.eh_frame`. Falling back to the hardware
    //   encoding would emit a table a debugger follows into the WRONG frame —
    //   strictly worse than today's absent table, which at least fails
    //   visibly. RED-on-disable: make `declared()` return true for an empty
    //   mapping and this test's expectation of an error goes green-with-
    //   garbage instead.
    std::vector<std::optional<CfiFunction>> fns{dssLeaf()};
    DwarfRegisterMapping empty{};
    empty.targetName = "x86_64";
    DiagnosticReporter rep;
    auto const sec = buildEhFrame(fns, empty, 8, rep);
    EXPECT_FALSE(sec.has_value());
    ASSERT_GT(rep.errorCount(), 0u);
    bool named = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("declares no DWARF register numbering")
                != std::string::npos
            && d.actual.find("*.target.json") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named)
        << "the refusal must name the missing per-target table AND where it "
           "belongs, not merely report failure";
}

TEST(DwarfCfi, RefusesOneUnmappedRegisterRatherThanEmittingTheRestOfTheTable) {
    // ★ A `.eh_frame` that silently covers 90% of an image is worse than
    //   none: the unwinder trusts it, finds no rule for the missing register,
    //   and reports a plausible wrong frame. So ONE unmapped ordinal fails
    //   the WHOLE section.
    auto h = sysvMapping();
    h.registers[15].dwarfNumber.reset();     // r15 loses its DWARF number
    h.rebind();

    std::vector<std::optional<CfiFunction>> fns{dssLeaf()};
    DiagnosticReporter rep;
    EXPECT_FALSE(buildEhFrame(fns, h.mapping, 8, rep).has_value());
    bool named = false;
    for (auto const& d : rep.all()) {
        // Both the ordinal AND the register's own name: an ordinal alone
        // makes the reader go and count rows in the target JSON.
        if (d.actual.find("ordinal 15") != std::string::npos
            && d.actual.find("('r15')") != std::string::npos) {
            named = true;
        }
    }
    EXPECT_TRUE(named) << "the refusal must name the register it could not map";
}

TEST(DwarfCfi, RefusesAModuleWhoseFunctionsDisagreeOnTheirEntryState) {
    // `.eh_frame` hoists the entry row into ONE shared CIE. Two functions that
    // disagree cannot both be described by it, and emitting one anyway
    // misdescribes every function that does not match.
    auto h = sysvMapping();
    CfiFunction a = dssLeaf();
    CfiFunction b = dssLeaf();
    b.initial.cfaOffset = 16;                // a different convention
    std::vector<std::optional<CfiFunction>> fns{a, b};
    DiagnosticReporter rep;
    EXPECT_FALSE(buildEhFrame(fns, h.mapping, 8, rep).has_value());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DwarfCfi, CieEncodesTheEntryStateWithDwarfNumbersNotHardwareEncodings) {
    auto h = sysvMapping();
    std::vector<std::optional<CfiFunction>> fns{dssLeaf()};
    DiagnosticReporter rep;
    auto const sec = buildEhFrame(fns, h.mapping, 8, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_TRUE(sec.has_value());
    auto const& b = sec->bytes;
    ASSERT_GE(b.size(), 24u);

    // CIE: length(4) | CIE_id=0(4) | version=1 | "zR\0" | CAF | DAF | RA |
    //      auglen=1 | ptr-enc | instructions | nop pad.
    EXPECT_EQ(b[0], 0x14u) << "CIE length (whole record 8-aligned)";
    EXPECT_EQ(b[4], 0x00u); EXPECT_EQ(b[5], 0x00u);
    EXPECT_EQ(b[6], 0x00u); EXPECT_EQ(b[7], 0x00u);
    EXPECT_EQ(b[8],  1u)    << "version";
    EXPECT_EQ(b[9],  'z');  EXPECT_EQ(b[10], 'R'); EXPECT_EQ(b[11], 0);
    EXPECT_EQ(b[12], 0x01u) << "code alignment factor = 1";
    EXPECT_EQ(b[13], 0x7Fu) << "data alignment factor = -1 (SLEB) — the "
                               "identity, so no offset can be lost to a "
                               "truncating divide";
    EXPECT_EQ(b[14], 16u)   << "return-address column (x86_64 SysV = 16, a "
                               "SYNTHETIC column that is not a register)";
    EXPECT_EQ(b[15], 0x01u) << "augmentation data length";
    EXPECT_EQ(b[16], 0x1Bu) << "FDE pointer encoding = pcrel|sdata4";
    // ★ DW_CFA_def_cfa(0x0c), register 7, offset 8. SEVEN is `%rsp`'s DWARF
    //   number; its DSS ordinal is 4 and its HARDWARE encoding is also 4.
    //   A writer that reached for either would emit 4 = `%rsi` in DWARF.
    EXPECT_EQ(b[17], 0x0Cu) << "DW_CFA_def_cfa";
    EXPECT_EQ(b[18], 7u)    << "rsp is DWARF 7, NOT its ordinal/hwEncoding 4";
    EXPECT_EQ(b[19], 8u)    << "CFA = rsp + 8 (what the CALL pushed)";
    // DW_CFA_offset | 16 (the RA column) = 0x90, factored offset 8 (-8 / -1).
    EXPECT_EQ(b[20], 0x90u) << "DW_CFA_offset for the RA column";
    EXPECT_EQ(b[21], 8u)    << "return address at CFA-8";
    EXPECT_EQ(b[22], 0x00u) << "DW_CFA_nop pad";
    EXPECT_EQ(b[23], 0x00u);
}

TEST(DwarfCfi, FdeCarriesTheMeasuredPcOfEveryRuleAndTheEpilogueTeardown) {
    auto h = sysvMapping();
    std::vector<std::optional<CfiFunction>> fns{dssLeaf()};
    DiagnosticReporter rep;
    auto const sec = buildEhFrame(fns, h.mapping, 8, rep);
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    ASSERT_TRUE(sec.has_value());
    ASSERT_EQ(sec->patches.size(), 1u);
    ASSERT_EQ(sec->fdeOffsets.size(), 1u);
    auto const& b = sec->bytes;
    std::size_t const f = sec->fdeOffsets[0];
    ASSERT_EQ(f, 24u) << "the FDE follows the 24-byte CIE";

    // CIE pointer = distance BACK from this field to the CIE (this is
    // `.eh_frame`; `.debug_frame` would use an absolute section offset, and
    // encoding one as the other yields a record readers mis-associate).
    // ✔ Cross-checked against real gcc output: its first FDE sits at 0x18 with
    // this field at 0x1c holding 0x1c — the same "distance back from the FIELD"
    // rule, not "distance back from the record".
    EXPECT_EQ(b[f + 4], 28u) << "CIE pointer = this field's offset minus the "
                                "CIE's offset (28 - 0)";
    EXPECT_EQ(sec->patches[0].byteOffset, f + 8)
        << "initial_location is the pcrel field that layout patches";
    EXPECT_EQ(b[f + 12], 0x3Fu) << "address_range = the function's extent";
    EXPECT_EQ(b[f + 16], 0x00u) << "augmentation data length";

    // Instructions. Each rule advances to its MEASURED pc.
    std::size_t i = f + 17;
    EXPECT_EQ(b[i++], 0x40u | 7u) << "advance_loc 7 -> end of `sub rsp,0x20`";
    EXPECT_EQ(b[i++], 0x0Eu)      << "DW_CFA_def_cfa_offset";
    EXPECT_EQ(b[i++], 40u)        << "CFA = rsp + 40";
    EXPECT_EQ(b[i++], 0x40u | 8u) << "advance_loc 8 -> end of the r14 store";
    EXPECT_EQ(b[i++], 0x80u | 14u)<< "DW_CFA_offset r14 (DWARF 14 == ordinal)";
    EXPECT_EQ(b[i++], 40u)        << "r14 at CFA-40 (factored by -1)";
    EXPECT_EQ(b[i++], 0x40u | 8u) << "advance_loc 8 -> end of the r15 store";
    EXPECT_EQ(b[i++], 0x80u | 15u)<< "DW_CFA_offset r15";
    EXPECT_EQ(b[i++], 24u)        << "r15 at CFA-24";
    // ★ The epilogue. A frame SHAPE could not express any of this.
    EXPECT_EQ(b[i++], 0x40u | 24u)<< "advance_loc 24 -> the r14 reload";
    EXPECT_EQ(b[i++], 0xC0u | 14u)<< "DW_CFA_restore r14";
    EXPECT_EQ(b[i++], 0x40u | 8u);
    EXPECT_EQ(b[i++], 0xC0u | 15u)<< "DW_CFA_restore r15";
    EXPECT_EQ(b[i++], 0x40u | 7u) << "advance_loc 7 -> after `add rsp,0x20`";
    EXPECT_EQ(b[i++], 0x0Eu)      << "DW_CFA_def_cfa_offset";
    EXPECT_EQ(b[i++], 8u)
        << "the frame is GONE here — without this rule an unwinder sampling "
           "the last instructions reads the return address 32 bytes off";

    // The section ends with the zero-length terminator record.
    ASSERT_GE(b.size(), 4u);
    EXPECT_EQ(b[b.size() - 4], 0u); EXPECT_EQ(b[b.size() - 3], 0u);
    EXPECT_EQ(b[b.size() - 2], 0u); EXPECT_EQ(b[b.size() - 1], 0u);
}

TEST(DwarfCfi, PcRelPatchAndSearchHeaderResolveAgainstRealAddresses) {
    auto h = sysvMapping();
    std::vector<std::optional<CfiFunction>> fns{dssLeaf(), dssLeaf()};
    DiagnosticReporter rep;
    auto sec = buildEhFrame(fns, h.mapping, 8, rep);
    ASSERT_TRUE(sec.has_value());
    ASSERT_EQ(sec->patches.size(), 2u);

    constexpr std::uint64_t kEhFrameVa = 0x401000;
    // Function 1 deliberately sits BELOW function 0 so the header's sort is
    // observable: an unsorted search table silently returns the wrong FDE for
    // some addresses rather than failing.
    auto const funcVa = [](std::size_t i) -> std::uint64_t {
        return i == 0 ? 0x402000ull : 0x400100ull;
    };
    applyEhFramePcRel(*sec, kEhFrameVa, funcVa);
    auto const rel0 = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(sec->bytes[sec->patches[0].byteOffset])
        | (static_cast<std::uint32_t>(sec->bytes[sec->patches[0].byteOffset + 1]) << 8)
        | (static_cast<std::uint32_t>(sec->bytes[sec->patches[0].byteOffset + 2]) << 16)
        | (static_cast<std::uint32_t>(sec->bytes[sec->patches[0].byteOffset + 3]) << 24));
    EXPECT_EQ(static_cast<std::int64_t>(kEhFrameVa + sec->patches[0].byteOffset)
                  + rel0,
              0x402000)
        << "initial_location is PC-relative to its own field";

    constexpr std::uint64_t kHdrVa = 0x400000;
    auto const hdr = buildEhFrameHdr(*sec, kHdrVa, kEhFrameVa, funcVa);
    ASSERT_EQ(hdr.size(), 12u + 2u * 8u);
    EXPECT_EQ(hdr[0], 1u)    << "version";
    EXPECT_EQ(hdr[1], 0x1Bu) << "eh_frame_ptr encoding = pcrel|sdata4";
    EXPECT_EQ(hdr[2], 0x03u) << "fde_count encoding = udata4";
    EXPECT_EQ(hdr[3], 0x3Bu) << "table encoding = datarel|sdata4";
    auto rd32 = [&](std::size_t o) {
        return static_cast<std::int32_t>(
            static_cast<std::uint32_t>(hdr[o])
            | (static_cast<std::uint32_t>(hdr[o + 1]) << 8)
            | (static_cast<std::uint32_t>(hdr[o + 2]) << 16)
            | (static_cast<std::uint32_t>(hdr[o + 3]) << 24));
    };
    EXPECT_EQ(rd32(8), 2) << "fde_count";
    EXPECT_LT(rd32(12), rd32(20))
        << "the search table MUST be sorted by initial_location — the "
           "unwinder binary-searches it";
    EXPECT_EQ(static_cast<std::int64_t>(kHdrVa) + rd32(12), 0x400100)
        << "the lower-addressed function sorts first";
}

TEST(DwarfCfi, EhFrameIsReadableByAnExternalDwarfReader) {
    // The plan-15 DB11 ORACLE witness. Writes the encoder's `.eh_frame` bytes
    // so an external reader can decode them:
    //
    //   objcopy --add-section .eh_frame=dss_eh_frame_witness.bin \
    //           --set-section-flags .eh_frame=alloc,readonly any.o out.o
    //   readelf --debug-dump=frames out.o        # or llvm-dwarfdump --eh-frame
    //
    // ✔MEASURED 2026-08-13 on this exact byte stream: `readelf` decodes the
    // CIE (`DW_CFA_def_cfa: r7 (rsp) ofs 8`, `DW_CFA_offset: r16 (rip) at
    // cfa-8`) and the FDE's per-PC rules including the epilogue teardown.
    // The assertions above pin the same facts byte-wise so CI catches a
    // regression without the external tool present.
    auto h = sysvMapping();
    std::vector<std::optional<CfiFunction>> fns{dssLeaf()};
    DiagnosticReporter rep;
    auto const sec = buildEhFrame(fns, h.mapping, 8, rep);
    ASSERT_TRUE(sec.has_value());
    if (std::FILE* f = std::fopen("dss_eh_frame_witness.bin", "wb")) {
        std::fwrite(sec->bytes.data(), 1, sec->bytes.size(), f);
        std::fclose(f);
    }
    // Every record's length field must describe a record that lands exactly on
    // the next one — the structural property a reader relies on to walk the
    // chain, and the one a hand-rolled encoder gets wrong first.
    std::size_t off = 0;
    std::size_t records = 0;
    while (off + 4 <= sec->bytes.size()) {
        std::uint32_t const len =
            static_cast<std::uint32_t>(sec->bytes[off])
            | (static_cast<std::uint32_t>(sec->bytes[off + 1]) << 8)
            | (static_cast<std::uint32_t>(sec->bytes[off + 2]) << 16)
            | (static_cast<std::uint32_t>(sec->bytes[off + 3]) << 24);
        if (len == 0) { off += 4; break; }        // terminator
        ASSERT_EQ((4u + len) % 8u, 0u)
            << "record at " << off << " is not 8-aligned in total length";
        off += 4 + len;
        ++records;
        ASSERT_LE(off, sec->bytes.size()) << "record overruns the section";
    }
    EXPECT_EQ(records, 2u) << "one CIE + one FDE";
    EXPECT_EQ(off, sec->bytes.size()) << "the chain consumes the section exactly";
}
