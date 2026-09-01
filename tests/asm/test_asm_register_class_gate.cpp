// ── THE ENCODER'S REGISTER-CLASS GATE ────────────────────────────────────
// D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD.
//
// ★★★ THE DEFECT THESE PINS EXIST FOR, in its original form. The mixed-class
// argument miscompile (`D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP`, audit
// F8) disassembled to `cvttss2si %xmm15`: a GPR ordinal — `r15` — had reached
// the ModR/M.rm field of an instruction whose rm field is an XMM field, and the
// encoder wrote 15 into it without a word. Fifteen is a legal value for that
// 4-bit field, so the bytes were VALID and named a DIFFERENT PHYSICAL REGISTER
// than the LIR meant. The disassembler round-tripped them, the linker patched
// nothing, and the program computed with whatever `%xmm15` held. No later stage
// could have noticed.
//
// The gate compares the LIR operand's register class against the bank the
// TARGET CONFIG declares for the destination field. These pins prove it on the
// SHIPPED documents (never a synthetic one — the anchor is about the real
// instructions), in both directions, on both encoding shapes, and prove it is
// read from CONFIG rather than compiled in by MUTATING the declaration and
// watching the verdict follow.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir.hpp"
#include "lir/lir_reg.hpp"
#include "mutate_target_schema.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] LirReg physReg(TargetSchema const& s, char const* name,
                             LirRegClass cls) {
    auto const ord = s.registerByName(name);
    EXPECT_TRUE(ord.has_value()) << name;
    return makePhysicalReg(static_cast<std::uint32_t>(ord.value_or(0)), cls);
}

// Assemble a one-block function and return BOTH the bytes and whether the
// assembler reported an error. A refusal must be LOUD, so the pins assert on
// the diagnostic, never merely on an empty byte vector.
struct EncodeOutcome {
    std::vector<std::uint8_t> bytes;
    std::size_t               errors = 0;
    std::string               joined;   // every diagnostic's text, concatenated
};

template <typename Emit>
[[nodiscard]] EncodeOutcome encodeOne(TargetSchema const& schema, Emit emit) {
    LirBuilder b{schema};
    (void)b.addFunction(SymbolId{1});
    auto blk = b.createBlock();
    b.beginBlock(blk);
    emit(b);
    (void)b.addReturn(*schema.opcodeByMnemonic("ret"), {});
    Lir lir = std::move(b).finish();

    DiagnosticReporter rep;
    std::vector<MirInstId> lirToMir(lir.instCount());
    auto r = assemble(lir, schema, lirToMir, rep);
    EncodeOutcome out;
    out.errors = rep.errorCount();
    for (auto const& d : rep.all()) out.joined += d.actual;
    if (!r.functions.empty()) out.bytes = r.functions[0].bytes;
    return out;
}

// Every refusal from this gate carries the anchor id, the field name and both
// class spellings — a message that names only "no matching variant" would send
// the reader hunting for a guard that matched perfectly well.
void expectNamesTheClassMismatch(EncodeOutcome const& o, char const* field,
                                 char const* wantBank, char const* gotBank) {
    EXPECT_NE(o.joined.find("D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD"),
              std::string::npos)
        << "the diagnostic must name the anchor: " << o.joined;
    EXPECT_NE(o.joined.find(field), std::string::npos)
        << "the diagnostic must name the FIELD (" << field << "): " << o.joined;
    EXPECT_NE(o.joined.find(std::string{"'"} + wantBank + "' bank"),
              std::string::npos)
        << "the diagnostic must name the EXPECTED bank (" << wantBank
        << "): " << o.joined;
    EXPECT_NE(o.joined.find(std::string{"'"} + gotBank + "'-class"),
              std::string::npos)
        << "the diagnostic must name the ACTUAL class (" << gotBank
        << "): " << o.joined;
}

} // namespace

// ── x86-variable shape: the anchor's own instruction ─────────────────────

// THE CONTROL. `cvttsd2si rax, xmm1` with the classes the machine wants —
// GPR destination through ModR/M.reg, XMM source through ModR/M.rm — still
// encodes. A gate that refuses this would be refusing correct input.
TEST(RegisterClassGate, CvttsdsiWithTheDeclaredBanksStillEncodes) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "xmm1", LirRegClass::FPR))};
        (void)b.addInst(*op, physReg(**s, "rax", LirRegClass::GPR), ops);
    });
    EXPECT_EQ(o.errors, 0u) << o.joined;
    EXPECT_GE(o.bytes.size(), 5u);
}

