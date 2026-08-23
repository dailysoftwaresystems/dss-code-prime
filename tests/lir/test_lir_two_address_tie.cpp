// THE TWO-ADDRESS TIE IS AN OPERAND **INDEX**, NOT A BOOL.
// D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE.
//
// ★★★ WHAT CHANGED AND WHY IT IS A CORE FIX. `requires2Address` was a
// per-opcode bool, and the operand it tied the result to was the literal `0`
// written out at FOUR sites: three in `lir_2addr_legalize.cpp` (the shape
// diagnostic, the needs-legalize test, and the `mov` insertion + rewrite) and
// one in `lir_regalloc.cpp` (the exclusion loop that must skip the legitimate
// coalesce target). No `(result, operand j)` pair was expressible anywhere in
// the pipeline, which is what made a read-write asm operand unrepresentable.
// ★ The fix belongs in the CORE precisely because it is not about asm: EVERY
// two-address target gains the ability to declare a tie its ISA really has,
// and an asm-private tie would have been the construct-private verb set the
// bar forbids.
//
// ★★ THE DEFAULT ARM IS PINNED AS HARD AS THE NEW ONE. Every shipped opcode
// spells `requires2Address: true` and means operand 0; a change that made the
// new index work while quietly moving the default would miscompile x86's whole
// reg-reg ALU with no diagnostic. So each claim below is a PAIR: the shipped
// operand-0 behaviour, and the mutated non-zero tie, through the SAME real
// legalizer.
//
// ⚠ CONFIG-LEVEL: `dss_add_test` sets `DSS_CONFIG_ROOT`, so this file must run
// through ctest and never as a bare `.exe`
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_2addr_legalize.hpp"
#include "lir/lir_liveness.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_regalloc.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;
using namespace dss::test_support;

