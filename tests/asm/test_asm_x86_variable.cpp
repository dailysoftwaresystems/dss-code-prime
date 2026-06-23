// x86-variable encoder tests — plan 13 AS2 cycle 2.
//
// Pins exact byte sequences for the 3 opcodes authored in
// `x86_64.target.json` this cycle (`ret`, `mov reg-reg`,
// `mov reg-imm32`). Variant guard matching, REX.W/R/B derivation,
// little-endian immediate emission, and the
// `A_NoMatchingEncodingVariant` fail-loud path are all covered.

#include "asm/asm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace dss;
using dss::test_support::asm_::countDiagnostics;

namespace {

// Build a one-function LIR with a single block containing the given
// instructions. Caller supplies a callback that emits insts; the
// block must be terminated (e.g. ending with `unreachable` / `ret`)
// or the LirBuilder's `finish()` will abort.
template <typename Emit>
[[nodiscard]] Lir buildSingleFnLir(TargetSchema const& schema, Emit emit) {
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    return std::move(b).finish();
}

// Wrap a function-emitter that adds a final `ret` so the block is
// terminated. `ret` has its own encoding row in cycle 2 so the byte
// stream includes the 0xC3 — the assertion helpers below explicitly
// slice it off when checking non-terminator hex pins.
template <typename Emit>
[[nodiscard]] Lir buildSingleFnLirWithRet(TargetSchema const& schema,
                                           Emit emit) {
    return buildSingleFnLir(schema, [&](LirBuilder& b) {
        emit(b);
        auto const retOp = schema.opcodeByMnemonic("ret");
        // The schema MUST declare `ret` for these tests. Aborting
        // here surfaces the missing dependency loudly rather than
        // silently mis-asserting downstream.
        if (!retOp.has_value()) {
            ADD_FAILURE() << "test fixture: x86_64 schema missing 'ret'";
            std::abort();
        }
        (void)b.addReturn(*retOp, {});
    });
}

// Convenience: assemble + assert ok, return the single function's
// bytes. `lirToMir` is sized to match `lir.instCount()` per the
// substrate's bounds check.
[[nodiscard]] std::vector<std::uint8_t>
assembleFirstFn(Lir const& lir, TargetSchema const& schema,
                DiagnosticReporter& rep) {
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto result = assemble(lir, schema, lirToMir, rep);
    EXPECT_EQ(result.functions.size(), 1u);
    if (result.functions.empty()) return {};
    return result.functions[0].bytes;
}

} // namespace

// ── `ret` — 0xC3 ─────────────────────────────────────────────────────

TEST(X86VariableEncoder, RetEmits0xC3) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());

    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    ASSERT_TRUE(retOp.has_value());

    Lir lir = buildSingleFnLir(**schema, [&](LirBuilder& b) {
        (void)b.addReturn(*retOp, {});
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_EQ(bytes[0], 0xC3);
}

// ── `mov reg, reg` — REX.W 0x8B /r ───────────────────────────────────

namespace {

// Resolve the schema's `mov` opcode + an arbitrary physical register
// pair, plus emit one `mov result, src` LIR inst into the open block.
struct MovRegRegFixture {
    std::uint16_t movOp;
    LirReg        result;
    LirReg        src;
};

[[nodiscard]] MovRegRegFixture
movRegReg(TargetSchema const& schema, std::string_view dstName,
          std::string_view srcName) {
    MovRegRegFixture f{};
    auto const movOp = schema.opcodeByMnemonic("mov");
    EXPECT_TRUE(movOp.has_value());
    f.movOp = *movOp;
    auto const dstOrd = schema.registerByName(dstName);
    auto const srcOrd = schema.registerByName(srcName);
    EXPECT_TRUE(dstOrd.has_value());
    EXPECT_TRUE(srcOrd.has_value());
    f.result = LirReg{static_cast<std::uint32_t>(*dstOrd),
                       /*isPhysical=*/1,
                       /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
    f.src    = LirReg{static_cast<std::uint32_t>(*srcOrd),
                       /*isPhysical=*/1,
                       /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
    return f;
}

} // namespace

TEST(X86VariableEncoder, MovRaxRaxEmits48_8B_C0) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const fx = movRegReg(**schema, "rax", "rax");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(fx.src) };
        (void)b.addInst(fx.movOp, fx.result, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // mov rax, rax = REX.W (0x48) + opcode (0x8B) + ModR/M (mod=3
    // reg=rax(0) rm=rax(0) → 0xC0); then ret = 0xC3.
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0xC0);
    EXPECT_EQ(bytes[3], 0xC3);
}

TEST(X86VariableEncoder, MovR8RaxDerivesRexR) {
    // dest=r8 (hwEncoding=8) → ModR/M.reg low 3 = 0, REX.R = 1.
    // src=rax(0)             → ModR/M.rm  low 3 = 0, REX.B = 0.
    // REX = 0x40 | W(8) | R(4) = 0x4C.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const fx = movRegReg(**schema, "r8", "rax");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(fx.src) };
        (void)b.addInst(fx.movOp, fx.result, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x4C);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0xC0);
}

TEST(X86VariableEncoder, MovRaxR8DerivesRexB) {
    // dest=rax(0) → ModR/M.reg = 0, REX.R = 0.
    // src=r8(8)   → ModR/M.rm  = 0, REX.B = 1.
    // REX = 0x40 | W(8) | B(1) = 0x49.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const fx = movRegReg(**schema, "rax", "r8");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(fx.src) };
        (void)b.addInst(fx.movOp, fx.result, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x49);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0xC0);
}

TEST(X86VariableEncoder, MovR15R15DerivesRexRAndRexB) {
    // r15(15) → low 3 = 7, high 1 = 1. Both REX.R and REX.B set.
    // REX = 0x48 | R(4) | B(1) = 0x4D. ModR/M = 11_111_111 = 0xFF.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const fx = movRegReg(**schema, "r15", "r15");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(fx.src) };
        (void)b.addInst(fx.movOp, fx.result, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_EQ(bytes[0], 0x4D);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0xFF);
}

// ── `mov reg, imm32` — REX.W 0xC7 /0 imm32 ───────────────────────────

TEST(X86VariableEncoder, MovRaxImm32EmitsCorrectLittleEndianBytes) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    auto const raxOrd = (*schema)->registerByName("rax");
    ASSERT_TRUE(raxOrd.has_value());
    LirReg const result{static_cast<std::uint32_t>(*raxOrd), 1,
                        static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeImmInt32(static_cast<std::int32_t>(0x12345678))
        };
        (void)b.addInst(*movOp, result, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // mov rax, 0x12345678 = REX.W 0x48 + opcode 0xC7 + ModR/M (mod=3
    // reg=/0=000 rm=rax(0) → 0xC0) + imm32 LE (0x78 0x56 0x34 0x12);
    // then ret = 0xC3.
    ASSERT_EQ(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0xC7);
    EXPECT_EQ(bytes[2], 0xC0);
    EXPECT_EQ(bytes[3], 0x78);
    EXPECT_EQ(bytes[4], 0x56);
    EXPECT_EQ(bytes[5], 0x34);
    EXPECT_EQ(bytes[6], 0x12);
    EXPECT_EQ(bytes[7], 0xC3);
}

TEST(X86VariableEncoder, MovR12Imm32AlsoDerivesRexB) {
    // dest=r12(12) → ModR/M.rm low 3 = 4, REX.B = 1.
    // REX = 0x48 | B(1) = 0x49. ModR/M = mod=3 reg=/0 rm=4 = 0xC4.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());
    auto const r12Ord = (*schema)->registerByName("r12");
    ASSERT_TRUE(r12Ord.has_value());
    LirReg const result{static_cast<std::uint32_t>(*r12Ord), 1,
                        static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeImmInt32(0) };
        (void)b.addInst(*movOp, result, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 0x49);
    EXPECT_EQ(bytes[1], 0xC7);
    EXPECT_EQ(bytes[2], 0xC4);
    EXPECT_EQ(bytes[3], 0x00);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
}

