// arm64 TLS encoder + schema pins — TLS C2 (D-CSUBSET-THREAD-LOCAL).
//
// Pins the AArch64 local-exec access sequence, word-exact, and the C2
// schema opt-ins. CRUCIALLY, the encoder needed ZERO new machinery:
//   * `tlsbase` — `MRS Xd, TPIDR_EL0` is ONE fixed word
//     (0xD53BD040 | Rd) riding the ordinary fixedWord + resultSlot
//     `rd` template (the `ret`/`mov` shape). The row is 0-OPERAND —
//     deliberately different from x86's 1-op `[memoffset]` row: the
//     system-register read consumes no memory operand and no prefix
//     byte, and the lowering's shape probe emits whichever shape the
//     target declares (test_mir_to_lir.cpp pins the probe itself).
//   * the 2-op `lea Xd, [tp + tpoff(sym)]` — the ADD/ADD hi12/lo12
//     word-pair (`ADD Xd, Xn, #hi12, LSL #12` 0x91400000 then
//     `ADD Xd, Xd, #lo12` 0x91000000) rides the SAME generic
//     `sym.patch` + extraResultSlots machinery the ADRP+ADD [symbol]
//     lea already uses — config-only: TWO relocations (tls-tprel-hi12
//     at word offset 0, tls-tprel-lo12 at word offset 4) against the
//     same symbol, imm12 fields ZERO in the emitted words (the linker
//     ORs the patch in; a dirty field fails loud at apply time —
//     test_aarch64_reloc_formulas.cpp pins the kernel arms).
// Plus the C2 schema pins: the arm64 `tls` identity block (Variant I,
// tcbHeaderBytes 16) + the two tls-flagged tprel relocation rows + the
// aarch64-ELF format's tlsAccess block and TLSLE native reloc ids.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "link/object_format_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

// Build a one-function LIR whose single block holds the instructions
// `emit` adds plus a trailing `ret` (the block must be terminated).
template <typename Emit>
[[nodiscard]] Lir buildSingleFnLirWithRet(TargetSchema const& schema,
                                          Emit emit) {
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    auto const retOp = schema.opcodeByMnemonic("ret");
    if (!retOp.has_value()) {
        ADD_FAILURE() << "test fixture: arm64 schema missing 'ret'";
        std::abort();
    }
    (void)b.addReturn(*retOp, {});
    return std::move(b).finish();
}

struct AssembledFn {
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation>   relocs;
};

[[nodiscard]] AssembledFn
assembleFirstFn(Lir const& lir, TargetSchema const& schema,
                DiagnosticReporter& rep) {
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, schema, lirToMir, rep);
    AssembledFn out;
    EXPECT_EQ(result.functions.size(), 1u);
    if (result.functions.empty()) return out;
    out.bytes  = result.functions[0].bytes;
    out.relocs = result.functions[0].relocations;
    return out;
}