// ★★★ THE PIN THIS ANCHOR EXISTS FOR. The SAME instruction with `r15` — a GPR
// — in the XMM source field. Before the gate this emitted `F2 49 0F 2C C7`,
// i.e. `cvttsd2si %xmm15`, silently. It must now be REFUSED, and the refusal
// must say which field, which bank the field draws from, and which class
// arrived.
TEST(RegisterClassGate, GprOrdinalInAnXmmFieldIsRefusedNotEncoded) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "r15", LirRegClass::GPR))};
        (void)b.addInst(*op, physReg(**s, "rax", LirRegClass::GPR), ops);
    });
    EXPECT_GT(o.errors, 0u)
        << "a GPR ordinal in an XMM operand field must fail loud — this is the "
           "`cvttss2si %xmm15` miscompile";
    expectNamesTheClassMismatch(o, "modrm.rm", "fpr", "gpr");
    EXPECT_TRUE(o.bytes.empty())
        << "the function must be dropped, not emitted with wrong bytes";
}

// The MIRROR direction, so the gate cannot be satisfied by a one-sided rule:
// `cvtsi2sd xmm0, r/m` writes an XMM result through ModR/M.reg. A GPR-class
// result there is the same defect with the operands swapped.
TEST(RegisterClassGate, GprResultInAnXmmResultFieldIsRefused) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("si_to_fp");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "rcx", LirRegClass::GPR))};
        (void)b.addInst(*op, physReg(**s, "rax", LirRegClass::GPR), ops);
    });
    EXPECT_GT(o.errors, 0u);
    expectNamesTheClassMismatch(o, "modrm.reg", "fpr", "gpr");
}

// An FPR operand where the machine wants the ADDRESS register. `movsd_load`'s
// value is XMM but its base is a GPR — a per-FIELD override inside an
// otherwise-FPR opcode, so this pin also proves the override is consulted
// rather than the opcode-wide bank alone.
TEST(RegisterClassGate, XmmOrdinalInAMemoryBaseFieldIsRefused) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("movsd_load");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "xmm2", LirRegClass::FPR)),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0)};
        (void)b.addInst(*op, physReg(**s, "xmm0", LirRegClass::FPR), ops);
    });
    EXPECT_GT(o.errors, 0u)
        << "an XMM ordinal as a memory BASE would address whatever GPR shares "
           "its number";
    expectNamesTheClassMismatch(o, "modrm.rm.mem", "gpr", "fpr");
}

// ── the operand's class tag vs. its own ordinal ──────────────────────────
//
// A `LirReg` carries a class TAG and an ORDINAL, and they can disagree. That
// is a different question from "is this register in the field's bank", and it
// needs its own comparison — ✔MEASURED: with the tag-vs-table arm disabled the
// gate still refused every case below EXCEPT the last one, which is the whole
// reason the two comparisons are TAG-vs-TABLE and TABLE-vs-FIELD rather than
// both against the field.
TEST(RegisterClassGate, ClassTagThatContradictsItsOwnOrdinalIsRefused) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        // `rax`'s ordinal wearing an FPR tag.
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "rax", LirRegClass::FPR))};
        (void)b.addInst(*op, physReg(**s, "rdx", LirRegClass::GPR), ops);
    });
    EXPECT_GT(o.errors, 0u);
    EXPECT_NE(o.joined.find("D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD"),
              std::string::npos) << o.joined;
    EXPECT_NE(o.joined.find("disagree"), std::string::npos)
        << "the message must say the tag and the ordinal disagree: " << o.joined;
}

// ★★ THE CASE ONLY THE TAG-vs-TABLE ARM CAN SEE, and therefore the pin that
// makes that arm load-bearing. `xmm1` reaching `fp_to_si`'s XMM source field is
// the RIGHT register in the RIGHT bank — the table says `fpr`, the field wants
// `fpr` — so the bank comparison is satisfied and says nothing. What is wrong
// is the operand's own tag: it claims `gpr`, which means whatever produced it
// took the ordinal out of the integer pool and landed on an XMM row by
// coincidence. That is the ordinal collision (`rcx` and `xmm1` are both "1" to
// their own files) the mixed-class miscompile was made of, and without this arm
// it encodes silently.
TEST(RegisterClassGate, GprTagOnAnFprOrdinalIsRefusedEvenInAnFprField) {
    auto s = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "xmm1", LirRegClass::GPR))};
        (void)b.addInst(*op, physReg(**s, "rdx", LirRegClass::GPR), ops);
    });
    EXPECT_GT(o.errors, 0u)
        << "the field's bank is satisfied here — only the tag-vs-table "
           "comparison can refuse this, and it must";
    EXPECT_NE(o.joined.find("disagree"), std::string::npos) << o.joined;
    EXPECT_TRUE(o.bytes.empty());
}