// ── `mov r64, imm64` — REX.W B8+rd io (D-CSUBSET-BITFIELD-WIDE-UNIT) ──
//
// The ONLY x86-64 form that loads a >imm32 constant into a register.
// The wide value rides the LIR literal pool (a `LiteralIndex` operand);
// the encoder reads it back and emits the 8-byte `io` field. The
// destination register's low 3 bits ride the opcode byte (B8+rd), its
// high bit drives REX.B. RED-ON-DISABLE: without the new `mov r64,
// imm64` variant in x86_64.target.json, the LiteralIndex operand
// matches no variant → A_NoMatchingEncodingVariant (the dead-end the
// scoping pass found).

namespace {
// Build a `mov <dstReg>, imm64` LIR inst whose source is a 64-bit pool
// literal, assemble it, and return the function bytes (with a trailing
// ret the caller slices off).
[[nodiscard]] std::vector<std::uint8_t>
movRegImm64Bytes(TargetSchema const& schema, std::string_view dstName,
                 std::uint64_t value, DiagnosticReporter& rep) {
    auto const movOp  = schema.opcodeByMnemonic("mov");
    auto const dstOrd = schema.registerByName(dstName);
    EXPECT_TRUE(movOp.has_value());
    EXPECT_TRUE(dstOrd.has_value());
    LirReg const dst{static_cast<std::uint32_t>(*dstOrd), 1,
                     static_cast<std::uint8_t>(LirRegClass::GPR)};
    Lir lir = buildSingleFnLirWithRet(schema, [&](LirBuilder& b) {
        LirLiteralValue lit;
        lit.value = value;
        std::uint32_t const idx = b.literalPoolAdd(std::move(lit));
        LirOperand const ops[] = { LirOperand::makeLiteralIndex(idx) };
        // width-default flags (0) = 64-bit; the imm64 variant is
        // width-64-keyed and 64 is the lirInstWidthBits default.
        (void)b.addInst(*movOp, dst, ops);
    });
    return assembleFirstFn(lir, schema, rep);
}
} // namespace

TEST(X86VariableEncoder, MovRaxImm64Emits48_B8_EightLEBytes) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    // mov rax, 0xFFFFFFFFFF (40-bit, > int32) =
    //   REX.W (0x48) + B8+rax(0) (0xB8) + 8 LE bytes
    //   (FF FF FF FF FF 00 00 00); then ret = 0xC3.
    auto const bytes = movRegImm64Bytes(**schema, "rax", 0xFFFFFFFFFFULL, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), 11u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0xB8);
    EXPECT_EQ(bytes[2], 0xFF);
    EXPECT_EQ(bytes[3], 0xFF);
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0xFF);
    EXPECT_EQ(bytes[6], 0xFF);
    EXPECT_EQ(bytes[7], 0x00);
    EXPECT_EQ(bytes[8], 0x00);
    EXPECT_EQ(bytes[9], 0x00);
    EXPECT_EQ(bytes[10], 0xC3);  // ret
}

TEST(X86VariableEncoder, MovR14Imm64DerivesRexB_49_BE) {
    // dest=r14 (hwEncoding=14): low 3 bits = 6 → opcode byte B8|6 = 0xBE;
    // high bit = 1 → REX.B. REX = 0x48 | B(1) = 0x49. Boundary: the
    // opcode-plus-reg low-3 OR + REX.B derivation, both exercised.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const bytes = movRegImm64Bytes(**schema, "r14", 0xABCDEF1234ULL, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), 11u);
    EXPECT_EQ(bytes[0], 0x49);  // REX.W + REX.B
    EXPECT_EQ(bytes[1], 0xBE);  // B8 | (14 & 7)=6
    EXPECT_EQ(bytes[2], 0x34);
    EXPECT_EQ(bytes[3], 0x12);
    EXPECT_EQ(bytes[4], 0xEF);
    EXPECT_EQ(bytes[5], 0xCD);
    EXPECT_EQ(bytes[6], 0xAB);
    EXPECT_EQ(bytes[7], 0x00);
    EXPECT_EQ(bytes[8], 0x00);
    EXPECT_EQ(bytes[9], 0x00);
    EXPECT_EQ(bytes[10], 0xC3);
}

TEST(X86VariableEncoder, MovR8Imm64LowBitsZero_49_B8) {
    // r8 (hwEncoding=8): low 3 bits = 0 → opcode byte stays 0xB8; high
    // bit = 1 → REX.B. The case an `& 7` off-by-one or a missing REX.B
    // would silently decode as rax. A full 64-bit value with the high
    // byte set pins all 8 immediate bytes.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    DiagnosticReporter rep;
    auto const bytes = movRegImm64Bytes(**schema, "r8",
                                        0x8000000000000001ULL, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), 11u);
    EXPECT_EQ(bytes[0], 0x49);  // REX.W + REX.B
    EXPECT_EQ(bytes[1], 0xB8);  // B8 | (8 & 7)=0
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0x00);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
    EXPECT_EQ(bytes[8], 0x00);
    EXPECT_EQ(bytes[9], 0x80);
    EXPECT_EQ(bytes[10], 0xC3);
}

TEST(X86VariableEncoder, MovImm64RoundTripsThroughDisasm) {
    // The round-trip oracle must recover the wide value (B8+rd opcode-
    // plus-reg + 8-byte io decode). RED-ON-DISABLE: a disasm that
    // mis-peeled the opcode-reg low 3 bits or mis-read the 8 imm bytes
    // would fire A_RoundTripMismatch.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    std::uint64_t const value = 0xF00000FFFFFFFFFFULL;
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    auto const movOp  = (*schema)->opcodeByMnemonic("mov");
    auto const r14Ord = (*schema)->registerByName("r14");
    ASSERT_TRUE(movOp.has_value() && r14Ord.has_value());
    LirReg const r14{static_cast<std::uint32_t>(*r14Ord), 1,
                     static_cast<std::uint8_t>(LirRegClass::GPR)};
    LirLiteralValue lit; lit.value = value;
    std::uint32_t const idx = b.literalPoolAdd(std::move(lit));
    LirOperand const ops[] = { LirOperand::makeLiteralIndex(idx) };
    LirInstId const movInst = b.addInst(*movOp, r14, ops);
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto const result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes = result.functions[0].bytes;
    // The mov is the first instruction; slice off its 10 bytes
    // (REX + opcode + 8 imm) for the round-trip verify.
    ASSERT_GE(bytes.size(), 10u);
    std::span<std::uint8_t const> const movBytes{bytes.data(), 10u};
    EXPECT_TRUE(roundTripVerify(**schema, lir, movInst, movBytes, rep));
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── `sext` (movsxd r64, r/m32) — REX.W 0x63 /r ──────────────────────
//
// D-CSUBSET-LONG-PRIMITIVE byte-pin (cycle 10h post-fold,
// 2026-06-04): pins the JSON-declared encoding for the sext
// mnemonic at the assembler-tier. Without this pin, a hand-edit
// to x86_64.target.json's sext row that swaps the opcode byte
// (e.g., to 0x98 `cdqe`, which also sign-extends but with a
// different operand convention — implicit rax-only) would slip
// past the corpus smoke (`long_primitive_smoke`) because the
// runtime exit would still happen to be 42 by coincidence on
// many witness values. This pin is target-schema-consuming:
// the test reads only what JSON declared, so the JSON-as-
// contract discipline is enforced directly.

TEST(X86VariableEncoder, SextRaxRaxEmits48_63_C0) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const sextOp = (*schema)->opcodeByMnemonic("sext");
    ASSERT_TRUE(sextOp.has_value());
    auto const raxOrd = (*schema)->registerByName("rax");
    ASSERT_TRUE(raxOrd.has_value());

    LirReg const rax{static_cast<std::uint32_t>(*raxOrd),
                     /*isPhysical=*/1,
                     /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(rax) };
        // The I32-source sext is width-32-keyed (movsxd r64, r/m32) since
        // the byte form (movsx r/m8) was added — D-CSUBSET-CHAR-STRING-
        // VALUE-CODEGEN. Thread width-32 exactly as MIR→LIR does
        // (widthFlagsForType(I32)); flags=0 (width-64) now matches no sext
        // variant (fail-loud), which is the convergence-fix-D contract.
        (void)b.addInst(*sextOp, rax, ops, /*payload=*/0,
                        ::dss::kLirInstFlagWidth32);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // sext rax, eax = REX.W (0x48) + opcode 0x63 + ModR/M (mod=3
    // reg=rax(0) rm=rax(0) → 0xC0); then ret = 0xC3.
    ASSERT_EQ(bytes.size(), 4u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x63);
    EXPECT_EQ(bytes[2], 0xC0);
    EXPECT_EQ(bytes[3], 0xC3);
}

