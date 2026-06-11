// FC3.5 sweep-c2 — MIR FCmp → LIR lowering pins
// (D-COND-FLOAT-NAN-TRUTHINESS-FCMP adjudication + the float arm of
// the width axis).
//
// THE EXHAUSTIVE PREDICATE-MAPPING TABLE lives here: for every
// implemented predicate × every comparison outcome (ordered-lt /
// ordered-eq / ordered-gt / UNORDERED) × both shipped targets, the
// suite extracts the lowering's emitted shape (operand swap, setcc
// condition payload(s), and/or combiner) and EVALUATES it against an
// INDEPENDENTLY re-derived flag simulator:
//   * x86 UCOMISD/UCOMISS truth table (Intel SDM Vol. 2A): unordered
//     → ZF=PF=CF=1; greater → all 0; less → CF=1; equal → ZF=1; the
//     setcc nibbles per SDM Vol. 1 Appendix B.
//   * arm64 FCMP NZCV (ARM ARM C7.2): equal → Z=1,C=1; less → N=1;
//     greater → C=1; unordered → C=1,V=1; the condition nibbles per
//     C1.2.4.
// A wrong nibble, a dropped composition half (e.g. Oeq without the
// ∧-ordered conjunct → NaN==NaN true), a missed swap, or a flipped
// combiner makes a truth-table cell disagree with IEEE — RED.
//
// Red-on-disable levers exercised here:
//   * StripFoeqNibbleFallsBackToComposition — removing arm64's foeq
//     entry must select the UNIVERSAL composed realization (the
//     capability signal is config absence, read at lowering time).
//   * StripFogtNibbleFailsLoud — a required-single predicate with no
//     declared nibble (and no composition) must fail loud naming it.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mutate_target_schema.hpp"
#include "synthetic_fn.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

// ── shape extraction ─────────────────────────────────────────────────

struct FcmpShape {
    bool ok = false;
    bool swapped = false;                    // fcmp(b, a) instead of (a, b)
    std::uint8_t fcmpWidthBits = 0;          // 32 or 64
    std::vector<std::uint32_t> setccPayloads;
    std::optional<std::string> combiner;     // "and" / "or" when composed
};

// Lower `double f(double a, double b) { return FCmp<op>(a, b); }` (or
// the F32 twin) through the REAL lowerToLir and extract the emitted
// float-compare shape.
[[nodiscard]] FcmpShape
lowerFcmpShape(TargetSchema const& schema, MirOpcode pred,
               TypeKind operandKind = TypeKind::F64) {
    FcmpShape out;
    std::array<TypeKind, 2> paramKinds{operandKind, operandKind};
    auto syn = test_support::buildSyntheticFn(
        paramKinds, TypeKind::Bool,
        [&](MirBuilder& mb, TypeInterner& in,
            std::span<TypeId const> params, TypeId) {
            MirInstId const a = mb.addArg(0, params[0]);
            MirInstId const b = mb.addArg(1, params[1]);
            MirInstId const ops[] = {a, b};
            MirInstId const r =
                mb.addInst(pred, ops, in.primitive(TypeKind::Bool));
            mb.addReturn(r);
        });
    DiagnosticReporter rep;
    auto const result = lowerToLir(syn.mir, schema, syn.interner, rep);
    if (!result.ok) return out;
    auto const argOp   = schema.opcodeByMnemonic("arg");
    auto const fcmpOp  = schema.opcodeByMnemonic("fcmp");
    auto const setccOp = schema.opcodeByMnemonic("setcc");
    auto const andOp   = schema.opcodeByMnemonic("and");
    auto const orOp    = schema.opcodeByMnemonic("or");
    if (!fcmpOp || !setccOp || !argOp) return out;
    LirReg argA{}, argB{};
    Lir const& lir = result.lir;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        auto const fn = lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            auto const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(bb); ++ii) {
                auto const id = lir.blockInstAt(bb, ii);
                auto const op = lir.instOpcode(id);
                if (op == *argOp) {
                    if (lir.instPayload(id) == 0) argA = lir.instResult(id);
                    else                          argB = lir.instResult(id);
                } else if (op == *fcmpOp) {
                    auto const ops = lir.instOperands(id);
                    if (ops.size() == 2
                        && ops[0].kind == LirOperandKind::Reg) {
                        out.swapped = (ops[0].reg == argB);
                    }
                    out.fcmpWidthBits = lirInstWidthBits(lir.instFlags(id));
                } else if (op == *setccOp) {
                    out.setccPayloads.push_back(lir.instPayload(id));
                } else if (andOp && op == *andOp) {
                    out.combiner = "and";
                } else if (orOp && op == *orOp) {
                    out.combiner = "or";
                }
            }
        }
    }
    out.ok = true;
    return out;
}