namespace {

// The shipped x86_64 `xor`: `requires2Address: true`, two LIR operands, and —
// ✔MEASURED over every two-address opcode in the document — one of only four
// (`mul`, `and`, `or`, `xor`) whose EVERY encoding variant guards
// `operandKinds: [reg, reg]`. That matters: `add`/`sub` also declare
// `[reg, imm32]` variants, so a tie aimed at operand 1 is genuinely invalid
// for them and the loader rejects it (correctly — the pass cannot copy from an
// immediate). Picking a reg-reg-only opcode is what makes the mutant a
// COHERENT declaration rather than a broken one.
constexpr std::string_view kTiedMnemonic = "xor";

// Point `xor`'s two-address tie at operand 1 instead of the default 0.
//
// ★★ THE WIRES MOVE WITH THE TIE, AND THAT IS NOT BOOKKEEPING — IT IS THE
// SECOND HALF OF THE CLAIM. `validate()` rule G accepts a two-address opcode
// with no `resultSlot` only when the TIED operand carries a destination-bearing
// wire, because after legalize that operand IS the destination. Shipped `xor`
// wires operand 0 → `modrm.rm` (a destination slot) and operand 1 →
// `modrm.reg`. Re-aiming the tie without swapping them would be refused at
// load — which is the rule working. Swapped, the row describes a real
// instruction: `XOR r64, r/m64` (0x33 /r), the same operation encoded in the
// other direction, so the mutant is architecturally honest rather than a
// schema that merely happens to validate.
//
// ⚠ `mutateShippedTargetSchemaDoc` THROWS if the lambda leaves the document
// byte-identical, so a navigator that missed the row cannot yield a "mutant"
// that is the shipped schema (D-TEST-SCHEMA-MUTATION-HELPER-FAILS-OPEN).
[[nodiscard]] std::shared_ptr<TargetSchema> schemaTiedToOperand(unsigned index) {
    auto r = mutateShippedTargetSchemaDoc("x86_64", [&](nlohmann::json& doc) {
        for (auto& op : doc.at("opcodes")) {
            if (!op.is_object()) continue;
            if (op.value("mnemonic", std::string{}) != kTiedMnemonic) continue;
            op["twoAddressSourceOperand"] = index;
            for (auto& v : op.at("encoding").at("variants")) {
                for (auto& w : v.at("wires")) {
                    w["slotKind"] = (w.at("index").get<unsigned>() == index)
                                        ? "modrm.rm" : "modrm.reg";
                }
                v.at("template").at("opcode") = nlohmann::json::array({0x33});
            }
        }
    });
    if (!r.has_value()) {
        std::string why;
        for (auto const& e : r.error()) why += e.path + ": " + e.message + "\n";
        throw std::runtime_error{"mutated x86_64 did not load: " + why};
    }
    return *r;
}

[[nodiscard]] std::shared_ptr<TargetSchema> shippedX86() {
    auto r = TargetSchema::loadShipped("x86_64");
    if (!r.has_value()) throw std::runtime_error{"cannot load shipped x86_64"};
    return *r;
}

[[nodiscard]] std::uint16_t op(TargetSchema const& t, std::string_view m) {
    auto const i = t.opcodeByMnemonic(m);
    if (!i.has_value()) throw std::runtime_error{"no such opcode"};
    return *i;
}

// `f() { a = mov #1 ; b = mov #2 ; r = xor a, b ; ret r }` — the shape the
// legalizer exists for: `r` differs from BOTH operands, so whichever operand
// the schema ties must be copied into `r` first, and the other must be left
// alone. Using two DISTINCT seeded operands is what makes the two arms
// distinguishable at all; `xor a, a` would legalize identically either way.
[[nodiscard]] Lir buildXorOfTwoDistinctValues(TargetSchema const& t) {
    LirBuilder b{t};
    (void)b.addFunction(SymbolId{1});
    LirBlockId const entry = b.createBlock();
    b.beginBlock(entry);
    LirReg const a = b.newVReg(LirRegClass::GPR);
    LirReg const c = b.newVReg(LirRegClass::GPR);
    LirReg const r = b.newVReg(LirRegClass::GPR);
    std::array<LirOperand, 1> const one{LirOperand::makeImmInt32(1)};
    std::array<LirOperand, 1> const two{LirOperand::makeImmInt32(2)};
    (void)b.addInst(op(t, "mov"), a, one);
    (void)b.addInst(op(t, "mov"), c, two);
    std::array<LirOperand, 2> const xorOps{LirOperand::makeReg(a),
                                           LirOperand::makeReg(c)};
    (void)b.addInst(op(t, kTiedMnemonic), r, xorOps);
    std::array<LirOperand, 1> const ret{LirOperand::makeReg(r)};
    (void)b.addReturn(op(t, "ret"), ret);
    return std::move(b).finish();
}

// What the legalizer produced for the tied op: the register the synthesized
// `mov` copied FROM, and the op's operands afterwards.
struct Legalized {
    bool                ok = false;
    std::vector<LirReg> tiedOpOperands;
    LirReg              tiedOpResult{InvalidLirReg};
    LirReg              copySource{InvalidLirReg};
    std::uint32_t       instCount = 0;
};

[[nodiscard]] Legalized legalize(TargetSchema const& t) {
    Lir const src = buildXorOfTwoDistinctValues(t);
    DiagnosticReporter rep;
    auto result = legalizeTwoAddress(src, t, rep);
    Legalized out;
    out.ok = result.ok();
    if (!out.ok) return out;

    Lir const& lir = result.lir;
    LirBlockId const bb = lir.funcBlockAt(lir.funcAt(0), 0);
    out.instCount = lir.blockInstCount(bb);
    std::uint16_t const tiedOp = op(t, kTiedMnemonic);
    for (std::uint32_t i = 0; i < out.instCount; ++i) {
        LirInstId const inst = lir.blockInstAt(bb, i);
        if (lir.instOpcode(inst) != tiedOp) continue;
        out.tiedOpResult = lir.instResult(inst);
        for (auto const& o : lir.instOperands(inst)) out.tiedOpOperands.push_back(o.reg);
        // The implicit copy is the instruction IMMEDIATELY BEFORE the tied op
        // — the legalizer emits it there and nowhere else.
        if (i > 0) {
            LirInstId const prev = lir.blockInstAt(bb, i - 1);
            auto const prevOps = lir.instOperands(prev);
            if (lir.instOpcode(prev) == op(t, "mov") && prevOps.size() == 1
                && prevOps[0].kind == LirOperandKind::Reg
                && lir.instResult(prev) == out.tiedOpResult) {
                out.copySource = prevOps[0].reg;
            }
        }
        break;
    }
    return out;
}

} // namespace

// ── the DEFAULT arm: `requires2Address: true` still means operand 0 ──
TEST(LirTwoAddressTie, TheShippedSchemaStillTiesTheResultToOperandZero) {
    auto const t = shippedX86();
    ASSERT_TRUE(t->opcodeInfo(op(*t, kTiedMnemonic))->requires2Address.has_value())
        << "the shipped `xor` must declare a two-address tie or this whole "
           "file is asking its question of the wrong opcode";
    EXPECT_EQ(*t->opcodeInfo(op(*t, kTiedMnemonic))->requires2Address, 0u)
        << "`requires2Address: true` with no index must keep meaning operand 0";

    Legalized const g = legalize(*t);
    ASSERT_TRUE(g.ok);
    ASSERT_EQ(g.tiedOpOperands.size(), 2u);
    // Operand 0 was rewritten to the result; operand 1 was left alone.
    EXPECT_EQ(g.tiedOpOperands[0], g.tiedOpResult)
        << "the TIED operand must read the result register after legalize";
    EXPECT_NE(g.tiedOpOperands[1], g.tiedOpResult)
        << "the untied operand must be left exactly as it was";
    // And the copy took its value from the ORIGINAL operand 0.
    EXPECT_TRUE(g.copySource.valid())
        << "no implicit `mov result, operands[tied]` was inserted";
    EXPECT_EQ(g.copySource.id, 1u)
        << "the copy must read the FIRST seeded vreg (operand 0's value)";
}