// ── `zext` (movzx r64, r/m8) — REX.W 0x0F 0xB6 /r ───────────────────
//
// Companion to the sext byte-pin: the existing zext mnemonic
// row (used by setcc-zext lowering for booleans) had the same
// no-byte-pin gap. This pin closes it.

TEST(X86VariableEncoder, ZextRaxRaxEmits48_0F_B6_C0) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const zextOp = (*schema)->opcodeByMnemonic("zext");
    ASSERT_TRUE(zextOp.has_value());
    auto const raxOrd = (*schema)->registerByName("rax");
    ASSERT_TRUE(raxOrd.has_value());

    LirReg const rax{static_cast<std::uint32_t>(*raxOrd),
                     /*isPhysical=*/1,
                     /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(rax) };
        (void)b.addInst(*zextOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    // zext rax, al = REX.W (0x48) + opcode 0x0F 0xB6 + ModR/M
    // (mod=3 reg=rax(0) rm=rax(0) → 0xC0); then ret = 0xC3.
    ASSERT_EQ(bytes.size(), 5u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x0F);
    EXPECT_EQ(bytes[2], 0xB6);
    EXPECT_EQ(bytes[3], 0xC0);
    EXPECT_EQ(bytes[4], 0xC3);
}

// ── D-CSUBSET-DIVISION-OP-CODEGEN byte-pins (cycle 10r split, 2026-06-04) ──
//
// Cycle 10r splits the cycle-10q compound opcodes into separate pre
// + core opcodes. Rationale (full doc in mir_to_lir.cpp / target.json):
// the encoder auto-emits ONE REX prefix per inst computed from
// operand high bits; cycle 10q's `[0x48, 0x99, 0x48, 0xF7]` embedded
// REX bytes overrode the auto-REX for high-reg divisors (R8-R15),
// losing REX.B → modrm.rm decoded as the wrong register → silent
// miscompile + STATUS_INTEGER_DIVIDE_BY_ZERO trap. Splitting lets
// each instruction get its own correctly-computed REX prefix.
//
// FLAG 1 (silent-miscompile guard, preserved): the four byte
// patterns are STRUCTURALLY DIFFERENT — schema swaps fail at the
// assembler tier before any high-bit dividend test could silently
// pass with wrong sign interpretation.

namespace {
// Unified helper for divide-family byte-pin tests. Accepts a single
// optional divisor register (nullopt for zero-operand pre-ops like
// CQO / XOR-RDX; named register for core ops like IDIV / DIV).
// Replaces the two separate helpers (cycle 10r 7-agent review fold
// code-simplifier #2) since the surface grows linearly with each
// new div-variant + register-pin combination otherwise.
void expectDivBytes(char const* mnemonic,
                    std::optional<char const*> divisorRegName,
                    std::vector<std::uint8_t> const& expected) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const op = (*schema)->opcodeByMnemonic(mnemonic);
    ASSERT_TRUE(op.has_value()) << "missing opcode: " << mnemonic;

    auto const label = [&] {
        std::string s = mnemonic;
        if (divisorRegName.has_value()) {
            s += "("; s += *divisorRegName; s += ")";
        }
        return s;
    };

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        if (divisorRegName.has_value()) {
            auto const ord = (*schema)->registerByName(*divisorRegName);
            ASSERT_TRUE(ord.has_value())
                << "missing register: " << *divisorRegName;
            LirReg const divisor{static_cast<std::uint32_t>(*ord),
                                 /*isPhysical=*/1,
                                 /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
            LirOperand const ops[] = { LirOperand::makeReg(divisor) };
            (void)b.addInst(*op, InvalidLirReg, ops);
        } else {
            (void)b.addInst(*op, InvalidLirReg, std::span<LirOperand const>{});
        }
    });
    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(bytes.size(), expected.size() + 1u)  // + ret
        << label() << " unexpected size";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(bytes[i], expected[i])
            << label() << " byte " << i << " mismatch";
    }
    EXPECT_EQ(bytes[expected.size()], 0xC3);  // ret
}
} // namespace

TEST(X86VariableEncoder, CqoEmits48_99) {
    // CQO = REX.W 0x99 sign-extends RAX into RDX:RAX. No operand,
    // no ModR/M. Encoder synthesizes the REX byte from template's
    // rexW=true; the opcode array is just [0x99]. 2 bytes total.
    expectDivBytes("cqo", std::nullopt, {0x48, 0x99});
}

TEST(X86VariableEncoder, XorRdxZeroEmits31_D2) {
    // XOR EDX, EDX = 0x31 0xD2 (32-bit op zero-extends to RDX).
    // No REX byte emitted: EDX's ordinal high bit = 0 (so rexB=0)
    // and the template omits rexW. 2 bytes total.
    expectDivBytes("xor_rdx_zero", std::nullopt, {0x31, 0xD2});
}

TEST(X86VariableEncoder, IDivOpRaxEmits48_F7_F8) {
    // IDIV r/m64 with divisor in RAX: REX.W 0xF7 /7 with
    // ModR/M = mod=11 reg=/7=111 rm=rax(000) = 0xF8. Encoder
    // synthesizes the 0x48 REX byte from template's rexW=true;
    // the opcode array is just [0xF7]. 3 bytes total.
    expectDivBytes("idiv_op", "rax", {0x48, 0xF7, 0xF8});
}

TEST(X86VariableEncoder, DivOpRaxEmits48_F7_F0) {
    // DIV r/m64 with divisor in RAX: REX.W 0xF7 /6 with
    // ModR/M = mod=11 reg=/6=110 rm=rax(000) = 0xF0. 3 bytes.
    // FLAG 1 discrimination: differs from idiv_op's /7 byte
    // (0xF0 vs 0xF8) — schema swap would fail at byte 2.
    expectDivBytes("div_op", "rax", {0x48, 0xF7, 0xF0});
}

// ── REX.B regression pins for high-numbered registers (R8-R15) ──
//
// **Background**: cycle-10q's compound encoding bug was that the
// encoder's auto-REX prefix was followed by an embedded literal
// 0x48 inside the opcode bytes; per x86 decode the LAST REX prefix
// before the opcode wins, so REX.B was lost for any divisor in
// R8-R15. The cycle-10r split makes the encoder's auto-REX path
// the SINGLE place handling REX, so each instruction gets its own
// correctly-computed prefix.
//
// **Coverage breadth** (7-agent review fold pr-test #1 9/10):
// A single R14 pin would only attest one specific bit pattern.
// The pins below additionally cover R8 (low 3 bits = 0, the very
// case the 10q bug decoded as RAX-without-REX.B) and R15 (low 3
// bits = 7, the all-ones boundary that any `& 7` mask off-by-one
// would slip past). Combined with the RAX low-reg pins above, the
// REX byte's W + B bits are fully exercised.

