// Round-trip oracle tests — plan 13 AS5.
//
// Pins:
//   * Each shipped variant on x86_64 (ret / mov reg-reg / mov reg-imm32
//     / add reg-reg / sub reg-reg / mul reg-reg / call sym) round-trips
//     through the disasm oracle without producing A_RoundTripMismatch.
//   * Each shipped variant on arm64 (ret / mov reg-reg / unreachable /
//     add / sub / mul / and / or / xor / bl sym) round-trips.
//   * A regression in either walker's bit packing would surface as a
//     mismatch — even if the byte length stayed the same — closing the
//     silent-failure class architect-review flagged.
//   * Truncated input → A_RoundTripMismatch (defensive guard).
//   * Bytes that don't match any variant → A_RoundTripMismatch.

#include "asm/asm.hpp"
#include "asm/disasm.hpp"
#include "asm_test_support.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;

namespace {

// Bundle: assemble + return both the bytes and the legalized Lir so
// round-trip checks have access to the same inst the encoder saw.
struct AsmBundle {
    Lir                     legalized;
    AssembledModule         result;
    DiagnosticReporter      reporter;
};

// Assemble a one-function Lir built by `emit` (caller closes the
// block with a terminator). Runs through legalize → assemble.
template <typename Emit>
[[nodiscard]] AsmBundle
buildAndAssemble(TargetSchema const& schema, Emit emit) {
    AsmBundle out;
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    Lir src = std::move(b).finish();
    auto legal = legalizeTwoAddress(src, schema, out.reporter);
    if (!legal.ok()) return out;
    out.legalized = std::move(legal.lir);
    std::vector<MirInstId> lirToMir(out.legalized.instCount());
    out.result = assemble(out.legalized, schema, lirToMir, out.reporter);
    return out;
}

// Round-trip every inst in the first function of `bundle.legalized`
// against the encoded bytes of `bundle.result.functions[0]`.
//
// Plan 13 AS6: stride via the encoder-stamped SourceMapEntry table.
// Closes the architect AS5 circular dependency (the prior fixture
// asked the disasm to find where instructions start, then asked the
// disasm to verify those same byte windows — if the disasm returned
// a wrong bytesConsumed for one instruction, the cursor misaligned
// and the failure diagnostic would name the wrong opcode). The
// encoder-stamped boundaries break the cycle.
void assertRoundTripsClean(AsmBundle& bundle, TargetSchema const& schema) {
    ASSERT_TRUE(bundle.result.ok());
    ASSERT_EQ(bundle.result.functions.size(), 1u);
    auto const& fn0    = bundle.result.functions[0];
    auto const& bytes  = fn0.bytes;
    auto const& srcMap = fn0.sourceMap;
    Lir const& lir = bundle.legalized;

    // Build a flat list of LIR insts (in encode order) for parallel
    // walking with srcMap. srcMap is monotonically non-decreasing in
    // byteOffset by construction (encoder appends both the entry and
    // its bytes in one step). Insts whose opcode encoding is `None`
    // are SKIPPED at stamp time, so srcMap and the encoded-inst list
    // should be parallel-indexed.
    std::vector<LirInstId> encodedInsts;
    LirFuncId const lirFn = lir.funcAt(0);
    for (std::uint32_t bi = 0; bi < lir.funcBlockCount(lirFn); ++bi) {
        LirBlockId const blk = lir.funcBlockAt(lirFn, bi);
        for (std::uint32_t ii = 0; ii < lir.blockInstCount(blk); ++ii) {
            LirInstId const inst = lir.blockInstAt(blk, ii);
            auto const opcode = lir.instOpcode(inst);
            auto const* info  = schema.opcodeInfo(opcode);
            if (info == nullptr
                || info->encoding.shape == TargetEncodingShape::None) {
                continue;
            }
            encodedInsts.push_back(inst);
        }
    }

    ASSERT_EQ(srcMap.size(), encodedInsts.size())
        << "srcMap entry count must equal encoded-inst count "
           "(parallel-index discipline)";

    for (std::size_t i = 0; i < srcMap.size(); ++i) {
        std::uint32_t const start = srcMap[i].byteOffset;
        std::uint32_t const end   =
            (i + 1 < srcMap.size())
                ? srcMap[i + 1].byteOffset
                : static_cast<std::uint32_t>(bytes.size());
        ASSERT_GE(end, start)
            << "srcMap byteOffsets must be monotonically non-decreasing";
        std::span<std::uint8_t const> instBytes{
            bytes.data() + start, end - start};
        DiagnosticReporter localRep;
        LirInstId const inst = encodedInsts[i];
        auto const opcode = lir.instOpcode(inst);
        auto const* info  = schema.opcodeInfo(opcode);
        EXPECT_TRUE(roundTripVerify(schema, lir, inst, instBytes, localRep))
            << "round-trip slot mismatch for "
            << (info ? info->mnemonic : std::string_view{"<unknown>"})
            << " at byte " << start;
    }
    // The last entry's slice extends to bytes.size(); the assertion
    // above guarantees we cover every byte in the function.
}

[[nodiscard]] LirReg gpr(TargetSchema const& s, std::string_view name) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value());
    return LirReg{static_cast<std::uint32_t>(ord.value_or(0)), 1,
                  static_cast<std::uint8_t>(LirRegClass::GPR)};
}

} // namespace