// ── the independent flag simulators ──────────────────────────────────

enum class Outcome : std::uint8_t { Lt, Eq, Gt, Uo };

[[nodiscard]] constexpr Outcome swapOutcome(Outcome o) noexcept {
    // fcmp(b, a): the operands trade places, so less↔greater flip;
    // equal and unordered are symmetric.
    switch (o) {
        case Outcome::Lt: return Outcome::Gt;
        case Outcome::Gt: return Outcome::Lt;
        default:          return o;
    }
}

// Intel SDM "UCOMISD — Unordered Compare Scalar Double Precision":
// the EFLAGS results of comparing op1 (the LIR left) with op2.
struct X86Flags { bool zf, pf, cf; };
[[nodiscard]] constexpr X86Flags x86FlagsFor(Outcome o) noexcept {
    switch (o) {
        case Outcome::Lt: return {false, false, true};
        case Outcome::Eq: return {true,  false, false};
        case Outcome::Gt: return {false, false, false};
        case Outcome::Uo: return {true,  true,  true};
    }
    return {false, false, false};
}
// Intel SDM Vol. 1 Appendix B condition encodings (the tttn nibble of
// SETcc 0F 9x / Jcc 0F 8x) — re-derived independently of the shipped
// condCodeEncoding table so a wrong table value cannot self-certify.
[[nodiscard]] bool evalX86Nibble(std::uint8_t n, X86Flags f) {
    switch (n) {
        case 0x2: return f.cf;                 // B/NAE
        case 0x3: return !f.cf;                // AE/NB
        case 0x4: return f.zf;                 // E/Z
        case 0x5: return !f.zf;                // NE/NZ
        case 0x6: return f.cf || f.zf;         // BE/NA
        case 0x7: return !f.cf && !f.zf;       // A/NBE
        case 0xA: return f.pf;                 // P/PE
        case 0xB: return !f.pf;                // NP/PO
        default:
            ADD_FAILURE() << "unexpected x86 cc nibble " << int(n);
            return false;
    }
}

// ARM ARM C7.2 FCMP NZCV results + C1.2.4 condition encodings.
struct Nzcv { bool n, z, c, v; };
[[nodiscard]] constexpr Nzcv armNzcvFor(Outcome o) noexcept {
    switch (o) {
        case Outcome::Lt: return {true,  false, false, false};
        case Outcome::Eq: return {false, true,  true,  false};
        case Outcome::Gt: return {false, false, true,  false};
        case Outcome::Uo: return {false, false, true,  true};
    }
    return {false, false, false, false};
}
[[nodiscard]] bool evalArm64Nibble(std::uint8_t n, Nzcv f) {
    switch (n) {
        case 0x0: return f.z;                   // EQ
        case 0x1: return !f.z;                  // NE
        case 0x2: return f.c;                   // CS/HS
        case 0x3: return !f.c;                  // CC/LO
        case 0x4: return f.n;                   // MI
        case 0x6: return f.v;                   // VS
        case 0x7: return !f.v;                  // VC
        case 0x8: return f.c && !f.z;           // HI
        case 0x9: return !f.c || f.z;           // LS
        case 0xA: return f.n == f.v;            // GE
        case 0xC: return !f.z && f.n == f.v;    // GT
        default:
            ADD_FAILURE() << "unexpected arm64 cc nibble " << int(n);
            return false;
    }
}