// ── the NEW arm: the tie is re-aimed at operand 1 and the pass follows ──
//
// ★★★ THIS IS THE CLAIM. Same module, same pass, same target document apart
// from one key: the synthesized copy must now read operand 1's value, operand
// 1 must be the one rewritten to the result, and operand 0 must be untouched.
// Every one of those three is the MIRROR of the default arm above — if the
// literal `0` had survived at any of the three legalize sites, at least one
// would still show the operand-0 answer.
TEST(LirTwoAddressTie, ANonZeroTieIsHonouredByTheRealLegalizer) {
    auto const t = schemaTiedToOperand(1);
    ASSERT_TRUE(t->opcodeInfo(op(*t, kTiedMnemonic))->requires2Address.has_value());
    EXPECT_EQ(*t->opcodeInfo(op(*t, kTiedMnemonic))->requires2Address, 1u);

    Legalized const g = legalize(*t);
    ASSERT_TRUE(g.ok);
    ASSERT_EQ(g.tiedOpOperands.size(), 2u);
    EXPECT_EQ(g.tiedOpOperands[1], g.tiedOpResult)
        << "operand 1 is the tied one now and must read the result register";
    EXPECT_NE(g.tiedOpOperands[0], g.tiedOpResult)
        << "operand 0 is no longer tied and must be left exactly as it was — "
           "a surviving literal 0 in the legalizer would rewrite it";
    EXPECT_TRUE(g.copySource.valid())
        << "no implicit `mov result, operands[1]` was inserted";
    EXPECT_EQ(g.copySource.id, 2u)
        << "the copy must read the SECOND seeded vreg (operand 1's value) — "
           "copying operand 0's value would compute `a + a`, a silent "
           "miscompile the shape of this module is built to expose";
}

// ── THE FOURTH SITE: the register allocator's exclusion loop ─────────
//
// ★★★ THIS IS THE CORRECTNESS HALF, AND IT IS NOT SYMMETRIC WITH THE
// LEGALIZER'S. `legalizeTwoAddress` runs AFTER register allocation and inserts
// `mov result, operands[tied]` BEFORE the op. So if `result` was allocated to
// the same physical register as an UNTIED operand, that operand's value is
// destroyed by the copy before the op ever reads it — a silent miscompile.
// The allocator prevents it by excluding every untied operand's ordinal from
// the result's candidates, and by deliberately NOT excluding the tied one
// (sharing there is the legitimate coalesce). Both halves are keyed on the
// index, so under a non-zero tie the exclusion must INVERT.
//
// ⚠ THE PAIR IS A FULL INVERSION, WHICH IS WHY IT DISCRIMINATES. Under the
// shipped tie (operand 0) the result takes operand 0's register and avoids
// operand 1's; under the mutated tie (operand 1) it must do exactly the
// opposite. A test asserting only the avoidance would pass on an allocator
// that excluded BOTH operands — correct but pessimal, and indistinguishable
// from the real thing without the sharing half.
namespace {

struct AllocatedOrdinals {
    std::uint16_t op0 = 0;   // the first seeded vreg  (LIR operand 0)
    std::uint16_t op1 = 0;   // the second seeded vreg (LIR operand 1)
    std::uint16_t res = 0;   // the tied op's result
};

[[nodiscard]] std::uint16_t physOf(LirAllocation const& a, std::uint32_t vreg) {
    auto const* r = a.perFunc.at(0).forVReg(vreg);
    if (r == nullptr || r->isSpilled()) {
        throw std::runtime_error{"vreg " + std::to_string(vreg) + " spilled — "
                                 "the comparison is only meaningful between "
                                 "register-resident values"};
    }
    return static_cast<std::uint16_t>(r->physReg().id);
}

[[nodiscard]] AllocatedOrdinals allocateFor(TargetSchema const& t) {
    // `buildXorOfTwoDistinctValues` mints them in this order, so the vreg ids
    // are 1, 2, 3. Not assumed silently: the two legalize tests above read the
    // SAME module and pin `copySource.id` to 1 under the shipped tie and to 2
    // under the moved one, so "vreg 1 is operand 0's value and vreg 2 is
    // operand 1's" is an assertion this file already makes twice. A renumbering
    // reddens those before it can quietly mislead this one.
    Lir const src = buildXorOfTwoDistinctValues(t);
    DiagnosticReporter  rep;
    LirLiveness const   lv = analyzeLiveness(src);
    LirAllocation const alloc =
        allocateRegisters(src, t, lv, /*ccIndex=*/0, rep);
    if (!alloc.ok()) throw std::runtime_error{"register allocation failed"};
    return AllocatedOrdinals{physOf(alloc, 1), physOf(alloc, 2),
                             physOf(alloc, 3)};
}

} // namespace