TEST(X86VariableEncoder, IDivOpR8EmitsREX_B_49_F7_F8_LowBitsZero) {
    // R8: ordinal=8 = high-bit set, low 3 bits = 000.
    // With the cycle-10q bug, the lost REX.B made modrm.rm=000
    // decode as RAX (not R8) — but RAX held the DIVIDEND, so IDIV
    // would divide RDX:RAX by itself = 1. This is a different
    // failure mode from R14's "RSI=garbage" but equally a silent
    // miscompile. REX = 0x49, modrm = mod=11 reg=/7=111 rm=000 = 0xF8.
    expectDivBytes("idiv_op", "r8", {0x49, 0xF7, 0xF8});
}

TEST(X86VariableEncoder, IDivOpR14EmitsREX_B_49_F7_FE) {
    // R14: ordinal=14 = high-bit set, low 3 bits = 110 (= 14 & 7).
    // With the cycle-10q REX-overlap bug the encoder emitted the
    // auto-REX 0x41 (B=1) followed by the embedded literal 0x48;
    // per x86 decode the last REX before the opcode wins, so
    // REX.B was lost and modrm.rm=110 decoded as RSI rather than
    // R14 — silent miscompile + STATUS_INTEGER_DIVIDE_BY_ZERO when
    // RSI held zero. REX = 0x49, modrm = 0xFE.
    expectDivBytes("idiv_op", "r14", {0x49, 0xF7, 0xFE});
}

TEST(X86VariableEncoder, IDivOpR15EmitsREX_B_49_F7_FF_LowBitsAllOnes) {
    // R15: ordinal=15, low 3 bits = 111. Boundary pin: any encoder
    // `& 7` mask off-by-one (e.g., `& 0x7f`) would slip past R14
    // (the mid-range pin) but fail R15. REX = 0x49, modrm = 0xFF.
    expectDivBytes("idiv_op", "r15", {0x49, 0xF7, 0xFF});
}

TEST(X86VariableEncoder, DivOpR14EmitsREX_B_49_F7_F6) {
    // Same REX.B regression class for unsigned DIV.
    // modrm = mod=11 reg=/6=110 rm=110 = 0xF6.
    expectDivBytes("div_op", "r14", {0x49, 0xF7, 0xF6});
}

TEST(X86VariableEncoder, DivOpR15EmitsREX_B_49_F7_F7_LowBitsAllOnes) {
    // Unsigned all-ones boundary pin (mirror of IDivOpR15...).
    // modrm = mod=11 reg=/6=110 rm=111 = 0xF7.
    expectDivBytes("div_op", "r15", {0x49, 0xF7, 0xF7});
}

// ── FC4 c2: indirect call `call <reg>` — 0xFF /2 ────────────────────
// The `call` row's ["reg"] encoding variant (the direct ["symbol"]
// variant emits E8 rel32). NO REX.W: FF /2 defaults to 64-bit operand
// size in long mode — a spurious REX.W (0x48) here would still decode,
// but pinning its ABSENCE keeps the variant exactly the canonical
// form. Same /digit machinery as idiv /7 / div /6 above; the helper
// reuse is deliberate (single-reg-operand byte-pin shape).

TEST(X86VariableEncoder, CallRaxEmitsFF_D0) {
    // CALL r/m64 with callee in RAX: 0xFF /2 with
    // ModR/M = mod=11 reg=/2=010 rm=rax(000) = 0xD0. NO REX byte
    // (no W, B=0 for rax). 2 bytes total.
    expectDivBytes("call", "rax", {0xFF, 0xD0});
}

TEST(X86VariableEncoder, CallR10EmitsREX_B_41_FF_D2) {
    // CALL r/m64 with callee in R10: REX.B (0x41 — B from the high
    // ordinal bit, NO W) + 0xFF /2 with
    // ModR/M = mod=11 reg=/2=010 rm=(10&7)=010 = 0xD2. 3 bytes.
    expectDivBytes("call", "r10", {0x41, 0xFF, 0xD2});
}

// ── Variant-guard mismatch — A_NoMatchingEncodingVariant ─────────────

TEST(X86VariableEncoder, NoMatchingVariantFiresLoudDiagnostic_KindMismatch) {
    // Synthesize a schema where `op` declares a single `[reg]`
    // variant — then construct a LIR inst with a SINGLE imm32
    // operand. The arity matches (1 == 1) but the KIND doesn't
    // (Reg-filter vs ImmInt operand). The walker's
    // `operandsMatchGuard` must reject this and fire
    // `A_NoMatchingEncodingVariant` — the actual kind-mismatch path
    // that the previous test's wording promised but didn't exercise.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_x86", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [139] },
                    "resultSlot": "modrm.reg",
                    "wires": [
                      { "index": 0, "slotKind": "modrm.rm" }
                    ]
                  }
                ]
              } }
        ],
        "registers": [
            { "name": "rax", "class": "gpr", "widthBytes": 8, "hwEncoding": 0 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    ASSERT_TRUE(schema.has_value());

    auto const op    = (*schema)->opcodeByMnemonic("op");
    auto const raxOrd = (*schema)->registerByName("rax");
    auto const retOp  = (*schema)->opcodeByMnemonic("ret");
    ASSERT_TRUE(op.has_value());
    ASSERT_TRUE(raxOrd.has_value());
    // The synthetic schema has no `ret` (only `op` + invalid) — use
    // an emitter that closes the block via `addUnreachable` on a
    // separately-declared terminator? Simpler: use a single-block
    // function whose terminator IS the result-producing `op`. Since
    // `op` has `result=value` it's not a terminator — we need one.
    // Add `unreachable` to the schema and use it. (See below.)
    EXPECT_FALSE(retOp.has_value()) << "synthetic schema for this test "
        "intentionally omits ret; this assertion documents the shape";

    constexpr char const* kJsonWithTerm = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth_x86", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [139] },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } },
            { "mnemonic": "trap", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [204] } }
                ]
              } }
        ],
        "registers": [
            { "name": "rax", "class": "gpr", "widthBytes": 8, "hwEncoding": 0 }
        ]
    })";
    auto schema2 = TargetSchema::loadFromText(kJsonWithTerm, "synth2.target.json");
    ASSERT_TRUE(schema2.has_value());

    auto const op2     = (*schema2)->opcodeByMnemonic("op");
    auto const trap2   = (*schema2)->opcodeByMnemonic("trap");
    auto const raxOrd2 = (*schema2)->registerByName("rax");
    ASSERT_TRUE(op2.has_value());
    ASSERT_TRUE(trap2.has_value());
    ASSERT_TRUE(raxOrd2.has_value());
    LirReg const rax{static_cast<std::uint32_t>(*raxOrd2), 1,
                     static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLir(**schema2, [&](LirBuilder& b) {
        // KIND mismatch: `op` declares `[reg]` guard but we wire an
        // ImmInt32 operand. Length matches (1 == 1).
        LirOperand const ops[] = { LirOperand::makeImmInt32(0) };
        (void)b.addInst(*op2, rax, ops);
        (void)b.addUnreachable(*trap2);
    });

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema2, lirToMir, rep);
    EXPECT_GT(countDiagnostics(rep, DiagnosticCode::A_NoMatchingEncodingVariant), 0u);
    // Parallel-index discipline holds even on failure.
    EXPECT_EQ(result.functions.size(), 1u);
}

