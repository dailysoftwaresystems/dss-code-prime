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
