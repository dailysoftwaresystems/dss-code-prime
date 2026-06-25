#include "analysis/semantic/type_rules.hpp"

#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

using namespace dss;

// ── compile-time invariants ───────────────────────────────────────────────
// Mirror the static_asserts in the header so a regression on the rank
// tables surfaces at compile time across the entire suite, not just the
// header's TU.
static_assert(detail::type_rules::signedIntRank(TypeKind::I8) == 1);
static_assert(detail::type_rules::signedIntRank(TypeKind::I128) == 5);
static_assert(detail::type_rules::unsignedIntRank(TypeKind::U8) == 1);
static_assert(detail::type_rules::floatRank(TypeKind::F64) > detail::type_rules::floatRank(TypeKind::F32));
static_assert(detail::type_rules::signedIntRank(TypeKind::Bool) == 0);
static_assert(detail::type_rules::unsignedIntRank(TypeKind::Char) == 0);
static_assert(detail::type_rules::floatRank(TypeKind::I32) == 0);

namespace {

// A self-contained TypeInterner needs a CompilationUnitId tag — any
// non-invalid value works for the rank-table behavior.
TypeInterner makeInterner() {
    return TypeInterner{CompilationUnitId{1}};
}

} // namespace

// isArithmetic accepts every signed/unsigned/float kind and rejects
// Bool/Char/Byte/Void (deliberately not arithmetic to forbid silent C-
// style implicit conversions).
TEST(TypeRules, IsArithmeticCoversIntsAndFloats) {
    auto in = makeInterner();
    EXPECT_TRUE(isArithmetic(in, in.primitive(TypeKind::I32)));
    EXPECT_TRUE(isArithmetic(in, in.primitive(TypeKind::U64)));
    EXPECT_TRUE(isArithmetic(in, in.primitive(TypeKind::F64)));
}
TEST(TypeRules, IsArithmeticRejectsNonArithmetic) {
    auto in = makeInterner();
    EXPECT_FALSE(isArithmetic(in, in.primitive(TypeKind::Bool)));
    EXPECT_FALSE(isArithmetic(in, in.primitive(TypeKind::Char)));
    EXPECT_FALSE(isArithmetic(in, in.primitive(TypeKind::Void)));
    EXPECT_FALSE(isArithmetic(in, InvalidType));
}

// Identical TypeIds are trivially assignable (the post-intern equality
// path).
TEST(TypeRules, IsAssignableIdentityHolds) {
    auto in = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    EXPECT_TRUE(isAssignable(in, i32, i32));
}

// Widening within the signed lattice: rank(rhs) <= rank(lhs) → assignable.
TEST(TypeRules, IsAssignableWidensWithinSigned) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto i16 = in.primitive(TypeKind::I16);
    auto i64 = in.primitive(TypeKind::I64);
    EXPECT_TRUE(isAssignable(in, i32, i16))  << "I16 widens to I32";
    EXPECT_TRUE(isAssignable(in, i64, i32))  << "I32 widens to I64";
    EXPECT_FALSE(isAssignable(in, i16, i32)) << "I32 does NOT narrow to I16 (intSameSignednessNarrows gate OFF default)";
}

// Cross-signedness is NOT assignable by DEFAULT (caller-declared explicit cast is
// the only path — no silent C-style I32 ↔ U32 for a non-C schema).
TEST(TypeRules, IsAssignableRejectsCrossSignedness) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto u32 = in.primitive(TypeKind::U32);
    EXPECT_FALSE(isAssignable(in, i32, u32));
    EXPECT_FALSE(isAssignable(in, u32, i32));
}

// D-CSUBSET-INT-CROSS-SIGNEDNESS-CONVERT: with the `intCrossSignednessConverts` gate ON
// (c-subset), signed↔unsigned IS assignable in BOTH directions and at ANY width (incl.
// cross-signedness narrowing U64→I32) — C 6.3.1.3 / 6.5.16.1. coerce() materializes the
// width-exact Cast. RED-ON-DISABLE: revert the isAssignable cross-signedness arm → the
// four EXPECT_TRUE flip to false. SCOPE GUARD: the gate is signed↔unsigned ONLY — a
// SAME-signedness narrowing stays strictly rejected even with the gate on.
TEST(TypeRules, IsAssignableAdmitsCrossSignednessWhenGated) {
    auto in  = makeInterner();
    auto i16 = in.primitive(TypeKind::I16);
    auto i32 = in.primitive(TypeKind::I32);
    auto u32 = in.primitive(TypeKind::U32);
    auto i64 = in.primitive(TypeKind::I64);
    auto u64 = in.primitive(TypeKind::U64);
    auto const G = [&](TypeId l, TypeId r) {
        return isAssignable(in, l, r, {}, /*boolWidensToArith=*/false,
                            /*charConvertsToArith=*/false, /*enumConvertsToArith=*/false,
                            /*intCrossSignednessConverts=*/true);
    };
    EXPECT_TRUE(G(i32, u32)) << "U32 -> I32 (same width)";
    EXPECT_TRUE(G(u32, i32)) << "I32 -> U32";
    EXPECT_TRUE(G(i32, u64)) << "U64 -> I32 (cross-signedness NARROWING, C 6.3.1.3)";
    EXPECT_TRUE(G(i64, u32)) << "U32 -> I64 (cross-signedness widening)";
    EXPECT_FALSE(G(i16, i32))
        << "I32 -> I16 SAME-signedness narrowing NOT admitted by the cross gate "
           "(needs the separate intSameSignednessNarrows gate, off here)";
}