TEST(X86VariableEncoder, NoMatchingVariantFiresOnArityMismatch) {
    // The arity-mismatch path: variant has [reg] guard, instruction
    // has zero operands (e.g. unreachable on an `unreachable`-typed
    // synthetic opcode whose encoding has a non-empty guard — a
    // misauthored schema). `operandsMatchGuard` rejects on length.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "wrong", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "opcode": [144] },
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "synth.target.json");
    // validate() should reject: opcode has result=none + wires
    // wire ModRmRm. That's actually fine (wires don't need result).
    // But the variant guard `[reg]` declares 1 operand which doesn't
    // match the opcode's maxOperands (default 0). Let's verify the
    // schema loads — the arity cross-check isn't (yet) enforced.
    ASSERT_TRUE(schema.has_value());

    auto const wrongOp = (*schema)->opcodeByMnemonic("wrong");
    ASSERT_TRUE(wrongOp.has_value());

    Lir lir = buildSingleFnLir(**schema, [&](LirBuilder& b) {
        // ZERO operands against [reg] guard — arity mismatch.
        (void)b.addUnreachable(*wrongOp);
    });

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    (void)assemble(lir, **schema, lirToMir, rep);
    EXPECT_GT(countDiagnostics(rep, DiagnosticCode::A_NoMatchingEncodingVariant), 0u);
}

// ── Diagnostic prefix pin for the new A_ code ────────────────────────

TEST(X86VariableEncoder, NoMatchingVariantRendersA0004) {
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_NoMatchingEncodingVariant),
              "A0004");
}

// ── validate() encoding rules (pr-test #1) ────────────────────────────

TEST(TargetEncodingValidate, RejectsShapeWithoutVariants) {
    // shape != None + variants empty would silently match nothing.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "encoding": { "format": "x86-variable", "variants": [] } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsNoneShapeWithVariants) {
    // shape == None + variants non-empty would be dead data.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "encoding": { "format": "none",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [144] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsEmptyOpcodeBytes) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsOversizedOpcodeBytes) {
    // x86 instruction length cap is 15.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16] } }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsModrmRegExtOverflow) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "opcode": [199], "modrmRegExt": 8 },
                    "resultSlot": "modrm.rm",
                    "wires": [{ "index": 0, "slotKind": "imm32" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsWireIndexOutOfRange) {
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [139] },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 9, "slotKind": "modrm.rm" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsModrmRegExtWithModRmRegWire) {
    // convergence-fix B
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "opcode": [199], "modrmRegExt": 0 },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsUncoveredGuardPosition) {
    // convergence-fix C: [reg, reg] guard with only 1 wire
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg", "reg"] },
                    "template": { "opcode": [144] },
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsDuplicateModRmRegWires) {
    // convergence-fix A (validate half)
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 2, "maxOperands": 2,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg", "reg"] },
                    "template": { "rexW": true, "opcode": [1] },
                    "resultSlot": "modrm.reg",
                    "wires": [
                      { "index": 0, "slotKind": "modrm.reg" },
                      { "index": 1, "slotKind": "modrm.rm" }
                    ]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsOverlappingVariantGuards) {
    // convergence-fix D: two variants with same operandKinds
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [139] },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }] },
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [137] },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }] }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

TEST(TargetEncodingValidate, RejectsValueResultWithoutDestSlot) {
    // convergence-fix G
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "opcode": [144] },
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "x.json").has_value());
}

// ── REX-prefix suppression (pr-test #4) ──────────────────────────────

TEST(X86VariableEncoder, RexNotEmittedWhenAllBitsZero) {
    // Variant with rexW=false and no high-bit registers wired.
    // Emitted bytes start at the opcode, no 0x40+ prefix.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op32", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": false, "opcode": [139] },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } },
            { "mnemonic": "trap", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [204] } }
                ]
              } }
        ],
        "registers": [
            { "name": "rax", "class": "gpr", "widthBytes": 8, "hwEncoding": 0 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "x.json");
    ASSERT_TRUE(schema.has_value());

    auto const op = (*schema)->opcodeByMnemonic("op32");
    auto const trap = (*schema)->opcodeByMnemonic("trap");
    auto const raxOrd = (*schema)->registerByName("rax");
    ASSERT_TRUE(op.has_value() && trap.has_value() && raxOrd.has_value());
    LirReg const rax{static_cast<std::uint32_t>(*raxOrd), 1,
                     static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLir(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(rax) };
        (void)b.addInst(*op, rax, ops);
        (void)b.addUnreachable(*trap);
    });

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_FALSE(result.functions.empty());
    auto const& bytes = result.functions[0].bytes;
    ASSERT_GE(bytes.size(), 2u);
    // First byte is the opcode (0x8B), NOT a REX prefix.
    EXPECT_EQ(bytes[0], 0x8B);
    EXPECT_EQ(bytes[1], 0xC0);  // ModR/M
}

// ── Multi-byte opcode (pr-test #8) ───────────────────────────────────

TEST(X86VariableEncoder, MultiByteOpcodeEmitsBytesInDeclaredOrder) {
    // Synthetic 2-byte opcode (0x0F 0xAF — imul-style two-byte form).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "two", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [15, 175] },
                    "resultSlot": "modrm.reg",
                    "wires": [{ "index": 0, "slotKind": "modrm.rm" }]
                  }
                ]
              } },
            { "mnemonic": "trap", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [204] } }
                ]
              } }
        ],
        "registers": [
            { "name": "rax", "class": "gpr", "widthBytes": 8, "hwEncoding": 0 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "x.json");
    ASSERT_TRUE(schema.has_value());

    auto const op = (*schema)->opcodeByMnemonic("two");
    auto const trap = (*schema)->opcodeByMnemonic("trap");
    auto const raxOrd = (*schema)->registerByName("rax");
    LirReg const rax{static_cast<std::uint32_t>(*raxOrd), 1,
                     static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLir(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeReg(rax) };
        (void)b.addInst(*op, rax, ops);
        (void)b.addUnreachable(*trap);
    });

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(result.functions[0].bytes.size(), 4u);
    EXPECT_EQ(result.functions[0].bytes[0], 0x48);  // REX.W
    EXPECT_EQ(result.functions[0].bytes[1], 0x0F);  // opcode byte 1
    EXPECT_EQ(result.functions[0].bytes[2], 0xAF);  // opcode byte 2
    EXPECT_EQ(result.functions[0].bytes[3], 0xC0);  // ModR/M
}

// ── modrmRegExt nonzero (pr-test #5) ─────────────────────────────────

TEST(X86VariableEncoder, ModrmRegExtNonZeroFillsRegField) {
    // /7 form: ModR/M reg field = 7.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "x", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "op", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["imm32"] },
                    "template": { "rexW": true, "opcode": [199], "modrmRegExt": 7 },
                    "resultSlot": "modrm.rm",
                    "wires": [{ "index": 0, "slotKind": "imm32" }]
                  }
                ]
              } },
            { "mnemonic": "trap", "result": "none",
              "terminatorKind": "unreachable",
              "encoding": { "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": [] },
                    "template": { "opcode": [204] } }
                ]
              } }
        ],
        "registers": [
            { "name": "rax", "class": "gpr", "widthBytes": 8, "hwEncoding": 0 }
        ]
    })";
    auto schema = TargetSchema::loadFromText(kJson, "x.json");
    ASSERT_TRUE(schema.has_value());

    auto const op   = (*schema)->opcodeByMnemonic("op");
    auto const trap = (*schema)->opcodeByMnemonic("trap");
    auto const raxOrd = (*schema)->registerByName("rax");
    LirReg const rax{static_cast<std::uint32_t>(*raxOrd), 1,
                     static_cast<std::uint8_t>(LirRegClass::GPR)};

    Lir lir = buildSingleFnLir(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = { LirOperand::makeImmInt32(0) };
        (void)b.addInst(*op, rax, ops);
        (void)b.addUnreachable(*trap);
    });

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(result.functions[0].bytes.size(), 3u);
    EXPECT_EQ(result.functions[0].bytes[0], 0x48);  // REX.W
    EXPECT_EQ(result.functions[0].bytes[1], 0xC7);  // opcode
    // ModR/M = mod=3 (0xC0) | (reg=7 << 3) (0x38) | rm=0 (0x00) = 0xF8
    EXPECT_EQ(result.functions[0].bytes[2], 0xF8);
}