// ── x86_64 round-trip — all shipped variants ───────────────────────

TEST(X86RoundTrip, RetRoundTrips) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(X86RoundTrip, MovRegRegRoundTrips) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const movOp = (*schema)->opcodeByMnemonic("mov");
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        LirReg const rax = gpr(**schema, "rax");
        LirReg const r8  = gpr(**schema, "r8");
        LirOperand const ops[] = { LirOperand::makeReg(r8) };
        (void)b.addInst(*movOp, rax, ops);
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(X86RoundTrip, MovRegImm32RoundTrips) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const movOp = (*schema)->opcodeByMnemonic("mov");
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        LirReg const rcx = gpr(**schema, "rcx");
        LirOperand const ops[] = {
            LirOperand::makeImmInt32(static_cast<std::int32_t>(0xDEADBEEF))
        };
        (void)b.addInst(*movOp, rcx, ops);
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(X86RoundTrip, AddSubMulInPlaceAllRoundTrip) {
    // All three binary ops back-to-back. requires2Address + legalize
    // pass should insert no implicit movs (result==op[0]==rax).
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        LirReg const rax = gpr(**schema, "rax");
        LirReg const rcx = gpr(**schema, "rcx");
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        for (auto mnem : { "add", "sub", "mul" }) {
            auto const op = (*schema)->opcodeByMnemonic(mnem);
            ASSERT_TRUE(op.has_value());
            LirOperand const ops[] = { LirOperand::makeReg(rax),
                                        LirOperand::makeReg(rcx) };
            (void)b.addInst(*op, rax, ops);
        }
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(X86RoundTrip, CallSymRoundTrips) {
    // call sym is the AS4-introduced symbol-bearing variant.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const callOp = (*schema)->opcodeByMnemonic("call");
        auto const retOp  = (*schema)->opcodeByMnemonic("ret");
        LirOperand const ops[] = { LirOperand::makeSymbolRef(77) };
        (void)b.addInst(*callOp, LirReg{}, ops);
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

// ── arm64 round-trip — all shipped variants ─────────────────────────

TEST(Arm64RoundTrip, RetUnreachableRoundTrip) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(Arm64RoundTrip, MovRegRegRoundTrips) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const movOp = (*schema)->opcodeByMnemonic("mov");
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        LirReg const x0 = gpr(**schema, "x0");
        LirReg const x5 = gpr(**schema, "x5");
        LirOperand const ops[] = { LirOperand::makeReg(x5) };
        (void)b.addInst(*movOp, x0, ops);
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(Arm64RoundTrip, AllBinaryOpsRoundTrip) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        LirReg const x0 = gpr(**schema, "x0");
        LirReg const x1 = gpr(**schema, "x1");
        LirReg const x2 = gpr(**schema, "x2");
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        for (auto mnem : { "add", "sub", "mul", "and", "or", "xor" }) {
            auto const op = (*schema)->opcodeByMnemonic(mnem);
            ASSERT_TRUE(op.has_value()) << "missing arm64 opcode " << mnem;
            LirOperand const ops[] = { LirOperand::makeReg(x1),
                                        LirOperand::makeReg(x2) };
            (void)b.addInst(*op, x0, ops);
        }
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

TEST(Arm64RoundTrip, BlSymRoundTrips) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto bundle = buildAndAssemble(**schema, [&](LirBuilder& b) {
        auto const blOp  = (*schema)->opcodeByMnemonic("bl");
        auto const retOp = (*schema)->opcodeByMnemonic("ret");
        LirOperand const ops[] = { LirOperand::makeSymbolRef(123) };
        (void)b.addInst(*blOp, LirReg{}, ops);
        (void)b.addReturn(*retOp, {});
    });
    assertRoundTripsClean(bundle, **schema);
}

// ── Defensive paths ────────────────────────────────────────────────

TEST(DisasmDefensive, TruncatedFixed32FailsLoud) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    DiagnosticReporter rep;
    std::array<std::uint8_t, 2> shortBytes{0xC0, 0x03};
    auto result = disassembleInst(**schema, *retOp, shortBytes, rep);
    EXPECT_FALSE(result.has_value());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, X86BytesThatDontMatchAnyVariantFailLoud) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    DiagnosticReporter rep;
    // Bytes that look like NOP (0x90) but the caller claims they're a
    // `mov` — the variant prefix check fails for both `mov` variants.
    std::array<std::uint8_t, 1> bytes{0x90};
    auto result = disassembleInst(**schema, *movOp, bytes, rep);
    EXPECT_FALSE(result.has_value());
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, RoundTripDetectsResultRegMismatch) {
    // LIR says `mov rax, rcx` but bytes encode `mov rcx, rcx`
    // (0x48 0x8B 0xC9 — Modrm reg=rcx instead of rax). Catches the
    // result-slot path that the wire-mismatch test doesn't exercise.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    LirReg const rax = gpr(**schema, "rax");
    LirReg const rcx = gpr(**schema, "rcx");
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(rcx) };
    LirInstId const movInst = b.addInst(*movOp, rax, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    std::array<std::uint8_t, 3> wrongBytes{0x48, 0x8B, 0xC9};  // dest=rcx, src=rcx
    DiagnosticReporter rep;
    EXPECT_FALSE(roundTripVerify(**schema, lir, movInst, wrongBytes, rep));
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, RoundTripDetectsImm32Mismatch) {
    // LIR says `mov rcx, 0xDEADBEEF` but bytes encode imm32 0xCAFEBABE.
    // Catches the Imm32 arm in roundTripVerify.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    LirReg const rcx = gpr(**schema, "rcx");
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = {
        LirOperand::makeImmInt32(static_cast<std::int32_t>(0xDEADBEEF))
    };
    LirInstId const movInst = b.addInst(*movOp, rcx, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    // REX.W 0x48 + opcode 0xC7 + ModRM (mod=3 reg=/0 rm=rcx=1 → 0xC1)
    // + imm32 LE 0xCAFEBABE = BE BA FE CA.
    std::array<std::uint8_t, 7> wrongBytes{
        0x48, 0xC7, 0xC1, 0xBE, 0xBA, 0xFE, 0xCA
    };
    DiagnosticReporter rep;
    EXPECT_FALSE(roundTripVerify(**schema, lir, movInst, wrongBytes, rep));
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, RoundTripDetectsArm64Imm26NonZero) {
    // Convergence-fix A (3-agent: silent-failure + pr-test + architect):
    // arm64 fixed32 disasm now requires Imm26 slot bits to be zero
    // (encoder writes zeros for the linker to patch). A hand-crafted
    // `bl` word with non-zero Imm26 bits must fail round-trip even
    // though it's the right LENGTH and right opcode prefix.
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const blOp = (*schema)->opcodeByMnemonic("bl");
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeSymbolRef(123) };
    LirInstId const blInst = b.addInst(*blOp, LirReg{}, ops);
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    // Correct bl word is 0x94000000; tamper with Imm26 = 0x1.
    // LE bytes of 0x94000001: 01 00 00 94.
    std::array<std::uint8_t, 4> tampered{0x01, 0x00, 0x00, 0x94};
    DiagnosticReporter rep;
    EXPECT_FALSE(roundTripVerify(**schema, lir, blInst, tampered, rep));
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, RoundTripDetectsTrailingBytes) {
    // bytesConsumed != bytes.size() — the defensive guard in
    // roundTripVerify. Pass a valid `ret` plus a stray trailing byte.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirInstId const retInst = b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    std::array<std::uint8_t, 2> tooLong{0xC3, 0x90};
    DiagnosticReporter rep;
    EXPECT_FALSE(roundTripVerify(**schema, lir, retInst, tooLong, rep));
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, RoundTripDetectsRegMismatch) {
    // Hand-construct an encoded buffer where the bytes claim "mov
    // rax ← rcx" but the LIR inst is "mov rax ← rdx" — round-trip
    // must catch the slot disagreement.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const movOp = (*schema)->opcodeByMnemonic("mov");
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    LirReg const rax = gpr(**schema, "rax");
    LirReg const rcx = gpr(**schema, "rcx");
    LirReg const rdx = gpr(**schema, "rdx");

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeReg(rdx) };
    LirInstId const movInst = b.addInst(*movOp, rax, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    // The encoder for `mov rax, rdx` produces 0x48 0x8B 0xC2.
    // We hand-craft `mov rax, rcx` bytes (0x48 0x8B 0xC1) and pass
    // them to the round-trip check against the `mov rax, rdx` inst.
    std::array<std::uint8_t, 3> wrongBytes{0x48, 0x8B, 0xC1};
    DiagnosticReporter rep;
    bool ok = roundTripVerify(**schema, lir, movInst, wrongBytes, rep);
    EXPECT_FALSE(ok) << "round-trip must detect that bytes encode rcx "
                        "while LIR says the source was rdx";
    EXPECT_GT(rep.errorCount(), 0u);
}

TEST(DisasmDefensive, RoundTripDiagnosticPrefixIsA0005) {
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::A_RoundTripMismatch),
              "A0005");
}