// D-CSUBSET-INT-SAME-SIGN-NARROW: with `intSameSignednessNarrows` ON (c-subset), a
// SAME-signedness integer NARROWING (`short s = anInt;`, `signed char c = anInt;`,
// `int i = aLong;`) IS assignable — C 6.3.1.3 / 6.5.16.1, value-preserving in range,
// truncating (modular) out of range. coerce()'s arithmetic-core arm materializes the
// width-exact Cast (MIR Trunc) — the SAME path cross-signedness narrowing uses.
// RED-ON-DISABLE: the SAME narrowing calls with the gate OFF (the default 9th arg) must
// REJECT — proving the rank-arm narrowing branch actually gates. SCOPE GUARDS: WIDENING
// stays unconditional (gate-independent), and the same-sign gate does NOT admit a
// signed↔unsigned pair (that is intCrossSignednessConverts).
TEST(TypeRules, IsAssignableAdmitsSameSignednessNarrowingWhenGated) {
    auto in  = makeInterner();
    auto i8  = in.primitive(TypeKind::I8);
    auto i16 = in.primitive(TypeKind::I16);
    auto i32 = in.primitive(TypeKind::I32);
    auto i64 = in.primitive(TypeKind::I64);
    auto u8  = in.primitive(TypeKind::U8);
    auto u16 = in.primitive(TypeKind::U16);
    auto u32 = in.primitive(TypeKind::U32);
    // gate ON (9th arg true): narrowing admitted in BOTH signednesses.
    auto const N = [&](TypeId l, TypeId r) {
        return isAssignable(in, l, r, {}, /*boolWidensToArith=*/false,
                            /*charConvertsToArith=*/false, /*enumConvertsToArith=*/false,
                            /*intCrossSignednessConverts=*/false,
                            /*intSameSignednessNarrows=*/true);
    };
    // gate OFF (default 9th arg): the RED-ON-DISABLE reference.
    auto const Off = [&](TypeId l, TypeId r) {
        return isAssignable(in, l, r, {}, false, false, false,
                            /*intCrossSignednessConverts=*/false);
    };
    EXPECT_TRUE(N(i16, i32)) << "I32 -> I16 narrows (signed)";
    EXPECT_TRUE(N(i8,  i32)) << "I32 -> I8 narrows (signed)";
    EXPECT_TRUE(N(i32, i64)) << "I64 -> I32 narrows (signed)";
    EXPECT_TRUE(N(u8,  u32)) << "U32 -> U8 narrows (unsigned)";
    EXPECT_TRUE(N(u16, u32)) << "U32 -> U16 narrows (unsigned)";
    // RED-ON-DISABLE: every narrowing above REJECTS with the gate off.
    EXPECT_FALSE(Off(i16, i32)) << "gate off -> I32->I16 rejected";
    EXPECT_FALSE(Off(i8,  i32)) << "gate off -> I32->I8 rejected";
    EXPECT_FALSE(Off(u8,  u32)) << "gate off -> U32->U8 rejected";
    // WIDENING admitted regardless of the gate (the rank arms return true first).
    EXPECT_TRUE(N(i32, i16)) << "I16 -> I32 widens (gate-independent)";
    EXPECT_TRUE(N(u32, u8))  << "U8 -> U32 widens (gate-independent)";
    // SCOPE GUARD: same-sign gate does NOT cross signedness.
    EXPECT_FALSE(N(i32, u32)) << "same-sign gate does NOT admit U32->I32 (needs cross gate)";
    EXPECT_FALSE(N(u16, i32)) << "same-sign gate does NOT admit I32->U16 (needs cross gate)";
}

// Int ↔ Float is NOT assignable.
TEST(TypeRules, IsAssignableRejectsIntFloatCross) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto f64 = in.primitive(TypeKind::F64);
    EXPECT_FALSE(isAssignable(in, i32, f64));
    EXPECT_FALSE(isAssignable(in, f64, i32));
}

// InvalidType passes through on either side (cascade suppression: a
// single previous error should not produce a downpour of follow-on
// type-mismatch diagnostics).
TEST(TypeRules, InvalidIsAssignableEitherWay) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    EXPECT_TRUE(isAssignable(in, i32,        InvalidType));
    EXPECT_TRUE(isAssignable(in, InvalidType, i32));
    EXPECT_TRUE(isAssignable(in, InvalidType, InvalidType));
}