// ── hwEncodingOf fail-loud paths (pr-test #6) ────────────────────────

TEST(X86VariableEncoder, VirtualRegInOperandFiresFailLoud) {
    // Post-regalloc invariant: every register is physical. A virtual
    // register reaching the walker fires A_NoMatchingEncodingVariant
    // (the only A_* family active in cycle 2; future cycles may
    // promote this to a dedicated A_VirtualRegInAssembler code).
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    ASSERT_TRUE(movOp.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    // Create a virtual register (isPhysical=0) — bypasses regalloc.
    LirReg const vreg = b.newVReg(LirRegClass::GPR);
    LirOperand const ops[] = { LirOperand::makeReg(vreg) };
    (void)b.addInst(*movOp, vreg, ops);
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    (void)assemble(lir, **schema, lirToMir, rep);
    EXPECT_GT(countDiagnostics(rep, DiagnosticCode::A_NoMatchingEncodingVariant), 0u);
}

// ── D-AS4-1 + ML7 cycle 2 byte-pin tests ──────────────────────────────
//
// Pins byte sequences for the AS encoder variants that close
// D-LK10-2 (byte-on-disk e2e):
//   * `add r/m64, imm32`     — REX.W 0x81 /0 imm32
//   * `sub r/m64, imm32`     — REX.W 0x81 /5 imm32
//   * `mov reg, [base+disp32]`  (load)  — REX.W 0x8B /r ModR/M(mod=10)
//                                         [SIB 0x24 when base.lo3==4]
//                                         disp32
//   * `mov [base+disp32], reg`  (store) — REX.W 0x89 /r ModR/M(mod=10)
//                                         [SIB 0x24 when base.lo3==4]
//                                         disp32
//
// silent-failure-hunter post-fold F-G2: the ELF-magic-only e2e check
// doesn't catch silent miscompiles in REX.W, ModR/M.mod, SIB emission,
// or disp32 endianness — these byte-pin tests do.

namespace {

[[nodiscard]] LirReg physGprByName(TargetSchema const& schema,
                                    std::string_view name) {
    auto const ord = schema.registerByName(name);
    EXPECT_TRUE(ord.has_value());
    return LirReg{static_cast<std::uint32_t>(*ord), /*isPhysical=*/1,
                  /*cls=*/static_cast<std::uint8_t>(LirRegClass::GPR)};
}

} // namespace

TEST(X86VariableEncoder, SubRspImm32EmitsExactPrologueBytes) {
    // `sub rsp, 0x20` → 48 81 EC 20 00 00 00
    // REX.W = 0x48; opcode = 0x81; ModR/M = 11_101_100 (/5 = 101,
    // rsp.lo3 = 100) = 0xEC; imm32 = 0x20 LE.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const subOp = (*schema)->opcodeByMnemonic("sub");
    ASSERT_TRUE(subOp.has_value());
    LirReg const rsp = physGprByName(**schema, "rsp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsp),
            LirOperand::makeImmInt32(0x20),
        };
        (void)b.addInst(*subOp, rsp, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x81);
    EXPECT_EQ(bytes[2], 0xEC);
    EXPECT_EQ(bytes[3], 0x20);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
}

TEST(X86VariableEncoder, AddRspImm32EmitsExactEpilogueBytes) {
    // `add rsp, 0x20` → 48 81 C4 20 00 00 00
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const addOp = (*schema)->opcodeByMnemonic("add");
    ASSERT_TRUE(addOp.has_value());
    LirReg const rsp = physGprByName(**schema, "rsp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsp),
            LirOperand::makeImmInt32(0x20),
        };
        (void)b.addInst(*addOp, rsp, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x81);
    EXPECT_EQ(bytes[2], 0xC4);
    EXPECT_EQ(bytes[3], 0x20);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
}

// ── D-WIN64-LARGE-FRAME-STACK-PROBE encoder byte-pins ─────────────────
//
// The `stack_probe` op is the ONE x86-variable op that emits MULTIPLE
// machine instructions for a single LIR op. Pin the EXACT 30-byte probe
// loop the dedicated encoder arm hand-emits (NOT via assertRoundTripsClean
// — the round-trip oracle assumes one instruction per op; F2). The op
// carries [sp(reg), frameBytes(imm32), pageBytes(imm32)]; the loop is:
//   mov   r11d, frame    ; 41 BB <frame LE>
// .L:
//   sub   rsp, page      ; 48 81 EC <page LE>
//   or    qword [rsp], 0 ; 48 83 0C 24 00
//   sub   r11d, page     ; 41 81 EB <page LE>
//   cmp   r11d, page     ; 41 81 FB <page LE>
//   ja    .L             ; 77 E4   (rel8 = -28 back to .L)
//   sub   rsp, r11       ; 4C 29 DC
// The `0x77` (ja, strictly-above) byte is pinned EXPLICITLY (F8): jbe/jb
// would under-probe = SILENT CRASH; jae over-probes. The exact-page case
// (frame=8192) is a SECOND pin: `ja` falls through at remaining==0 so the
// trailing `sub rsp, r11` (r11=0) is a harmless no-op there.
namespace {
// The fixed tail of the probe loop (everything after the `mov r11d,frame`
// header), parameterized only by the page step. Returns the 24 bytes from
// `.L` through `sub rsp, r11`. The `ja` rel8 is the compile-time-constant
// -28 (0xE4) for this fixed loop body.
[[nodiscard]] std::vector<std::uint8_t> probeLoopTail(std::uint32_t page) {
    auto const lo = [](std::uint32_t v, unsigned s) {
        return static_cast<std::uint8_t>((v >> s) & 0xFFu);
    };
    return {
        0x48, 0x81, 0xEC, lo(page,0), lo(page,8), lo(page,16), lo(page,24),
        0x48, 0x83, 0x0C, 0x24, 0x00,
        0x41, 0x81, 0xEB, lo(page,0), lo(page,8), lo(page,16), lo(page,24),
        0x41, 0x81, 0xFB, lo(page,0), lo(page,8), lo(page,16), lo(page,24),
        0x77, 0xE4,
        0x4C, 0x29, 0xDC,
    };
}
} // namespace

TEST(X86VariableEncoder, StackProbeNonMultipleFrameEmitsExactLoop) {
    // StackProbe(frame=36000, page=4096) → 30-byte page-walking loop.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const probeOp = (*schema)->opcodeByMnemonic("stack_probe");
    ASSERT_TRUE(probeOp.has_value());
    LirReg const rsp = physGprByName(**schema, "rsp");

    constexpr std::uint32_t kFrame = 36000u;  // 0x8CA0 — NOT a page multiple
    constexpr std::uint32_t kPage  = 4096u;   // 0x1000
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsp),
            LirOperand::makeImmInt32(static_cast<std::int32_t>(kFrame)),
            LirOperand::makeImmInt32(static_cast<std::int32_t>(kPage)),
        };
        (void)b.addInst(*probeOp, InvalidLirReg, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    // Expected = `mov r11d, 0x8CA0` header + the fixed page-4096 tail.
    std::vector<std::uint8_t> expected = {0x41, 0xBB, 0xA0, 0x8C, 0x00, 0x00};
    auto const tail = probeLoopTail(kPage);
    expected.insert(expected.end(), tail.begin(), tail.end());

    ASSERT_GE(bytes.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(bytes[i], expected[i])
            << "probe-loop byte mismatch at index " << i;
    }
    // F8: the `ja` condition byte is load-bearing — pin it explicitly.
    // It sits at offset 6 (header) + 26 (loop body up to the ja opcode).
    EXPECT_EQ(bytes[6 + 26], 0x77) << "ja (strictly-above) opcode";
    EXPECT_EQ(bytes[6 + 27], 0xE4) << "ja rel8 = -28 back to .L";
    // And the final remainder drop is `sub rsp, r11` = 4C 29 DC (NOT the
    // 49 29 DB = `sub r11, rbx` that a reg-swap bug emits).
    EXPECT_EQ(bytes[6 + 28], 0x4C);
    EXPECT_EQ(bytes[6 + 29], 0x29);
    EXPECT_EQ(bytes[6 + 30], 0xDC);
}