[[nodiscard]] LirReg physGpr(TargetSchema const& schema,
                             std::string_view name) {
    auto const ord = schema.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << "register not found: " << name;
    return LirReg{static_cast<std::uint32_t>(ord.value_or(0)),
                  /*isPhysical=*/1,
                  /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
}

[[nodiscard]] std::uint32_t rdWord(std::vector<std::uint8_t> const& b,
                                   std::size_t off) {
    return  static_cast<std::uint32_t>(b[off])
         | (static_cast<std::uint32_t>(b[off + 1]) << 8)
         | (static_cast<std::uint32_t>(b[off + 2]) << 16)
         | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

// Emit ONE bare `tlsbase <dst>` (the arm64 0-operand MRS shape) and
// assemble.
[[nodiscard]] AssembledFn
assembleTlsBase(TargetSchema const& schema, std::string_view dstName,
                DiagnosticReporter& rep) {
    auto const tlsbaseOp = schema.opcodeByMnemonic("tlsbase");
    EXPECT_TRUE(tlsbaseOp.has_value());
    LirReg const dst = physGpr(schema, dstName);
    Lir lir = buildSingleFnLirWithRet(schema, [&](LirBuilder& b) {
        (void)b.addInst(*tlsbaseOp, dst, {});
    });
    return assembleFirstFn(lir, schema, rep);
}

} // namespace

// ── `tlsbase` (MRS TPIDR_EL0) word pins ─────────────────────────────

TEST(Arm64Tls, TlsBaseX9EmitsMrsTpidrEl0WithRd9) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const fn = assembleTlsBase(**schema, "x9", rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // MRS X9, TPIDR_EL0 = 0xD53BD040 | 9 = 0xD53BD049; then ret.
    ASSERT_EQ(fn.bytes.size(), 8u);
    EXPECT_EQ(rdWord(fn.bytes, 0), 0xD53BD049u);
    EXPECT_EQ(rdWord(fn.bytes, 4), 0xD65F03C0u);
    // The tp read is self-contained — no relocation, ever.
    EXPECT_TRUE(fn.relocs.empty());
}

TEST(Arm64Tls, TlsBaseRdSlotFollowsDestRegister) {
    // The destination rides the ordinary 5-bit Rd slot — x12 flips
    // ONLY bits 0..4 of the fixed word.
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const fn = assembleTlsBase(**schema, "x12", rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(fn.bytes.size(), 4u);
    EXPECT_EQ(rdWord(fn.bytes, 0), 0xD53BD04Cu);
}

// ── the TLS `lea` (ADD/ADD hi12/lo12 macro) pins ─────────────────────

TEST(Arm64Tls, TlsLeaEmitsAddAddPairWithBothRelocsAtWordOffsets) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const x0 = physGpr(**schema, "x0");
    LirReg const x9 = physGpr(**schema, "x9");
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(x9),
            LirOperand::makeSymbolRef(99),
        };
        (void)b.addInst(*leaOp, x0, ops);
    });
    DiagnosticReporter rep;
    auto const fn = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // word0 = ADD X0, X9, #0, LSL #12  = 0x91400000 | Rn(9)<<5 | Rd(0)
    //       = 0x91400120  — imm12 ZERO (the linker ORs hi12 in).
    // word1 = ADD X0, X0, #0           = 0x91000000 | Rn(0)<<5 | Rd(0)
    //       = 0x91000000  — imm12 ZERO (lo12); Rd threads via
    //       extraResultSlots into word1.Rd AND word1.Rn.
    // then ret.
    ASSERT_EQ(fn.bytes.size(), 12u);
    EXPECT_EQ(rdWord(fn.bytes, 0), 0x91400120u);
    EXPECT_EQ(rdWord(fn.bytes, 4), 0x91000000u);
    EXPECT_EQ(rdWord(fn.bytes, 8), 0xD65F03C0u);
    // Belt: BOTH imm12 fields [21:10] are zero in the emitted words —
    // the reloc kernel's rejectIfBitfieldDirty depends on it.
    EXPECT_EQ(rdWord(fn.bytes, 0) & (0xFFFu << 10), 0u);
    EXPECT_EQ(rdWord(fn.bytes, 4) & (0xFFFu << 10), 0u);
    // TWO relocations against the SAME symbol at the exact word
    // offsets: tls-tprel-hi12 at 0, tls-tprel-lo12 at 4.
    ASSERT_EQ(fn.relocs.size(), 2u);
    auto const* hi = (*schema)->relocationByName("tls-tprel-hi12");
    auto const* lo = (*schema)->relocationByName("tls-tprel-lo12");
    ASSERT_NE(hi, nullptr);
    ASSERT_NE(lo, nullptr);
    EXPECT_EQ(fn.relocs[0].offset, 0u);
    EXPECT_EQ(fn.relocs[0].kind, hi->kind);
    EXPECT_EQ(fn.relocs[0].target, SymbolId{99});
    EXPECT_EQ(fn.relocs[0].addend, 0);
    EXPECT_EQ(fn.relocs[1].offset, 4u);
    EXPECT_EQ(fn.relocs[1].kind, lo->kind);
    EXPECT_EQ(fn.relocs[1].target, SymbolId{99});
    EXPECT_EQ(fn.relocs[1].addend, 0);
}

TEST(Arm64Tls, TlsLeaDistinctBaseAndDestWireRnAndThreadRd) {
    // dst=x2, base=x11: word0.Rn = 11 (the tp register), word0.Rd = 2;
    // word1.Rn = word1.Rd = 2 (the DEST threads through word1 — the
    // scratch-free extraResultSlots contract).
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const x2  = physGpr(**schema, "x2");
    LirReg const x11 = physGpr(**schema, "x11");
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(x11),
            LirOperand::makeSymbolRef(7),
        };
        (void)b.addInst(*leaOp, x2, ops);
    });
    DiagnosticReporter rep;
    auto const fn = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(fn.bytes.size(), 8u);
    // 0x91400000 | 11<<5 | 2 = 0x91400162; 0x91000000 | 2<<5 | 2 = 0x91000042.
    EXPECT_EQ(rdWord(fn.bytes, 0), 0x91400162u);
    EXPECT_EQ(rdWord(fn.bytes, 4), 0x91000042u);
}

// ── shipped-schema pins (the C2 opt-ins) ─────────────────────────────