TEST(LirTwoAddressTie, TheAllocatorExcludesTheUNTIEDOperandFromTheResult) {
    AllocatedOrdinals const shippedTie = allocateFor(*shippedX86());
    EXPECT_NE(shippedTie.res, shippedTie.op1)
        << "the result shares a register with operand 1, which the tie does "
           "NOT name — legalize's `mov result, operands[0]` would destroy "
           "operand 1's value before `xor` reads it";
    EXPECT_EQ(shippedTie.res, shippedTie.op0)
        << "the result did not coalesce with the TIED operand. The avoidance "
           "assertion above proves nothing unless the allocator is otherwise "
           "willing to share: an allocator that excluded both operands would "
           "satisfy it while pessimising every two-address op in the tree";

    AllocatedOrdinals const movedTie = allocateFor(*schemaTiedToOperand(1));
    EXPECT_NE(movedTie.res, movedTie.op0)
        << "the tie names operand 1 now, so operand 0 is the one that must be "
           "excluded — a surviving literal 0 in the allocator's exclusion loop "
           "leaves it free and legalize then clobbers it";
    EXPECT_EQ(movedTie.res, movedTie.op1)
        << "the result must be free to coalesce with the operand the tie NOW "
           "names";
}

// ── the loader's guards on the new key ───────────────────────────────
//
// An index with no tie declared would be read by nothing, so the operand the
// author meant to tie would silently stay free. Refused at LOAD.
TEST(LirTwoAddressTie, AnIndexWithoutTheFlagIsRefusedAtLoad) {
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        for (auto& o : doc.at("opcodes")) {
            if (!o.is_object()) continue;
            // `mov` is deliberately NOT two-address in the shipped schema.
            if (o.value("mnemonic", std::string{}) != "mov") continue;
            o["twoAddressSourceOperand"] = 1u;
            break;
        }
    });
    ASSERT_FALSE(r.has_value())
        << "an index with no `requires2Address: true` loaded clean";
    std::string why;
    for (auto const& e : r.error()) why += e.message + "\n";
    EXPECT_NE(why.find("requires2Address"), std::string::npos) << why;
}

// An index past the opcode's own arity names an operand that cannot exist, so
// the legalize pass would have nothing to copy from. Refused at LOAD rather
// than surfacing as a per-instruction diagnostic on every `add` in the program.
TEST(LirTwoAddressTie, AnOutOfRangeIndexIsRefusedAtLoad) {
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        for (auto& o : doc.at("opcodes")) {
            if (!o.is_object()) continue;
            if (o.value("mnemonic", std::string{}) != kTiedMnemonic) continue;
            o["twoAddressSourceOperand"] = 9u;
        }
    });
    ASSERT_FALSE(r.has_value())
        << "a tie naming operand 9 of a 2-operand opcode loaded clean";
    std::string why;
    for (auto const& e : r.error()) why += e.message + "\n";
    EXPECT_NE(why.find("maxOperands"), std::string::npos) << why;
}

// The key must be an INTEGER. A boolean here is the mistake the two-key design
// exists to make impossible to misread — `twoAddressSourceOperand: true` must
// not quietly resolve to "operand 1".
TEST(LirTwoAddressTie, ANonIntegerIndexIsRefusedAtLoad) {
    auto r = mutateShippedTargetSchemaDoc("x86_64", [](nlohmann::json& doc) {
        for (auto& o : doc.at("opcodes")) {
            if (!o.is_object()) continue;
            if (o.value("mnemonic", std::string{}) != kTiedMnemonic) continue;
            o["twoAddressSourceOperand"] = true;
        }
    });
    ASSERT_FALSE(r.has_value())
        << "`twoAddressSourceOperand: true` loaded clean — a boolean must not "
           "be read as an operand index";
    std::string why;
    for (auto const& e : r.error()) why += e.message + "\n";
    EXPECT_NE(why.find("twoAddressSourceOperand"), std::string::npos) << why;
}