// IEEE 754 / C 6.5.8-6.5.9 expected truth per predicate × outcome.
[[nodiscard]] bool ieeeExpected(MirOpcode pred, Outcome o) {
    bool const lt = (o == Outcome::Lt), eq = (o == Outcome::Eq),
               gt = (o == Outcome::Gt), uo = (o == Outcome::Uo);
    switch (pred) {
        case MirOpcode::FCmpOeq: return eq;
        case MirOpcode::FCmpOne: return lt || gt;
        case MirOpcode::FCmpOlt: return lt;
        case MirOpcode::FCmpOle: return lt || eq;
        case MirOpcode::FCmpOgt: return gt;
        case MirOpcode::FCmpOge: return gt || eq;
        case MirOpcode::FCmpUne: return lt || gt || uo;  // C 6.5.9 `!=`
        default:
            ADD_FAILURE() << "no IEEE expectation for this predicate";
            return false;
    }
}

// Evaluate the EXTRACTED shape against a comparison outcome on the
// given target's flag model.
[[nodiscard]] bool evalShape(TargetSchema const& schema, bool isArm64,
                             FcmpShape const& s, Outcome o) {
    Outcome const eff = s.swapped ? swapOutcome(o) : o;
    auto const evalOne = [&](std::uint32_t payload) -> bool {
        auto const nib = schema.condCodeEncoding(
            static_cast<TargetCondCode>(payload));
        EXPECT_TRUE(nib.has_value())
            << "the lowering emitted cond payload " << payload
            << " with no declared nibble";
        if (!nib.has_value()) return false;
        return isArm64 ? evalArm64Nibble(*nib, armNzcvFor(eff))
                       : evalX86Nibble(*nib, x86FlagsFor(eff));
    };
    if (s.setccPayloads.size() == 1) {
        EXPECT_FALSE(s.combiner.has_value());
        return evalOne(s.setccPayloads[0]);
    }
    EXPECT_EQ(s.setccPayloads.size(), 2u);
    EXPECT_TRUE(s.combiner.has_value());
    if (s.setccPayloads.size() != 2 || !s.combiner.has_value()) return false;
    bool const a = evalOne(s.setccPayloads[0]);
    bool const b = evalOne(s.setccPayloads[1]);
    return *s.combiner == "and" ? (a && b) : (a || b);
}

constexpr std::array<MirOpcode, 7> kImplementedPredicates{
    MirOpcode::FCmpOeq, MirOpcode::FCmpOne, MirOpcode::FCmpOlt,
    MirOpcode::FCmpOle, MirOpcode::FCmpOgt, MirOpcode::FCmpOge,
    MirOpcode::FCmpUne};
constexpr std::array<Outcome, 4> kOutcomes{
    Outcome::Lt, Outcome::Eq, Outcome::Gt, Outcome::Uo};

} // namespace

// ── THE exhaustive predicate × outcome × target truth table ──────────

TEST(FcmpLowering, PredicateTruthTableMatchesIeeeOnBothTargets) {
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto schema = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(schema.has_value());
        bool const isArm64 = std::string{targetName} == "arm64";
        for (MirOpcode pred : kImplementedPredicates) {
            auto const shape = lowerFcmpShape(**schema, pred);
            ASSERT_TRUE(shape.ok)
                << targetName << " failed to lower predicate "
                << static_cast<int>(pred);
            for (Outcome o : kOutcomes) {
                EXPECT_EQ(evalShape(**schema, isArm64, shape, o),
                          ieeeExpected(pred, o))
                    << targetName << " predicate ordinal "
                    << static_cast<int>(pred) << " outcome ordinal "
                    << static_cast<int>(o)
                    << " — the emitted condition shape disagrees with "
                       "IEEE 754 / C 6.5.8-6.5.9 (a NaN-parity "
                       "miscompile lever: e.g. dropping the ∧-ordered "
                       "half from Oeq flips the Uo cell)";
            }
        }
    }
}

// ── per-target shape pins (the swap/single/composed decisions) ───────