// ── fixed32 shape: the SAME gate, no second implementation ───────────────

// arm64's Q-form store is `STUR Qt` — `fstur` at WIDTH 128, one of that row's
// three width variants since R2 of design A′. Its data field draws from `fpr`,
// like every other width of the same row.
//
// ⚠⚠ TWO ARMS STOOD HERE AND BOTH ASSERTED A CLASS THIS TARGET NO LONGER HAS.
// `Arm64QFormStoreRefusesTheNarrowFprView` passed `d0` (class `fpr`) to a
// `fstur_q` whose data field drew from `vr`, and expected the refusal
// `rd: 'vr' bank, 'fpr'-class`. `Arm64QFormStoreAcceptsTheVrView` was its
// control, passing `v0` tagged `LirRegClass::VR`. With arm64's SIMD&FP file
// declared ONCE (R1), `v0` and `d0` are both `fpr`, the Q form's `rd` wire
// banks `fpr`, and there is no `vr` register anywhere — so the first arm's
// refusal cannot fire and the second arm's operand would itself be refused.
//
// ★★★ AND THE GUARANTEE THE FIRST ARM CARRIED IS GENUINELY NOT A CLASS
// QUESTION ANY MORE — STATED PLAINLY RATHER THAN QUIETLY REPLACED. `d0` in a
// width-128 `fstur` now encodes `STUR Q0` and the bytes are CORRECT, because
// `d0` is a declared view of `v0` and the sixteen bytes come from the
// INSTRUCTION's width rather than from the operand's row. The operand and the
// instruction can no longer disagree about how many bytes move, which is what
// D-OPT-LIR-ARG-REGISTER-CLASS-MISMATCH-FAILLOUD was about. What keeps a view
// out of the places that would matter is D-TARGET-CC-NAMES-SUB-REGISTER at
// load time (pinned in `tests/core/test_target_subregister_validation` and in
// `tests/lir/test_lir_aliased_view_allocability`), not this gate.
//
// ⇒ The two arms are replaced by the claim this gate CAN still make about the
// Q form: a GPR in its data field is refused, and the width flag is what
// elects the Q encoding.
TEST(RegisterClassGate, Arm64QFormStoreRefusesAGprDataOperand) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fstur");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "x2", LirRegClass::GPR)),
            LirOperand::makeReg(physReg(**s, "x1", LirRegClass::GPR)),
            LirOperand::makeMemBase(1),
            LirOperand::makeMemOffset(0)};
        (void)b.addInst(*op, InvalidLirReg, ops, /*payload=*/0,
                        kLirInstFlagWidth128);
    });
    EXPECT_GT(o.errors, 0u)
        << "an integer register in the SIMD&FP data field would encode a "
           "DIFFERENT physical register with valid-looking bytes";
    expectNamesTheClassMismatch(o, "rd", "fpr", "gpr");
}

TEST(RegisterClassGate, Arm64QFormStoreAcceptsTheFullFpRegisterAtWidth128) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fstur");
    ASSERT_TRUE(op.has_value());
    auto const emit = [&](std::uint8_t flags) {
        return encodeOne(**s, [&](LirBuilder& b) {
            LirOperand const ops[] = {
                LirOperand::makeReg(physReg(**s, "v0", LirRegClass::FPR)),
                LirOperand::makeReg(physReg(**s, "x1", LirRegClass::GPR)),
                LirOperand::makeMemBase(1),
                LirOperand::makeMemOffset(0)};
            (void)b.addInst(*op, InvalidLirReg, ops, /*payload=*/0, flags);
        });
    };

    auto const q = emit(kLirInstFlagWidth128);
    EXPECT_EQ(q.errors, 0u) << q.joined;
    ASSERT_GE(q.bytes.size(), 4u);
    EXPECT_EQ(q.bytes[3], 0x3C) << "STUR Q — 0x38 would be STURB";

    // ★ THE WIDTH IS WHAT ELECTS THE Q FORM, AND THIS IS THE ARM THAT SAYS SO.
    // The same opcode and the same operands at the width-DEFAULT (64) must
    // encode the D form — a DIFFERENT instruction moving eight bytes. Without
    // this control, an implementation that ignored the width flag entirely
    // would pass the arm above.
    auto const d = emit(0);
    EXPECT_EQ(d.errors, 0u) << d.joined;
    ASSERT_GE(d.bytes.size(), 4u);
    EXPECT_EQ(d.bytes[3], 0xFC) << "STUR Dt at the width default";
    EXPECT_NE(q.bytes[3], d.bytes[3])
        << "the width flag changed nothing — the Q form is not being elected "
           "by width, so a 16-byte access would silently emit an 8-byte store";
}

