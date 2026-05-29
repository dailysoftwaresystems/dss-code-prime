// x86_64 relocation-emission tests — plan 13 AS4.
//
// Pins:
//   * `call sym` emits 5 bytes (0xE8 00 00 00 00) + one Relocation
//     {offset=1, kind=rel32, target=sym}.
//   * The 4 displacement bytes are zero (linker patches at link time).
//   * Reloc offset = byte position of the placeholder (1 past the
//     0xE8 opcode byte).

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using namespace dss;

TEST(X86Relocations, CallSymEmitsRel32Reloc) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const callOp = (*schema)->opcodeByMnemonic("call");
    auto const retOp  = (*schema)->opcodeByMnemonic("ret");
    ASSERT_TRUE(callOp.has_value() && retOp.has_value());

    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    LirOperand const ops[] = { LirOperand::makeSymbolRef(99) };
    (void)b.addInst(*callOp, LirReg{}, ops);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();

    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    ASSERT_EQ(result.functions.size(), 1u);
    auto const& bytes = result.functions[0].bytes;
    auto const& relocs = result.functions[0].relocations;

    EXPECT_EQ(rep.errorCount(), 0u);
    // call sym = E8 00 00 00 00 ; then ret = C3 → total 6 bytes.
    ASSERT_EQ(bytes.size(), 6u);
    EXPECT_EQ(bytes[0], 0xE8);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x00);
    EXPECT_EQ(bytes[4], 0x00);
    EXPECT_EQ(bytes[5], 0xC3);

    ASSERT_EQ(relocs.size(), 1u);
    // The relocation patches bytes [1..4], so its offset is 1.
    EXPECT_EQ(relocs[0].offset, 1u);
    EXPECT_EQ(relocs[0].target, SymbolId{99});
    auto const rel32 = (*schema)->relocationByName("rel32");
    ASSERT_NE(rel32, nullptr);
    EXPECT_EQ(relocs[0].kind, rel32->kind);
    EXPECT_EQ(relocs[0].addend, 0);
}

// ── validate() rule pinning ────────────────────────────────────────

TEST(X86Relocations, RelocationKindRequiredOnSymbolBearingSlot) {
    // A wire targeting Disp32 WITHOUT a relocationKind must fail
    // at schema-load time (convergence-fix rule, target_schema.cpp).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "call", "result": "none",
              "hasSideEffects": true,
              "isCall": true,
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["symbol"] },
                    "template": { "opcode": [232] },
                    "wires": [
                      { "index": 0, "slotKind": "disp32" }
                    ]
                  }
                ]
              } }
        ],
        "relocations": [
            { "name": "rel32", "kind": 1, "formula": "S + A - P - 4" }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value())
        << "wire to disp32 missing relocationKind must be rejected";
}

TEST(X86Relocations, RelocationKindForbiddenOnNonSymbolSlot) {
    // A wire targeting a non-symbol slot (modrm.rm) with
    // relocationKind must fail — it would be dead data, or worse a
    // misleading hint that a reloc would be emitted.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "mov", "result": "value",
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg"] },
                    "template": { "rexW": true, "opcode": [139] },
                    "resultSlot": "modrm.reg",
                    "wires": [
                      { "index": 0, "slotKind": "modrm.rm",
                        "relocationKind": "rel32" }
                    ]
                  }
                ]
              } }
        ],
        "relocations": [
            { "name": "rel32", "kind": 1 }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value())
        << "relocationKind on non-symbol slot must be rejected";
}

TEST(X86Relocations, RelocationKindUnresolvedNameRejected) {
    // Wire references a relocationKind name that doesn't appear in
    // the schema's `relocations[]` rows. Loader fails with
    // C_MalformedJson at the wire's path.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "call", "result": "none",
              "hasSideEffects": true,
              "isCall": true,
              "minOperands": 1, "maxOperands": 1,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["symbol"] },
                    "template": { "opcode": [232] },
                    "wires": [
                      { "index": 0, "slotKind": "disp32",
                        "relocationKind": "rel999_unknown" }
                    ]
                  }
                ]
              } }
        ],
        "relocations": [
            { "name": "rel32", "kind": 1 }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value())
        << "unknown relocationKind name must be rejected at load";
}

TEST(X86Relocations, IsCallWithResultSlotIsRejected) {
    // `isCall: true` opcode declaring a resultSlot is a contradiction
    // — the call's return value lives in the callconv return reg,
    // not in an encoding slot (convergence-fix C / silent-failure F1).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "bogus", "result": "value",
              "hasSideEffects": true,
              "isCall": true,
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
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value());
}

TEST(X86Relocations, Requires2AddressWithoutWireOnOperand0IsRejected) {
    // Rule G's `requires2Address` exception requires a wire on
    // operand 0 targeting a destination-bearing slot. A
    // `requires2Address: true` opcode whose wires omit operand 0
    // would silently let the destination drop — validate() must
    // reject.
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "mov", "result": "value",
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
            { "mnemonic": "bogus", "result": "value",
              "requires2Address": true,
              "minOperands": 2, "maxOperands": 2,
              "encoding": {
                "format": "x86-variable",
                "variants": [
                  { "guard": { "operandKinds": ["reg", "reg"] },
                    "template": { "rexW": true, "opcode": [1] },
                    "wires": [
                      { "index": 1, "slotKind": "modrm.reg" }
                    ]
                  }
                ]
              } }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value())
        << "requires2Address opcode whose wires omit operand 0 must "
           "be rejected — destination would silently drop";
}

TEST(X86Relocations, IsCallWithoutSideEffectsIsRejected) {
    // `isCall: true` requires `hasSideEffects: true` (regalloc relies
    // on the side-effect bit for call-boundary liveness; a
    // side-effect-free call is a contradiction).
    constexpr char const* kJson = R"({
        "dssTargetVersion": 1,
        "target": { "name": "synth", "version": "0.1" },
        "opcodes": [
            { "mnemonic": "invalid", "result": "none" },
            { "mnemonic": "bogus", "result": "none",
              "hasSideEffects": false,
              "isCall": true,
              "minOperands": 1, "maxOperands": 1 }
        ]
    })";
    EXPECT_FALSE(TargetSchema::loadFromText(kJson, "synth.target.json").has_value());
}

TEST(X86Relocations, RelocationsArePopulatedOnly_WhenSymbolRefOperandPresent) {
    // Sanity: a function with NO call (just ret) emits zero relocs.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const retOp = (*schema)->opcodeByMnemonic("ret");
    LirBuilder b{**schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    (void)b.addReturn(*retOp, {});
    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount());
    DiagnosticReporter rep;
    auto result = assemble(lir, **schema, lirToMir, rep);
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(result.functions.size(), 1u);
    EXPECT_TRUE(result.functions[0].relocations.empty());
}