TEST(FcmpLowering, X86ShapesMatchTheLockedRealization) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    using CC = TargetCondCode;
    auto const expectSingle = [&](MirOpcode pred, CC cc, bool swapped) {
        auto const s = lowerFcmpShape(**schema, pred);
        ASSERT_TRUE(s.ok);
        ASSERT_EQ(s.setccPayloads.size(), 1u);
        EXPECT_EQ(s.setccPayloads[0], static_cast<std::uint32_t>(cc));
        EXPECT_EQ(s.swapped, swapped);
        EXPECT_FALSE(s.combiner.has_value());
    };
    // Relational family: seta/setae singles; Olt/Ole swap-canonicalized.
    expectSingle(MirOpcode::FCmpOgt, CC::Fogt, false);
    expectSingle(MirOpcode::FCmpOge, CC::Foge, false);
    expectSingle(MirOpcode::FCmpOlt, CC::Fogt, true);
    expectSingle(MirOpcode::FCmpOle, CC::Foge, true);
    // One IS a single on x86 (setne — ZF=0 is false on both equal and
    // unordered, since UCOMI sets ZF on unordered).
    expectSingle(MirOpcode::FCmpOne, CC::Fone, false);
    // Oeq/Une have NO single NaN-correct x86 condition → composed.
    auto const oeq = lowerFcmpShape(**schema, MirOpcode::FCmpOeq);
    ASSERT_TRUE(oeq.ok);
    ASSERT_EQ(oeq.setccPayloads.size(), 2u);
    EXPECT_EQ(oeq.setccPayloads[0], static_cast<std::uint32_t>(CC::Eq));
    EXPECT_EQ(oeq.setccPayloads[1], static_cast<std::uint32_t>(CC::Ford));
    EXPECT_EQ(oeq.combiner.value_or(""), "and");
    auto const une = lowerFcmpShape(**schema, MirOpcode::FCmpUne);
    ASSERT_TRUE(une.ok);
    ASSERT_EQ(une.setccPayloads.size(), 2u);
    EXPECT_EQ(une.setccPayloads[0], static_cast<std::uint32_t>(CC::Ne));
    EXPECT_EQ(une.setccPayloads[1], static_cast<std::uint32_t>(CC::Fuo));
    EXPECT_EQ(une.combiner.value_or(""), "or");
}

TEST(FcmpLowering, Arm64ShapesMatchTheLockedRealization) {
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    using CC = TargetCondCode;
    auto const expectSingle = [&](MirOpcode pred, CC cc, bool swapped) {
        auto const s = lowerFcmpShape(**schema, pred);
        ASSERT_TRUE(s.ok);
        ASSERT_EQ(s.setccPayloads.size(), 1u);
        EXPECT_EQ(s.setccPayloads[0], static_cast<std::uint32_t>(cc));
        EXPECT_EQ(s.swapped, swapped);
        EXPECT_FALSE(s.combiner.has_value());
    };
    expectSingle(MirOpcode::FCmpOgt, CC::Fogt, false);   // GT
    expectSingle(MirOpcode::FCmpOge, CC::Foge, false);   // GE
    expectSingle(MirOpcode::FCmpOlt, CC::Fogt, true);    // swapped GT
    expectSingle(MirOpcode::FCmpOle, CC::Foge, true);    // swapped GE
    expectSingle(MirOpcode::FCmpOeq, CC::Foeq, false);   // EQ (exact)
    expectSingle(MirOpcode::FCmpUne, CC::Fune, false);   // NE (true on Uo)
    // One has NO single NZCV condition (NE is true on unordered) →
    // composed NE ∧ VC — the x86 mirror-asymmetry.
    auto const one = lowerFcmpShape(**schema, MirOpcode::FCmpOne);
    ASSERT_TRUE(one.ok);
    ASSERT_EQ(one.setccPayloads.size(), 2u);
    EXPECT_EQ(one.setccPayloads[0], static_cast<std::uint32_t>(CC::Ne));
    EXPECT_EQ(one.setccPayloads[1], static_cast<std::uint32_t>(CC::Ford));
    EXPECT_EQ(one.combiner.value_or(""), "and");
}

// ── the F32 width axis on fcmp + the exotic-width wall ───────────────

TEST(FcmpLowering, F32OperandsSelectWidth32OnBothTargets) {
    for (char const* targetName : {"x86_64", "arm64"}) {
        auto schema = TargetSchema::loadShipped(targetName);
        ASSERT_TRUE(schema.has_value());
        auto const s64 = lowerFcmpShape(**schema, MirOpcode::FCmpOgt,
                                        TypeKind::F64);
        ASSERT_TRUE(s64.ok);
        EXPECT_EQ(s64.fcmpWidthBits, 64u) << targetName;
        auto const s32 = lowerFcmpShape(**schema, MirOpcode::FCmpOgt,
                                        TypeKind::F32);
        ASSERT_TRUE(s32.ok);
        EXPECT_EQ(s32.fcmpWidthBits, 32u)
            << targetName
            << ": F32 fcmp must select the width-32 variant "
               "(UCOMISS / FCMP S-form), never the F64 sibling";
    }
}