// A GPR in arm64's FP arithmetic — the fixed32 twin of the x86 pin, so the
// second walker is proved to consult the declaration too rather than
// inheriting the x86 one's behaviour by accident.
TEST(RegisterClassGate, Arm64FaddRefusesAGprSource) {
    auto s = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(s.has_value());
    auto const op = (*s)->opcodeByMnemonic("fadd");
    ASSERT_TRUE(op.has_value());
    auto const o = encodeOne(**s, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**s, "d1", LirRegClass::FPR)),
            LirOperand::makeReg(physReg(**s, "x2", LirRegClass::GPR))};
        (void)b.addInst(*op, physReg(**s, "d0", LirRegClass::FPR), ops);
    });
    EXPECT_GT(o.errors, 0u);
    expectNamesTheClassMismatch(o, "rm", "fpr", "gpr");
}

// ── the gate is CONFIG-DRIVEN, and this is how that is shown ─────────────
//
// ★★ A CONFIG-LEVEL MUTANT, WHICH IS WHY THIS FILE RUNS THROUGH ctest.
// `dss_add_test` sets DSS_CONFIG_ROOT; a bare .exe would walk the cwd and read
// whichever tree the shell happens to stand in
// (D-TEST-CONFIG-RED-ON-DISABLE-READS-THE-WRONG-TREE).
//
// Re-point `fp_to_si`'s XMM source field at the `gpr` bank — a LIE about the
// machine, and exactly the state the schema was in before this anchor closed
// (no declaration at all, nothing to compare against). The verdict must FLIP:
// the GPR operand the gate refuses above becomes acceptable, and the correct
// XMM operand becomes the refused one. A gate with a compiled-in table would
// not move.
TEST(RegisterClassGate, MutatingTheDeclaredBankFlipsTheVerdict) {
    auto mutated = dss::test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto& op : doc.at("opcodes")) {
                if (!op.contains("mnemonic")
                    || op.at("mnemonic") != "fp_to_si") {
                    continue;
                }
                for (auto& v : op.at("encoding").at("variants")) {
                    for (auto& w : v.at("wires")) {
                        if (w.value("slotKind", std::string{}) == "modrm.rm") {
                            w["regClass"] = "gpr";
                        }
                    }
                }
            }
        });
    ASSERT_TRUE(mutated.has_value())
        << "the mutant must LOAD — the point is a changed verdict, not a "
           "rejected document";
    auto const op = (*mutated)->opcodeByMnemonic("fp_to_si");
    ASSERT_TRUE(op.has_value());

    auto const withGpr = encodeOne(**mutated, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**mutated, "r15", LirRegClass::GPR))};
        (void)b.addInst(*op, physReg(**mutated, "rax", LirRegClass::GPR), ops);
    });
    EXPECT_EQ(withGpr.errors, 0u)
        << "under the mutated declaration the GPR operand is what the field "
           "asks for, so it must encode — proving the gate read the CONFIG "
           "and not a table in the encoder: " << withGpr.joined;

    auto const withXmm = encodeOne(**mutated, [&](LirBuilder& b) {
        LirOperand const ops[] = {
            LirOperand::makeReg(physReg(**mutated, "xmm1", LirRegClass::FPR))};
        (void)b.addInst(*op, physReg(**mutated, "rax", LirRegClass::GPR), ops);
    });
    EXPECT_GT(withXmm.errors, 0u)
        << "and the correct-for-the-machine operand becomes the refused one";
    expectNamesTheClassMismatch(withXmm, "modrm.rm", "gpr", "fpr");
}