// ── enum ↔ int conversion (D-CSUBSET-ENUM-INT-CONVERSION) ──────────────────
// C 6.7.2.2: an enum IS an integer type. The `enumConvertsToArith` gate admits
// Enum ↔ the integer ranks in BOTH directions. RED-ON-DISABLE: the SAME calls
// with the gate OFF (the default) must REJECT — proving the arm actually gates.
TEST(TypeRules, IsAssignableEnumToIntWhenGated) {
    auto in    = makeInterner();
    auto i32   = in.primitive(TypeKind::I32);
    auto color = in.enumType("Color", TypeKind::I32);
    EXPECT_TRUE(isAssignable(in, i32, color, {}, false, false, /*enum=*/true))
        << "enum→int widens when gated (`return BLUE;`)";
    EXPECT_FALSE(isAssignable(in, i32, color))
        << "RED-ON-DISABLE: ungated, enum stays distinct from int";
}
TEST(TypeRules, IsAssignableIntToEnumWhenGated) {
    auto in    = makeInterner();
    auto i32   = in.primitive(TypeKind::I32);
    auto color = in.enumType("Color", TypeKind::I32);
    EXPECT_TRUE(isAssignable(in, color, i32, {}, false, false, /*enum=*/true))
        << "int→enum narrows when gated (`enum e = 1;` / `e += 1` write-back)";
    EXPECT_FALSE(isAssignable(in, color, i32))
        << "RED-ON-DISABLE: ungated, int does not flow into enum";
}
// SCOPE GUARD: the arm admits enum↔INT only — NEVER enum↔DIFFERENT-enum, even
// gated (a cross-enum assignment is a C constraint violation that stays loud).
TEST(TypeRules, IsAssignableDifferentEnumsRemainMismatch) {
    auto in = makeInterner();
    auto a  = in.enumType("A", TypeKind::I32);
    auto b  = in.enumType("B", TypeKind::I32);
    EXPECT_FALSE(isAssignable(in, a, b, {}, false, false, /*enum=*/true))
        << "different enums stay a mismatch even gated (no over-admission)";
    EXPECT_TRUE(isAssignable(in, a, a, {}, false, false, /*enum=*/true))
        << "same enum is the identity path";
}
// UAC: an enum participates in arithmetic AS its underlying int (`e + 1` types
// as int, not enum). RED-ON-DISABLE: revert the enum-underlying resolution in
// `usualArithmeticCommonType` → enum is unrecognized → InvalidType returns.
TEST(TypeRules, EnumPromotesToUnderlyingInArith) {
    auto in    = makeInterner();
    auto i32   = in.primitive(TypeKind::I32);
    auto color = in.enumType("Color", TypeKind::I32);
    ResolvedArithmeticRules const rules{};
    EXPECT_EQ(usualArithmeticCommonType(in, color, i32, rules), i32)
        << "enum + int → the underlying int (C 6.7.2.2 promotion)";
    EXPECT_EQ(usualArithmeticCommonType(in, color, color, rules), i32)
        << "enum + same-enum → the underlying int";
}

// D-UAC-SHIFT-RESULT-RULE-CONFIG: `shiftResultType` is the SINGLE chokepoint
// both the CST→HIR shift lowering and the semantic-tier expression typer route
// through, so the config verb `shiftResult` cannot be read inconsistently. The
// verb picks the result type: `promotedLeft` (C 6.5.7) → the promoted LEFT
// operand (`i32 << i64` is I32, the count's type never contributes);
// `commonType` → the usual-arithmetic common type (`i32 << i64` is I64). RED-ON-
// DISABLE: the I32↔I64 flip when ONLY the verb changes — a dead read would peg
// both arms at promotedLeft's I32 (or a divergent edit to one tier would let
// the two disagree; routing both through this one function forecloses that).
TEST(TypeRules, ShiftResultRuleSelectsByVerb) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto i64 = in.primitive(TypeKind::I64);
    ResolvedArithmeticRules rules{};   // default integer-promotion floor = I32
    rules.shiftResult = ShiftResultRule::PromotedLeft;
    EXPECT_EQ(shiftResultType(in, i32, i64, rules).v, i32.v)
        << "promotedLeft: i32 << i64 → the promoted LEFT operand i32";
    rules.shiftResult = ShiftResultRule::CommonType;
    EXPECT_EQ(shiftResultType(in, i32, i64, rules).v, i64.v)
        << "commonType: i32 << i64 → common(i32,i64) = i64 (red-on-disable flip)";
}

// unify picks the wider operand within a lattice; returns Invalid when
// the kinds don't share a lattice.
TEST(TypeRules, UnifyPicksWider) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto i16 = in.primitive(TypeKind::I16);
    auto f32 = in.primitive(TypeKind::F32);
    EXPECT_EQ(unify(in, i32, i16).v, i32.v);
    EXPECT_EQ(unify(in, i16, i32).v, i32.v);
    EXPECT_FALSE(unify(in, i32, f32).valid())
        << "int ↔ float is not in a shared lattice";
}

// unify cascades Invalid through (one Invalid → the other; both → Invalid).
TEST(TypeRules, UnifyCascadesInvalid) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    EXPECT_EQ(unify(in, i32, InvalidType).v, i32.v);
    EXPECT_EQ(unify(in, InvalidType, i32).v, i32.v);
    EXPECT_FALSE(unify(in, InvalidType, InvalidType).valid());
}