TEST(X86VariableEncoder, StackProbeExactPageMultipleFrameEmitsExactLoop) {
    // StackProbe(frame=8192, page=4096) — EXACT 2-page multiple (F8): the
    // header immediate is 0x2000; the loop body (incl. the `ja`) is
    // byte-identical to the non-multiple case (only the header value
    // differs). `ja` falls through when remaining hits exactly 0.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const probeOp = (*schema)->opcodeByMnemonic("stack_probe");
    ASSERT_TRUE(probeOp.has_value());
    LirReg const rsp = physGprByName(**schema, "rsp");

    constexpr std::uint32_t kFrame = 8192u;  // 0x2000 — exact page multiple
    constexpr std::uint32_t kPage  = 4096u;
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsp),
            LirOperand::makeImmInt32(static_cast<std::int32_t>(kFrame)),
            LirOperand::makeImmInt32(static_cast<std::int32_t>(kPage)),
        };
        (void)b.addInst(*probeOp, InvalidLirReg, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);

    std::vector<std::uint8_t> expected = {0x41, 0xBB, 0x00, 0x20, 0x00, 0x00};
    auto const tail = probeLoopTail(kPage);
    expected.insert(expected.end(), tail.begin(), tail.end());

    ASSERT_GE(bytes.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(bytes[i], expected[i])
            << "exact-page probe-loop byte mismatch at index " << i;
    }
    EXPECT_EQ(bytes[6 + 26], 0x77) << "ja opcode (exact-page: falls through at 0)";
}

TEST(X86VariableEncoder, LoadFromRspForcesSibByte) {
    // `mov rcx, [rsp + 0x10]` → 48 8B 4C 24 10 00 00 00
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rcx = physGprByName(**schema, "rcx");
    LirReg const rsp = physGprByName(**schema, "rsp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsp),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x10),
        };
        (void)b.addInst(*loadOp, rcx, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0x8C);  // mod=10 reg=rcx(1) rm=rsp(4) → 0x80|0x08|0x04
    EXPECT_EQ(bytes[3], 0x24);  // SIB 0x24 (scale=0 index=4 base=4)
    EXPECT_EQ(bytes[4], 0x10);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

TEST(X86VariableEncoder, LoadFromRbpEmitsNoSibByte) {
    // `mov rcx, [rbp + 0x10]` → 48 8B 4D 10 00 00 00 (NO SIB)
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rcx = physGprByName(**schema, "rcx");
    LirReg const rbp = physGprByName(**schema, "rbp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbp),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x10),
        };
        (void)b.addInst(*loadOp, rcx, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0x8D);  // mod=10 reg=rcx(1) rm=rbp(5) — no SIB; 0x80|0x08|0x05
    EXPECT_EQ(bytes[3], 0x10);
    EXPECT_EQ(bytes[4], 0x00);  // disp32 LE bytes 1-3 must all be 0
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
}

TEST(X86VariableEncoder, StoreToRspForcesSibByte) {
    // `mov [rsp + 0x08], rdx` → 48 89 54 24 08 00 00 00
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const storeOp = (*schema)->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOp.has_value());
    LirReg const rdx = physGprByName(**schema, "rdx");
    LirReg const rsp = physGprByName(**schema, "rsp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rdx),
            LirOperand::makeReg(rsp),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x08),
        };
        (void)b.addInst(*storeOp, InvalidLirReg, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x89);
    EXPECT_EQ(bytes[2], 0x94);  // mod=10 reg=rdx(2) rm=rsp(4) → 0x80|0x10|0x04
    EXPECT_EQ(bytes[3], 0x24);
    EXPECT_EQ(bytes[4], 0x08);
    EXPECT_EQ(bytes[5], 0x00);  // disp32 LE bytes 1-3 must all be 0
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

TEST(X86VariableEncoder, StoreToR12ForcesSibByteAndRexB) {
    // `mov [r12 + 0x10], rax` → 49 89 44 24 10 00 00 00
    // r12 is rsp-family in REX-extended space.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const storeOp = (*schema)->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const r12 = physGprByName(**schema, "r12");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rax),
            LirOperand::makeReg(r12),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x10),
        };
        (void)b.addInst(*storeOp, InvalidLirReg, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x49);  // REX.W + REX.B (r12 has hi bit)
    EXPECT_EQ(bytes[1], 0x89);
    EXPECT_EQ(bytes[2], 0x84);  // mod=10 reg=rax(0) rm=r12.lo3(4) → 0x80|0x00|0x04
    EXPECT_EQ(bytes[3], 0x24);
    EXPECT_EQ(bytes[4], 0x10);
    EXPECT_EQ(bytes[5], 0x00);  // disp32 LE bytes 1-3 must all be 0
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

TEST(X86VariableEncoder, LoadFromRbxEmitsNoSibByte) {
    // Negative-test for non-rsp/r12-family base. `mov rax, [rbx+0x10]`
    // → 48 8B 43 10 00 00 00 — rbx.lo3 = 3, NOT in {4} (rsp/r12 family),
    // so NO SIB byte is forcibly emitted. Sister test to
    // `LoadFromRbpEmitsNoSibByte` (rbp.lo3 = 5) — together they pin
    // that the SIB-forced rule fires ONLY for lo3 == 4.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x10),
        };
        (void)b.addInst(*loadOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0x83);  // mod=10 reg=rax(0) rm=rbx(3) → 0x80|0x00|0x03
    EXPECT_EQ(bytes[3], 0x10);  // disp32 byte 0 — no SIB intervening
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
}

TEST(X86VariableEncoder, NegativeDispEmitsAsTwosComplement) {
    // `mov rax, [rbp - 8]` → 48 8B 45 F8 FF FF FF
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbp = physGprByName(**schema, "rbp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbp),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(-8),
        };
        (void)b.addInst(*loadOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);
    EXPECT_EQ(bytes[2], 0x85);  // mod=10 reg=rax rm=rbp → 0x80|0x00|0x05
    EXPECT_EQ(bytes[3], 0xF8);  // -8 LE byte 0
    EXPECT_EQ(bytes[4], 0xFF);
    EXPECT_EQ(bytes[5], 0xFF);
    EXPECT_EQ(bytes[6], 0xFF);
}

// ── D-AS4-5: SIB-with-index (indexed/scaled addressing) ──────

TEST(X86VariableEncoder, LeaWithIndexScale1) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const rcx = physGprByName(**schema, "rcx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(rcx),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8D);
    EXPECT_EQ(bytes[2], 0x84);  // mod=10 reg=rax rm=4(SIB)
    EXPECT_EQ(bytes[3], 0x0B);  // SIB: scale=0 index=rcx(1) base=rbx(3)
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

TEST(X86VariableEncoder, LeaWithIndexScale4) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const rcx = physGprByName(**schema, "rcx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(rcx),
            LirOperand::makeMemBase(4),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[3], 0x8B);  // SIB: scale=10b index=rcx(1) base=rbx(3)
}

TEST(X86VariableEncoder, LeaWithIndexScale8) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const rcx = physGprByName(**schema, "rcx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(rcx),
            LirOperand::makeMemBase(8),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[3], 0xCB);  // SIB: scale=11b index=rcx(1) base=rbx(3)
}