TEST(FcmpLowering, F16OperandsFailLoudViaTheWidthGuard) {
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    std::array<TypeKind, 2> paramKinds{TypeKind::F16, TypeKind::F16};
    auto syn = test_support::buildSyntheticFn(
        paramKinds, TypeKind::Bool,
        [&](MirBuilder& mb, TypeInterner& in,
            std::span<TypeId const> params, TypeId) {
            MirInstId const a = mb.addArg(0, params[0]);
            MirInstId const b = mb.addArg(1, params[1]);
            MirInstId const ops[] = {a, b};
            mb.addReturn(mb.addInst(MirOpcode::FCmpOgt, ops,
                                    in.primitive(TypeKind::Bool)));
        });
    DiagnosticReporter rep;
    auto const result = lowerToLir(syn.mir, **schema, syn.interner, rep);
    EXPECT_FALSE(result.ok);
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.actual.find("D-TARGET-ENCODING-WIDTH-GUARD")
                != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw);
}

// ── the unimplemented unordered-relational wall ──────────────────────

TEST(FcmpLowering, UnorderedRelationalPredicatesFailLoud) {
    // Ueq/Ult/Ule/Ugt/Uge have NO C producer (C's relationals are the
    // ordered forms; == is Oeq; != is Une) — they must REJECT loudly,
    // never silently borrow a wrong-parity condition.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    for (MirOpcode pred : {MirOpcode::FCmpUeq, MirOpcode::FCmpUlt,
                           MirOpcode::FCmpUle, MirOpcode::FCmpUgt,
                           MirOpcode::FCmpUge}) {
        std::array<TypeKind, 2> paramKinds{TypeKind::F64, TypeKind::F64};
        auto syn = test_support::buildSyntheticFn(
            paramKinds, TypeKind::Bool,
            [&](MirBuilder& mb, TypeInterner& in,
                std::span<TypeId const> params, TypeId) {
                MirInstId const a = mb.addArg(0, params[0]);
                MirInstId const b = mb.addArg(1, params[1]);
                MirInstId const ops[] = {a, b};
                mb.addReturn(mb.addInst(pred, ops,
                                        in.primitive(TypeKind::Bool)));
            });
        DiagnosticReporter rep;
        auto const result = lowerToLir(syn.mir, **schema, syn.interner, rep);
        EXPECT_FALSE(result.ok)
            << "predicate ordinal " << static_cast<int>(pred)
            << " must fail loud (no realization)";
    }
}

// ── CondBr fusion: single-cc fuses, composed materializes ────────────