TEST(Arm64Tls, ShippedArm64DeclaresTlsIdentityVariant1AndTprelRows) {
    // TLS C2 landed: arm64 declares its Variant-I identity + the
    // tprel reloc pair + the tlsbase opcode. (The C1-era negative pin
    // — no identity / no tlsbase / no rows — lived in
    // test_asm_x86_tls.cpp and flipped WITH this cycle.)
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const& tls = (*schema)->tlsIdentity();
    ASSERT_TRUE(tls.has_value())
        << "arm64 must declare its static-TLS layout convention at C2";
    EXPECT_EQ(tls->variant, TlsVariant::Variant1);
    EXPECT_EQ(tls->tcbHeaderBytes, 16u);
    EXPECT_TRUE((*schema)->opcodeByMnemonic("tlsbase").has_value());

    auto const* hi = (*schema)->relocationByName("tls-tprel-hi12");
    ASSERT_NE(hi, nullptr);
    EXPECT_TRUE(hi->tls);
    EXPECT_EQ(hi->formulaKind, RelocFormulaKind::Aarch64TprelAddHi12);
    EXPECT_EQ(hi->widthBytes, 4u);

    auto const* lo = (*schema)->relocationByName("tls-tprel-lo12");
    ASSERT_NE(lo, nullptr);
    EXPECT_TRUE(lo->tls);
    // The lo12 arm REUSES the Aarch64AddAbsLo12 FORMULA — but as a
    // DISTINCT tls-flagged KIND row (the CRIT-1 cross-check keys the
    // flag; the plain add_abs_lo12_nc kind must stay non-tls).
    EXPECT_EQ(lo->formulaKind, RelocFormulaKind::Aarch64AddAbsLo12);
    EXPECT_EQ(lo->widthBytes, 4u);
    EXPECT_NE(lo->kind, (*schema)->relocationByName("add_abs_lo12_nc")->kind);
    EXPECT_FALSE((*schema)->relocationByName("add_abs_lo12_nc")->tls);
}

TEST(Arm64Tls, ShippedAarch64ElfFormatDeclaresTlsAccessAndTlsleNativeIds) {
    auto fmt = ObjectFormatSchema::loadShipped("elf64-aarch64-linux-exec");
    ASSERT_TRUE(fmt.has_value());
    // The tlsAccess opt-in: local-exec; the two x86 template values
    // are declared 0 — the MRS shape never consumes them (they are
    // read only by encoders whose templates request them).
    auto const ta = (*fmt)->tlsAccess();
    ASSERT_TRUE(ta.has_value())
        << "aarch64-ELF must declare tlsAccess at TLS C2";
    EXPECT_EQ(ta->model, TlsAccessModel::LocalExec);
    EXPECT_EQ(ta->segmentPrefixByte, 0u);
    EXPECT_EQ(ta->baseDisplacement, 0u);

    // The format-side reloc rows join the target rows by KIND; the
    // native ids are the AArch64 psABI TLSLE pair. The lo12 id is the
    // *_NC variant (551) — DSS's lo12 formula is the psABI's
    // 'no overflow check' semantic (magnitude-free low-12 extraction;
    // the PAIRED hi12 arm range-checks the full value), exactly the
    // pairing gcc/binutils emit for -ftls-model=local-exec.
    auto tgtR = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(tgtR.has_value());
    auto const& tgt = **tgtR;
    auto const* hi = tgt.relocationByName("tls-tprel-hi12");
    auto const* lo = tgt.relocationByName("tls-tprel-lo12");
    ASSERT_NE(hi, nullptr);
    ASSERT_NE(lo, nullptr);
    auto const* fmtHi = (*fmt)->relocationByKind(hi->kind);
    ASSERT_NE(fmtHi, nullptr)
        << "the pre-walker cross-reference unifier needs a format row "
           "for every kind the assembler can emit";
    EXPECT_EQ(fmtHi->name, "R_AARCH64_TLSLE_ADD_TPREL_HI12");
    EXPECT_EQ(fmtHi->nativeId, 549u);
    auto const* fmtLo = (*fmt)->relocationByKind(lo->kind);
    ASSERT_NE(fmtLo, nullptr);
    EXPECT_EQ(fmtLo->name, "R_AARCH64_TLSLE_ADD_TPREL_LO12_NC");
    EXPECT_EQ(fmtLo->nativeId, 551u);

    // The section opt-ins: tdata (PROGBITS, WRITE|ALLOC|TLS, floor 1)
    // + tbss (NOBITS, same flags) — byte-for-byte the C1 x86 rows;
    // the shared elf.cpp walker is machine-neutral.
    auto const* tdata = (*fmt)->sectionByKind(SectionKind::ThreadData);
    ASSERT_NE(tdata, nullptr);
    EXPECT_EQ(tdata->name, ".tdata");
    EXPECT_EQ(tdata->type, 1u);
    EXPECT_EQ(tdata->flags, 1027u);
    EXPECT_EQ(tdata->addrAlign, 1u);
    auto const* tbss = (*fmt)->sectionByKind(SectionKind::ThreadBss);
    ASSERT_NE(tbss, nullptr);
    EXPECT_EQ(tbss->name, ".tbss");
    EXPECT_EQ(tbss->type, 8u);
    EXPECT_EQ(tbss->flags, 1027u);
    EXPECT_EQ(tbss->addrAlign, 1u);
}