// ── what the SHIPPED documents must keep saying ─────────────────────────
//
// The declarations above are machine facts read off the Intel SDM / ARM ARM
// operand roles. Pin the three that carry the anchor's own defect so a careless
// edit reds HERE, next to the reason, rather than as a wrong-looking byte in
// some example.
TEST(RegisterClassGate, ShippedDocumentsDeclareTheMixedClassFields) {
    auto x = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(x.has_value());
    auto const* fpToSi = (*x)->opcodeInfo(*(*x)->opcodeByMnemonic("fp_to_si"));
    ASSERT_NE(fpToSi, nullptr);
    ASSERT_FALSE(fpToSi->encoding.variants.empty());
    for (auto const& v : fpToSi->encoding.variants) {
        EXPECT_EQ(encodingResultRegClass(fpToSi->encoding, v),
                  TargetRegClass::GPR)
            << "CVTTSD2SI writes a GPR through ModR/M.reg";
        ASSERT_FALSE(v.wires.empty());
        EXPECT_EQ(encodingWireRegClass(fpToSi->encoding, v.wires[0]),
                  TargetRegClass::FPR)
            << "CVTTSD2SI reads an XMM through ModR/M.rm — the field the "
               "anchor's GPR ordinal reached";
    }

    auto a = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(a.has_value());
    auto const* fstur = (*a)->opcodeInfo(*(*a)->opcodeByMnemonic("fstur"));
    ASSERT_NE(fstur, nullptr);
    ASSERT_GT(fstur->encoding.variants.size(), 1u)
        << "`fstur` must carry its width variants — the Q form is one of them "
           "since R2 of design A′, not a separate mnemonic";
    // ⚠ THIS USED TO READ THE OPCODE `fstur_q` AND EXPECT `TargetRegClass::VR`,
    // with the reason "STUR Qt moves 16 bytes, so its data operand is the
    // 128-bit V view". The bytes moved are still sixteen; what changed is that
    // the width says so instead of a second register class. EVERY width of this
    // row banks the SAME class, and that uniformity is the assertion — a row
    // whose widest arm banked something else would be the double declaration
    // coming back through the encoding table.
    for (auto const& v : fstur->encoding.variants) {
        ASSERT_FALSE(v.wires.empty());
        EXPECT_EQ(encodingWireRegClass(fstur->encoding, v.wires[0]),
                  TargetRegClass::FPR)
            << "every width of the SIMD&FP store draws its data operand from "
               "ONE class; a per-width class is one physical file declared "
               "twice, which is "
               "D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS";
    }
}

// EVERY register-bearing field of BOTH shipped targets resolves to a bank.
// `validate()` refuses a document that leaves one undeclared, so this cannot
// fail while the targets load — which is the point: it states the property in
// one place, so a future weakening of the validator is visible as a green test
// that stopped meaning anything only if this one goes red too.
TEST(RegisterClassGate, EveryRegisterBearingFieldOfEveryShippedTargetHasABank) {
    for (char const* name : {"x86_64", "arm64"}) {
        auto s = TargetSchema::loadShipped(name);
        ASSERT_TRUE(s.has_value()) << name;
        std::size_t checked = 0;
        for (std::size_t i = 0; i < (*s)->opcodeCount(); ++i) {
            auto const* info = (*s)->opcodeInfo(static_cast<std::uint16_t>(i));
            if (info == nullptr) continue;
            for (auto const& v : info->encoding.variants) {
                if (v.resultSlot.has_value()) {
                    EXPECT_TRUE(
                        encodingResultRegClass(info->encoding, v).has_value())
                        << name << " opcode '" << info->mnemonic
                        << "': result field has no declared bank";
                    ++checked;
                }
                for (auto const& w : v.wires) {
                    if (w.index >= v.operandKinds.size()) continue;
                    if (v.operandKinds[w.index] != OperandKindFilter::Reg) {
                        continue;
                    }
                    EXPECT_TRUE(
                        encodingWireRegClass(info->encoding, w).has_value())
                        << name << " opcode '" << info->mnemonic
                        << "': a register operand field has no declared bank";
                    ++checked;
                }
            }
        }
        EXPECT_GT(checked, 100u)
            << name << ": the walk must actually reach fields — a zero count "
                       "would make this pin vacuous";
    }
}