namespace {
// f(double a, double b) { if (FCmp<pred>(a,b)) return 7; return 9; }
[[nodiscard]] test_support::SyntheticFn
buildFcmpBranchFn(MirOpcode pred) {
    std::array<TypeKind, 2> paramKinds{TypeKind::F64, TypeKind::F64};
    return test_support::buildSyntheticFn(
        paramKinds, TypeKind::I32,
        [&](MirBuilder& mb, TypeInterner& in,
            std::span<TypeId const> params, TypeId retT) {
            MirInstId const a = mb.addArg(0, params[0]);
            MirInstId const b = mb.addArg(1, params[1]);
            MirInstId const ops[] = {a, b};
            MirInstId const c =
                mb.addInst(pred, ops, in.primitive(TypeKind::Bool));
            MirBlockId const thenB = mb.createBlock(StructCfMarker::IfThen);
            MirBlockId const elseB = mb.createBlock(StructCfMarker::IfElse);
            mb.addCondBr(c, thenB, elseB);
            mb.beginBlock(thenB);
            MirLiteralValue v7; v7.value = std::int64_t{7};
            v7.core = TypeKind::I32;
            mb.addReturn(mb.addConst(v7, retT));
            mb.beginBlock(elseB);
            MirLiteralValue v9; v9.value = std::int64_t{9};
            v9.core = TypeKind::I32;
            mb.addReturn(mb.addConst(v9, retT));
        });
}

struct BranchShape {
    bool ok = false;
    std::vector<std::uint32_t> jccPayloads;
    std::size_t fcmpCount = 0;
};

[[nodiscard]] BranchShape
lowerBranchShape(TargetSchema const& schema, MirOpcode pred) {
    BranchShape out;
    auto syn = buildFcmpBranchFn(pred);
    DiagnosticReporter rep;
    auto const result = lowerToLir(syn.mir, schema, syn.interner, rep);
    if (!result.ok) return out;
    auto const jccOp  = schema.opcodeByMnemonic("jcc");
    auto const fcmpOp = schema.opcodeByMnemonic("fcmp");
    if (!jccOp || !fcmpOp) return out;
    Lir const& lir = result.lir;
    for (std::uint32_t f = 0; f < lir.moduleFuncCount(); ++f) {
        auto const fn = lir.funcAt(f);
        for (std::uint32_t bi = 0; bi < lir.funcBlockCount(fn); ++bi) {
            auto const bb = lir.funcBlockAt(fn, bi);
            for (std::uint32_t ii = 0; ii < lir.blockInstCount(bb); ++ii) {
                auto const id = lir.blockInstAt(bb, ii);
                if (lir.instOpcode(id) == *jccOp) {
                    out.jccPayloads.push_back(lir.instPayload(id));
                } else if (lir.instOpcode(id) == *fcmpOp) {
                    ++out.fcmpCount;
                }
            }
        }
    }
    out.ok = true;
    return out;
}
} // namespace

TEST(FcmpLowering, SingleCcPredicateFusesIntoJcc) {
    // x86 Ogt: the CondBr must branch DIRECTLY on the float condition
    // (jcc payload = Fogt) over a re-emitted fcmp — the ICmp-fusion
    // mirror. The materialized setcc copy from the value lowering
    // stays as dead code (D-LIR-SETCC-DEAD-AFTER-FUSION).
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const s = lowerBranchShape(**schema, MirOpcode::FCmpOgt);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(s.jccPayloads.size(), 1u);
    EXPECT_EQ(s.jccPayloads[0],
              static_cast<std::uint32_t>(TargetCondCode::Fogt));
    EXPECT_EQ(s.fcmpCount, 2u)
        << "the fused arm re-emits the float compare (the dead "
           "materialized pair is the established ICmp-fusion shape)";
}

TEST(FcmpLowering, ComposedPredicateBranchesOnMaterializedBool) {
    // x86 Oeq has NO single condition: the CondBr must take the
    // non-fused arm — branch Ne on the COMPOSED materialized Bool
    // (sete ∧ setnp), never a single jcc on a parity-blind cc. Fusion
    // of composed float conditions (a jcc pair / flag forwarding) is
    // deliberately an optimizer peephole, not done at lowering.
    auto schema = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(schema.has_value());
    auto const s = lowerBranchShape(**schema, MirOpcode::FCmpOeq);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(s.jccPayloads.size(), 1u);
    EXPECT_EQ(s.jccPayloads[0],
              static_cast<std::uint32_t>(TargetCondCode::Ne))
        << "composed predicates branch on the materialized Bool "
           "(cmp-against-0 + jne)";
    EXPECT_EQ(s.fcmpCount, 1u)
        << "no second fcmp — the composed value path is the branch "
           "input";
}

TEST(FcmpLowering, Arm64UneFusesAsSingleNe) {
    // The float_nan corpus' `if (nan != nan)` shape: arm64 fune = NE
    // is a NaN-correct single → fused b.ne; on x86 the same predicate
    // is composed → materialized (the cross-target asymmetry both
    // corpus arms witness at runtime).
    auto schema = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(schema.has_value());
    auto const s = lowerBranchShape(**schema, MirOpcode::FCmpUne);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(s.jccPayloads.size(), 1u);
    EXPECT_EQ(s.jccPayloads[0],
              static_cast<std::uint32_t>(TargetCondCode::Fune));
}

// ── red-on-disable: the capability signal is READ from config ────────