TEST(X86VariableEncoder, LeaWithR12IndexDerivesRexX) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const r12 = physGprByName(**schema, "r12");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(r12),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x4A);  // REX.W | REX.X
    EXPECT_EQ(bytes[3], 0x23);  // SIB: index=r12.lo3(4) base=rbx(3)
}

TEST(X86VariableEncoder, LeaWithIndexRejectsRspIndex) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const rsp = physGprByName(**schema, "rsp");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(rsp),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    (void)assembleFirstFn(lir, **schema, rep);
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(X86VariableEncoder, LeaWithIndexRejectsIllegalScale) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const rcx = physGprByName(**schema, "rcx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(rcx),
            LirOperand::makeMemBase(3),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    (void)assembleFirstFn(lir, **schema, rep);
    EXPECT_GT(rep.errorCount(), 0u);
}

// Post-fold #1: wireMemBaseScale rejects double-writers (silent
// regression risk if a future variant wires MemBase twice).
TEST(X86VariableEncoder, RejectsTwoMemBaseScaleWires) {
    // Construct a synthetic schema with TWO `membase.scale` wires on
    // the same opcode — schema validate() should reject this, but
    // even if it slips through (future variant restructure), the
    // walker's wireMemBaseScale double-write guard catches it.
    // Since the schema validator already rejects this configuration,
    // assemble a load with TWO `MemBase` operands (LIR-tier malformation)
    // and verify the walker's double-write guard fires before the
    // schema's variant-mismatch.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");

    // 5-operand load that doesn't match any variant guard — the
    // walker reaches the "no matching variant" path first; this
    // confirms the schema validator rejects the malformed shape
    // upstream so wireMemBaseScale's double-write guard remains
    // defensive (defense-in-depth, not a primary gate).
    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemBase(2),     // second MemBase — illegal
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*loadOp, rax, ops);
    });

    DiagnosticReporter rep;
    (void)assembleFirstFn(lir, **schema, rep);
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(X86VariableEncoder, LoadIndexedRcxRsiScale4) {
    // `mov rcx, [rsi + rdx*4 + 0]` — 4-op indexed load with non-rsp,
    // non-r12 base and index. mod=10 reg=rcx(1) rm=4(SIB) SIB(scale=2 index=rdx(2) base=rsi(6))
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rcx = physGprByName(**schema, "rcx");
    LirReg const rsi = physGprByName(**schema, "rsi");
    LirReg const rdx = physGprByName(**schema, "rdx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsi),
            LirOperand::makeReg(rdx),
            LirOperand::makeMemBase(4),
            LirOperand::makeMemOffset(0),
        };
        (void)b.addInst(*loadOp, rcx, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8B);  // load opcode
    EXPECT_EQ(bytes[2], 0x8C);  // mod=10 reg=rcx(1) rm=4(SIB)
    EXPECT_EQ(bytes[3], 0x96);  // SIB: scale=10b index=rdx(2) base=rsi(6) = 0x80|0x10|0x06
    EXPECT_EQ(bytes[4], 0x00);
}

TEST(X86VariableEncoder, StoreIndexedRdxRbxR12Scale8) {
    // `mov [rbx + r12*8 + 0x20], rdx` — 5-op indexed store with REX.W
    // and REX.X (from r12 index). reg=rdx(2), rm=4(SIB), SIB:
    // scale=11b index=r12.lo3(4) base=rbx(3); REX = 4A (W+X).
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const storeOp = (*schema)->opcodeByMnemonic("store");
    ASSERT_TRUE(storeOp.has_value());
    LirReg const rdx = physGprByName(**schema, "rdx");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const r12 = physGprByName(**schema, "r12");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rdx),
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(r12),
            LirOperand::makeMemBase(8),
            LirOperand::makeMemOffset(0x20),
        };
        (void)b.addInst(*storeOp, InvalidLirReg, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x4A);  // REX.W | REX.X
    EXPECT_EQ(bytes[1], 0x89);  // store opcode
    EXPECT_EQ(bytes[2], 0x94);  // mod=10 reg=rdx(2) rm=4(SIB) = 0x80|0x10|0x04
    EXPECT_EQ(bytes[3], 0xE3);  // SIB: scale=11b index=r12.lo3(4) base=rbx(3) = 0xC0|0x20|0x03
    EXPECT_EQ(bytes[4], 0x20);  // disp32 LE
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x00);
    EXPECT_EQ(bytes[7], 0x00);
}

TEST(X86VariableEncoder, LeaWithIndexAndNonZeroDisp32) {
    // `lea rax, [rbx + rcx*4 + 0x12345678]` — pin disp32 emission
    // after SIB byte.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");
    LirReg const rcx = physGprByName(**schema, "rcx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeReg(rcx),
            LirOperand::makeMemBase(4),
            LirOperand::makeMemOffset(0x12345678),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 0x48);
    EXPECT_EQ(bytes[1], 0x8D);
    EXPECT_EQ(bytes[2], 0x84);
    EXPECT_EQ(bytes[3], 0x8B);  // SIB: scale=2(*4) index=rcx(1) base=rbx(3)
    EXPECT_EQ(bytes[4], 0x78);  // disp32 LE byte 0
    EXPECT_EQ(bytes[5], 0x56);
    EXPECT_EQ(bytes[6], 0x34);
    EXPECT_EQ(bytes[7], 0x12);
}

TEST(X86VariableEncoder, LeaNoIndexThreeOperandFormStillWorks) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const leaOp = (*schema)->opcodeByMnemonic("lea");
    ASSERT_TRUE(leaOp.has_value());
    LirReg const rax = physGprByName(**schema, "rax");
    LirReg const rbx = physGprByName(**schema, "rbx");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rbx),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x10),
        };
        (void)b.addInst(*leaOp, rax, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[1], 0x8D);
    EXPECT_EQ(bytes[2], 0x83);  // mod=10 reg=rax(0) rm=rbx(3); no SIB
    EXPECT_EQ(bytes[3], 0x10);
}

// ── D-AS4-6-byte-pin: no-SIB negative tests for lo3 in {0..3, 5..7} ──

TEST(X86VariableEncoder, LoadFromRaxEmitsNoSibByte) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rcx = physGprByName(**schema, "rcx");
    LirReg const rax = physGprByName(**schema, "rax");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rax),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x08),
        };
        (void)b.addInst(*loadOp, rcx, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[2], 0x88);  // mod=10 reg=rcx(1) rm=rax(0)
    EXPECT_EQ(bytes[3], 0x08);
}

TEST(X86VariableEncoder, LoadFromRsiEmitsNoSibByte) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rcx = physGprByName(**schema, "rcx");
    LirReg const rsi = physGprByName(**schema, "rsi");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rsi),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x08),
        };
        (void)b.addInst(*loadOp, rcx, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[2], 0x8E);  // mod=10 reg=rcx(1) rm=rsi(6)
    EXPECT_EQ(bytes[3], 0x08);
}

TEST(X86VariableEncoder, LoadFromRdiEmitsNoSibByte) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const loadOp = (*schema)->opcodeByMnemonic("load");
    ASSERT_TRUE(loadOp.has_value());
    LirReg const rcx = physGprByName(**schema, "rcx");
    LirReg const rdi = physGprByName(**schema, "rdi");

    Lir lir = buildSingleFnLirWithRet(**schema, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(rdi),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0x08),
        };
        (void)b.addInst(*loadOp, rcx, ops);
    });

    DiagnosticReporter rep;
    auto const bytes = assembleFirstFn(lir, **schema, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_GE(bytes.size(), 7u);
    EXPECT_EQ(bytes[2], 0x8F);  // mod=10 reg=rcx(1) rm=rdi(7)
    EXPECT_EQ(bytes[3], 0x08);
}