TEST(FcmpLowering, StripFoeqNibbleFallsBackToComposition) {
    // Remove arm64's foeq single → the lowering must select the
    // UNIVERSAL composed realization (Eq ∧ Ford) — proving the
    // single-vs-composed decision is the DECLARED config, not a
    // hardcoded per-target branch. (The truth table above already
    // proves the composition is NaN-correct on arm64's NZCV.)
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "arm64", [](nlohmann::json& doc) {
            doc["condCodeEncoding"].erase("foeq");
        });
    ASSERT_TRUE(mutated.has_value());
    auto const s = lowerFcmpShape(**mutated, MirOpcode::FCmpOeq);
    ASSERT_TRUE(s.ok)
        << "Oeq must still lower — via the composed fallback";
    ASSERT_EQ(s.setccPayloads.size(), 2u);
    EXPECT_EQ(s.setccPayloads[0],
              static_cast<std::uint32_t>(TargetCondCode::Eq));
    EXPECT_EQ(s.setccPayloads[1],
              static_cast<std::uint32_t>(TargetCondCode::Ford));
    EXPECT_EQ(s.combiner.value_or(""), "and");
}

// ── condCodeEncoding loader rules for the float arms ─────────────────

TEST(FcmpLowering, FloatCondArmsAreOptionalAtLoad) {
    // Stripping EVERY float arm must still load (pre-FC3.5 schemas
    // carry only the 10 integer arms — back-compat); the lowering then
    // realizes everything composable via compositions and fails loud
    // on the relational singles.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            for (auto const* k : {"fogt", "foge", "fone", "fuo", "ford"}) {
                doc["condCodeEncoding"].erase(k);
            }
        });
    EXPECT_TRUE(mutated.has_value())
        << "the float condCodeEncoding arms must be per-entry OPTIONAL";
}

TEST(FcmpLowering, MissingIntegerCondArmStillRejectsAtLoad) {
    // The pre-existing rule is unchanged: the 10 INTEGER arms are
    // required once the table is declared.
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["condCodeEncoding"].erase("eq");
        });
    EXPECT_FALSE(mutated.has_value());
}

TEST(FcmpLowering, UnknownCondKeyRejectsAtLoad) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["condCodeEncoding"]["bogus"] = 3;
        });
    EXPECT_FALSE(mutated.has_value());
}

TEST(FcmpLowering, FloatCondNibbleOutOfRangeRejectsAtLoad) {
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["condCodeEncoding"]["fogt"] = 16;
        });
    EXPECT_FALSE(mutated.has_value());
}

TEST(FcmpLowering, StripFogtNibbleFailsLoud) {
    // Fogt is a required single (no composed fallback exists for the
    // relational family) — stripping it must FAIL LOUD naming the
    // missing entry, never borrow the integer ugt nibble (HI/A would
    // be TRUE on unordered — the exact NaN miscompile the float codes
    // exist to prevent).
    auto mutated = test_support::mutateShippedTargetSchemaDoc(
        "x86_64", [](nlohmann::json& doc) {
            doc["condCodeEncoding"].erase("fogt");
        });
    ASSERT_TRUE(mutated.has_value());
    std::array<TypeKind, 2> paramKinds{TypeKind::F64, TypeKind::F64};
    auto syn = test_support::buildSyntheticFn(
        paramKinds, TypeKind::Bool,
        [&](MirBuilder& mb, TypeInterner& in,
            std::span<TypeId const> params, TypeId) {
            MirInstId const a = mb.addArg(0, params[0]);
            MirInstId const b = mb.addArg(1, params[1]);
            MirInstId const ops[] = {a, b};
            mb.addReturn(mb.addInst(MirOpcode::FCmpOgt, ops,
                                    in.primitive(TypeKind::Bool)));
        });
    DiagnosticReporter rep;
    auto const result = lowerToLir(syn.mir, **mutated, syn.interner, rep);
    EXPECT_FALSE(result.ok);
    bool saw = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::L_RequiredLirOpcodeMissing
            && d.actual.find("fogt") != std::string::npos) {
            saw = true;
        }
    }
    EXPECT_TRUE(saw) << "the diagnostic must name the missing 'fogt' "
                        "condCodeEncoding entry";
}
