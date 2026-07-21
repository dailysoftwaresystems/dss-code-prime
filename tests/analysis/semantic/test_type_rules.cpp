#include "analysis/semantic/type_rules.hpp"

#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <utility>

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
// C14 (C 6.2.5p15 / 6.7.9p14): the three character types are {Char, I8=signed
// char, U8=unsigned char}; a NARROW string literal (element Char) may init an
// array of ANY of them, a WIDE literal only its same-kind array, and a
// non-character lhs never. RED-ON-DISABLE (compile-time): revert
// stringLiteralArrayInitCompatible to char-only and the U8/I8-from-Char asserts
// below FAIL to compile.
static_assert(detail::type_rules::isCharacterType(TypeKind::Char));
static_assert(detail::type_rules::isCharacterType(TypeKind::I8));
static_assert(detail::type_rules::isCharacterType(TypeKind::U8));
static_assert(!detail::type_rules::isCharacterType(TypeKind::I16));  // `short` is not a char type
static_assert(!detail::type_rules::isCharacterType(TypeKind::I32));
static_assert(detail::type_rules::stringLiteralArrayInitCompatible(TypeKind::U8,   TypeKind::Char));  // unsigned char[] = "…"
static_assert(detail::type_rules::stringLiteralArrayInitCompatible(TypeKind::I8,   TypeKind::Char));  // signed char[]   = "…"
static_assert(detail::type_rules::stringLiteralArrayInitCompatible(TypeKind::Char, TypeKind::Char));  // char[]          = "…"
static_assert(detail::type_rules::stringLiteralArrayInitCompatible(TypeKind::I32,  TypeKind::I32));   // wchar_t[] = L"…" (same kind)
static_assert(!detail::type_rules::stringLiteralArrayInitCompatible(TypeKind::I32, TypeKind::Char));  // int[]  = "…"  → reject
static_assert(!detail::type_rules::stringLiteralArrayInitCompatible(TypeKind::U8,  TypeKind::I32));   // uchar[] = L"…" → reject

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

// Int ↔ Float is NOT assignable with the gates OFF (the default — a non-C schema).
// This is the RED-ON-DISABLE reference for the gated-ON test below.
TEST(TypeRules, IsAssignableRejectsIntFloatCross) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto f64 = in.primitive(TypeKind::F64);
    EXPECT_FALSE(isAssignable(in, i32, f64));
    EXPECT_FALSE(isAssignable(in, f64, i32));
}

// D-CSUBSET-INT-FLOAT-CONVERSION: with `intConvertsToFloat` / `floatConvertsToInt`
// ON (c-subset), the int↔float implicit ASSIGNMENT conversion is admitted in the
// gated direction — `double d = 5;` (int→float), `int n = aDouble;` (float→int) —
// C 6.3.1.4 / 6.3.1.5 / 6.5.16.1. The two gates are INDEPENDENT: each admits only
// its own direction. coerce()'s arithmetic-core arm materializes the MIR
// SIToFP/UIToFP (int→float) / FPToSI/FPToUI (float→int). RED-ON-DISABLE: the SAME
// calls with both gates OFF (IsAssignableRejectsIntFloatCross above) REJECT.
// SCOPE GUARD: the gates admit int↔float ONLY — a pointer/struct (rank 0 in every
// helper) stays a loud mismatch, so `double d = ptr;` is NOT admitted.
TEST(TypeRules, IsAssignableAdmitsIntFloatWhenGated) {
    auto in  = makeInterner();
    auto i32 = in.primitive(TypeKind::I32);
    auto i64 = in.primitive(TypeKind::I64);
    auto u32 = in.primitive(TypeKind::U32);
    auto u64 = in.primitive(TypeKind::U64);
    auto f32 = in.primitive(TypeKind::F32);
    auto f64 = in.primitive(TypeKind::F64);
    auto pi  = in.pointer(i32);   // int*
    // intConvertsToFloat ON only (10th arg), floatConvertsToInt OFF (11th default).
    auto const I2F = [&](TypeId l, TypeId r) {
        return isAssignable(in, l, r, {}, /*boolWidensToArith=*/false,
                            /*charConvertsToArith=*/false, /*enumConvertsToArith=*/false,
                            /*intCrossSignednessConverts=*/false,
                            /*intSameSignednessNarrows=*/false,
                            /*intConvertsToFloat=*/true);
    };
    // floatConvertsToInt ON only (11th arg).
    auto const F2I = [&](TypeId l, TypeId r) {
        return isAssignable(in, l, r, {}, false, false, false, false, false,
                            /*intConvertsToFloat=*/false,
                            /*floatConvertsToInt=*/true);
    };
    // int → float (both signednesses, both float widths), the sqlite i64→f64 shape.
    EXPECT_TRUE(I2F(f64, i64)) << "i64 -> f64 (the sqlite volatile-double-param shape)";
    EXPECT_TRUE(I2F(f64, i32)) << "i32 -> f64";
    EXPECT_TRUE(I2F(f32, i32)) << "i32 -> f32";
    EXPECT_TRUE(I2F(f64, u64)) << "u64 -> f64";
    EXPECT_TRUE(I2F(f32, u32)) << "u32 -> f32";
    // The int→float gate does NOT admit the REVERSE (float→int) — that is the
    // independent floatConvertsToInt gate (off here).
    EXPECT_FALSE(I2F(i32, f64)) << "intConvertsToFloat does NOT admit f64 -> i32";
    EXPECT_FALSE(I2F(i64, f64)) << "intConvertsToFloat does NOT admit f64 -> i64";
    // float → int (both signednesses), via the independent gate.
    EXPECT_TRUE(F2I(i32, f64)) << "f64 -> i32 (`int n = aDouble;`)";
    EXPECT_TRUE(F2I(i64, f64)) << "f64 -> i64";
    EXPECT_TRUE(F2I(u32, f32)) << "f32 -> u32";
    // The float→int gate does NOT admit the reverse (int→float).
    EXPECT_FALSE(F2I(f64, i32)) << "floatConvertsToInt does NOT admit i32 -> f64";
    // SCOPE GUARD: neither gate admits a pointer ↔ float pair (rank 0 → no arm).
    EXPECT_FALSE(I2F(f64, pi)) << "int->float gate does NOT admit int* -> f64";
    EXPECT_FALSE(F2I(pi, f64)) << "float->int gate does NOT admit f64 -> int*";
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
// C14 (C 6.7.9p14 + 6.2.5p15, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): a NARROW
// string literal (`char[M]`) may initialize an array of ANY character type —
// char, signed char (I8), unsigned char (U8) — when N >= M. Gated on
// `charArrayFromStringLiteralInit` (position 8). RED-ON-DISABLE: revert the arm's
// element check to char-on-both-sides → the U8/I8 EXPECT_TRUEs below fail.
TEST(TypeRules, IsAssignableStringLiteralInitsAnyCharacterArray) {
    auto in     = makeInterner();
    auto charEl = in.primitive(TypeKind::Char);
    auto char4  = in.array(charEl, 4);                       // the narrow literal "abc" → char[4]
    auto uchar8 = in.array(in.primitive(TypeKind::U8), 8);
    auto schar8 = in.array(in.primitive(TypeKind::I8), 8);
    auto char8  = in.array(charEl, 8);
    auto int8   = in.array(in.primitive(TypeKind::I32), 8);
    auto uchar3 = in.array(in.primitive(TypeKind::U8), 3);   // N=3 < M=4 → over-long
    auto G      = true;   // charArrayFromStringLiteralInit gate (position 8)
    EXPECT_TRUE(isAssignable(in, uchar8, char4, {}, false, false, false, false, false, false, false, G))
        << "unsigned char[8] <- char[4] string literal (C 6.2.5p15) — the sqlite `const unsigned char zHex[]` shape";
    EXPECT_TRUE(isAssignable(in, schar8, char4, {}, false, false, false, false, false, false, false, G))
        << "signed char[8] <- char[4] string literal";
    EXPECT_TRUE(isAssignable(in, char8, char4, {}, false, false, false, false, false, false, false, G))
        << "char[8] <- char[4] string literal (the baseline must still hold)";
    EXPECT_FALSE(isAssignable(in, int8, char4, {}, false, false, false, false, false, false, false, G))
        << "int[8] <- char[4]: int is NOT a character type → stays a loud mismatch";
    EXPECT_FALSE(isAssignable(in, uchar3, char4, {}, false, false, false, false, false, false, false, G))
        << "unsigned char[3] <- char[4]: OVER-LONG (N < M) stays a loud mismatch";
    EXPECT_FALSE(isAssignable(in, uchar8, char4))
        << "ungated (no string-literal init context) → not admitted";
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

// ── C23 nullptr_t (D-CSUBSET-NULLPTR) ─────────────────────────────────────────
// `nullptr` (TypeKind::NullptrT) is a null pointer constant assignable WITHOUT cast
// to ANY pointer type, gated on `nullPointerConstantFromNullptrT`. ONE-WAY: nothing
// converts TO nullptr_t. nullptr → int is a constraint error; nullptr → bool is
// DEFERRED (D-CSUBSET-NULLPTR-BOOL-CONVERSION), so it is also rejected here.
TEST(TypeRules, NullptrTAssignsToPointerWhenEnabled) {
    auto in = makeInterner();
    TypeId const nptr    = in.primitive(TypeKind::NullptrT);
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    TypeId const intPtr  = in.pointer(in.primitive(TypeKind::I32));
    TypeId const boolT   = in.primitive(TypeKind::Bool);
    TypeId const intT    = in.primitive(TypeKind::I32);
    SemanticConfig::PointerConversionRules on;
    on.nullPointerConstantFromNullptrT = true;
    // nullptr → any pointer (object or, in the shipped surface, function pointer)
    EXPECT_TRUE(isAssignable(in, voidPtr, nptr, on));
    EXPECT_TRUE(isAssignable(in, intPtr,  nptr, on));
    // nullptr → int is NEVER admitted; nullptr → bool needs `scalarConvertsToBool`
    // (defaulted OFF here — see ScalarConvertsToBoolWhenGated for the gated-ON case).
    EXPECT_FALSE(isAssignable(in, intT,  nptr, on));
    EXPECT_FALSE(isAssignable(in, boolT, nptr, on));
    // ONE-WAY: nothing converts TO nullptr_t
    EXPECT_FALSE(isAssignable(in, nptr, voidPtr, on));
    EXPECT_FALSE(isAssignable(in, nptr, intT,    on));
}

// Red-on-disable: with the flag OFF, `p = nullptr` reverts to a type mismatch — the
// flag genuinely gates the behavior (a non-C23 schema keeps NullptrT inert).
TEST(TypeRules, NullptrTInertWhenDisabled) {
    auto in = makeInterner();
    TypeId const nptr    = in.primitive(TypeKind::NullptrT);
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    SemanticConfig::PointerConversionRules off;   // flag defaults false
    EXPECT_FALSE(isAssignable(in, voidPtr, nptr, off));
}

// Explicit cast: `(T*)nullptr` AND `(bool)nullptr` are castable; `(int)nullptr`
// (a non-bool arithmetic target) is NOT, and nothing casts TO nullptr_t (the
// one-way constraint holds for casts too). `(bool)nullptr` -> false — C23 6.3.2.3.2
// (D-CSUBSET-NULLPTR-BOOL-CONVERSION; nullptr lowers to 0, which truncates false).
TEST(TypeRules, NullptrTExplicitCastToPointerOrBool) {
    auto in = makeInterner();
    TypeId const nptr    = in.primitive(TypeKind::NullptrT);
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    TypeId const boolT   = in.primitive(TypeKind::Bool);
    TypeId const intT    = in.primitive(TypeKind::I32);
    EXPECT_TRUE(isExplicitCastable(in, voidPtr, nptr));    // (void*)nullptr
    EXPECT_FALSE(isExplicitCastable(in, intT,  nptr));     // (int)nullptr  — rejected
    EXPECT_TRUE(isExplicitCastable(in, boolT, nptr));      // (bool)nullptr — C23, -> false
    EXPECT_FALSE(isExplicitCastable(in, nptr, voidPtr));   // nothing → nullptr_t
    EXPECT_FALSE(isExplicitCastable(in, nptr, intT));
}

// C 6.3.1.2 (D-CSUBSET-NULLPTR-BOOL-CONVERSION): with `scalarConvertsToBool` ON, a
// scalar (arithmetic / Char / pointer / nullptr) is assignable INTO a `_Bool` lhs;
// OFF (the default), every one reverts to a mismatch — the flag genuinely gates the
// behavior (a non-C schema keeps `_Bool` strict). A non-scalar source (Void here,
// standing in for struct/union/FnSig — all rank-0, non-pointer) stays LOUD either
// way. This is the MIRROR of the boolWidensToArith arm (Bool rhs -> arith lhs).
TEST(TypeRules, ScalarConvertsToBoolWhenGated) {
    auto in = makeInterner();
    TypeId const boolT   = in.primitive(TypeKind::Bool);
    TypeId const intT    = in.primitive(TypeKind::I32);
    TypeId const dblT    = in.primitive(TypeKind::F64);
    TypeId const charT   = in.primitive(TypeKind::Char);
    TypeId const nptr    = in.primitive(TypeKind::NullptrT);
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    TypeId const voidT   = in.primitive(TypeKind::Void);
    SemanticConfig::PointerConversionRules pr;
    auto asg = [&](TypeId lhs, TypeId rhs, bool gate) {
        return isAssignable(in, lhs, rhs, pr,
            /*boolWidensToArith=*/false, /*charConvertsToArith=*/false,
            /*enumConvertsToArith=*/false, /*intCrossSignednessConverts=*/false,
            /*intSameSignednessNarrows=*/false, /*intConvertsToFloat=*/false,
            /*floatConvertsToInt=*/false, /*charArrayFromStringLiteralInit=*/false,
            /*bitIntConversions=*/false, /*scalarConvertsToBool=*/gate);
    };
    // Gated ON: scalar -> Bool admitted for arithmetic / char / pointer / nullptr.
    EXPECT_TRUE(asg(boolT, intT,    true));
    EXPECT_TRUE(asg(boolT, dblT,    true));
    EXPECT_TRUE(asg(boolT, charT,   true));
    EXPECT_TRUE(asg(boolT, voidPtr, true));
    EXPECT_TRUE(asg(boolT, nptr,    true));
    // Gated OFF (default): every one reverts to a mismatch (red-on-disable).
    EXPECT_FALSE(asg(boolT, intT,    false));
    EXPECT_FALSE(asg(boolT, dblT,    false));
    EXPECT_FALSE(asg(boolT, charT,   false));
    EXPECT_FALSE(asg(boolT, voidPtr, false));
    EXPECT_FALSE(asg(boolT, nptr,    false));
    // A genuinely-incompatible (non-scalar) source stays LOUD even gated ON.
    EXPECT_FALSE(asg(boolT, voidT,   true));
}

// ── D-LANG-VOIDPTR-FN-CONVERT (C 6.3.2.3) ─────────────────────────────────────
// Implicit function-pointer <-> `void*` conversion — the gcc/POSIX dlsym / Tcl
// ClientData idiom — gated on `allowVoidPtrFnConvert`, the SINGLE authoritative
// gate for the whole fn<->void* class (Option B). The newly-admitted form is the
// BARE function DESIGNATOR (`FnSig`, not yet decayed) -> `void*`; the `Ptr<FnSig>`
// <-> `void*` pointer-to-pointer arms are RE-HOMED onto the same flag. The
// boundary is Void-pointee-ONLY: a function designator / pointer -> a NON-void
// object pointer STAYS a loud reject regardless of the flag.

namespace {
// A bare `int(int)` function signature (a function DESIGNATOR type, NOT yet a
// pointer) — the un-decayed form the sqlite test_md5 `Tcl_CreateCommand` call
// passes into a `void*` ClientData parameter.
TypeId makeIntIntFnSig(TypeInterner& in) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const params[1] = { i32 };
    return in.fnSig(params, i32, CallConv::CcSysV);
}
} // namespace

// (a) + (d): a bare FnSig -> void* ADMITS with the flag on, REJECTS with it off
// (red-on-disable — the default-false gate genuinely controls the behavior).
TEST(TypeRules, FnSigToVoidPtrGatedByFlag) {
    auto in = makeInterner();
    TypeId const fnSig   = makeIntIntFnSig(in);
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    SemanticConfig::PointerConversionRules on;
    on.allowVoidPtrFnConvert = true;
    SemanticConfig::PointerConversionRules off;   // defaults false (strict)
    // `void* p = add40;` (bare designator into void*) — the exact blocker.
    EXPECT_TRUE(isAssignable(in, voidPtr, fnSig, on))
        << "bare FnSig -> void* admits when allowVoidPtrFnConvert is on";
    EXPECT_FALSE(isAssignable(in, voidPtr, fnSig, off))
        << "RED-ON-DISABLE: reverts to a loud reject when the flag is off";
}

// (b): the Option-B re-homing — `Ptr<FnSig> -> void*` AND `void* -> Ptr<FnSig>`
// route through `allowVoidPtrFnConvert`, NOT the generic implicitToVoidPtr /
// implicitFromVoidPtr. Proven with a config that turns ONLY the fn<->void* gate
// on (the generic void-ptr flags stay OFF): the conversion still admits, so it
// cannot be riding the generic flags. With everything off, both reject.
TEST(TypeRules, VoidPtrFnConvertReHomesPtrToPtrArms) {
    auto in = makeInterner();
    TypeId const fnSig   = makeIntIntFnSig(in);
    TypeId const fnPtr   = in.pointer(fnSig);                       // Ptr<FnSig>
    TypeId const voidPtr = in.pointer(in.primitive(TypeKind::Void));
    SemanticConfig::PointerConversionRules fnOnly;                  // ONLY the fn gate
    fnOnly.allowVoidPtrFnConvert = true;                           // generic flags stay false
    SemanticConfig::PointerConversionRules off;                    // all false
    // Ptr<FnSig> -> void* and void* -> Ptr<FnSig> both admit on the fn gate ALONE.
    EXPECT_TRUE(isAssignable(in, voidPtr, fnPtr, fnOnly))
        << "Ptr<FnSig> -> void* rides allowVoidPtrFnConvert, not implicitToVoidPtr";
    EXPECT_TRUE(isAssignable(in, fnPtr, voidPtr, fnOnly))
        << "void* -> Ptr<FnSig> rides allowVoidPtrFnConvert, not implicitFromVoidPtr";
    // RED-ON-DISABLE: with the fn gate off, both revert to a loud reject.
    EXPECT_FALSE(isAssignable(in, voidPtr, fnPtr, off));
    EXPECT_FALSE(isAssignable(in, fnPtr, voidPtr, off));
}

// (c): the FAIL-LOUD boundary — a function designator / function pointer -> a
// NON-void object pointer (`int*`) STAYS a loud reject regardless of the flag.
// The new admit is Void-pointee-ONLY; over-admitting here would be a real
// type-safety hole (a function address reinterpreted as int-typed storage). Even
// with EVERY void-ptr flag on, a non-void object pointee is out of scope for the
// fn<->void* class.
TEST(TypeRules, VoidPtrFnConvertBoundaryStaysLoud) {
    auto in = makeInterner();
    TypeId const fnSig  = makeIntIntFnSig(in);
    TypeId const fnPtr  = in.pointer(fnSig);                        // Ptr<FnSig>
    TypeId const intPtr = in.pointer(in.primitive(TypeKind::I32));  // int* (non-void)
    SemanticConfig::PointerConversionRules on;                     // ALL void-ptr flags on
    on.allowVoidPtrFnConvert = true;
    on.implicitToVoidPtr     = true;
    on.implicitFromVoidPtr   = true;
    SemanticConfig::PointerConversionRules off;
    // FnSig -> int* : never (the new arm is Void-pointee-only).
    EXPECT_FALSE(isAssignable(in, intPtr, fnSig, on))
        << "bare FnSig -> int* stays loud even with every void-ptr flag on";
    EXPECT_FALSE(isAssignable(in, intPtr, fnSig, off));
    // Ptr<FnSig> -> int* : two distinct non-void pointees, never implicit.
    EXPECT_FALSE(isAssignable(in, intPtr, fnPtr, on))
        << "Ptr<FnSig> -> int* stays loud (distinct non-void pointees)";
    EXPECT_FALSE(isAssignable(in, intPtr, fnPtr, off));
}

// ── C23 _BitInt(N) (D-CSUBSET-BITINT) ─────────────────────────────────────────

// A `_BitInt(N)` IS arithmetic (ungated shape admission — inert for non-C schemas
// which never mint a BitInt TypeId).
TEST(TypeRules, IsArithmeticAdmitsBitInt) {
    auto in = makeInterner();
    EXPECT_TRUE(isArithmetic(in, in.bitInt(17, /*signed=*/true)));
    EXPECT_TRUE(isArithmetic(in, in.bitInt(40, /*signed=*/false)));
}

// isAssignable admits BitInt↔BitInt and BitInt↔standard-integer ONLY when the
// `bitIntConversions` gate is on (RED-ON-DISABLE: default false rejects, so a
// non-C schema stays strict). M-8.
TEST(TypeRules, IsAssignableBitIntGated) {
    auto in = makeInterner();
    auto b4  = in.bitInt(4,  /*signed=*/false);
    auto b40 = in.bitInt(40, /*signed=*/false);
    auto i32 = in.primitive(TypeKind::I32);
    // gate OFF (default) — every BitInt pairing is a loud reject
    EXPECT_FALSE(isAssignable(in, b4, i32));
    EXPECT_FALSE(isAssignable(in, i32, b4));
    EXPECT_FALSE(isAssignable(in, b4, b40));
    // gate ON — bidirectional BitInt↔int + BitInt↔BitInt(any width)
    auto A = [&](TypeId l, TypeId r) {
        return isAssignable(in, l, r, {}, false, false, false, false, false,
                            false, false, false, /*bitIntConversions=*/true);
    };
    EXPECT_TRUE(A(b4, i32));
    EXPECT_TRUE(A(i32, b4));
    EXPECT_TRUE(A(b4, b40));
    EXPECT_TRUE(A(b40, b4));
}

// An explicit `(int)b` / `(_BitInt(N))x` cast is legal (isExplicitCastable).
TEST(TypeRules, IsExplicitCastableBitInt) {
    auto in = makeInterner();
    auto b17 = in.bitInt(17, true);
    auto i32 = in.primitive(TypeKind::I32);
    auto f64 = in.primitive(TypeKind::F64);
    EXPECT_TRUE(isExplicitCastable(in, i32, b17));   // (int)b
    EXPECT_TRUE(isExplicitCastable(in, b17, i32));   // (_BitInt(17))x
    EXPECT_TRUE(isExplicitCastable(in, b17, f64));   // (_BitInt(17))aDouble
    EXPECT_TRUE(isExplicitCastable(in, b17, in.bitInt(40, false)));  // BitInt→BitInt
}

// The usual arithmetic conversions (C23 6.3.1.8): two BitInts → the wider N (equal
// N → unsigned wins); BitInt(N) vs a standard int of width W → N>W ? BitInt : std.
TEST(TypeRules, UsualArithmeticCommonTypeBitInt) {
    auto in = makeInterner();
    ResolvedArithmeticRules rules;
    rules.minRank = TypeKind::I32;
    rules.bitIntConversions = true;
    auto UAC = [&](TypeId a, TypeId b) {
        return usualArithmeticCommonType(in, a, b, rules);
    };
    auto b20s = in.bitInt(20, true);
    auto b40s = in.bitInt(40, true);
    auto b20u = in.bitInt(20, false);
    auto i32  = in.primitive(TypeKind::I32);
    // two BitInts → wider N
    EXPECT_EQ(UAC(b20s, b40s).v, b40s.v);
    EXPECT_EQ(UAC(b40s, b20s).v, b40s.v);
    // equal N, mixed sign → unsigned wins
    EXPECT_EQ(UAC(b20s, b20u).v, b20u.v);
    // BitInt(40) vs int(32): N>W → the BitInt
    EXPECT_EQ(UAC(b40s, i32).v, b40s.v);
    // BitInt(20) vs int(32): N<W → the standard int (a _BitInt(20) does NOT out-rank int)
    EXPECT_EQ(in.kind(UAC(in.bitInt(20, true), i32)), TypeKind::I32);
    // BitInt vs float → the float
    EXPECT_EQ(in.kind(UAC(b40s, in.primitive(TypeKind::F64))), TypeKind::F64);
    // gate OFF → InvalidType (no accidental promotion)
    ResolvedArithmeticRules off;
    off.minRank = TypeKind::I32;
    EXPECT_FALSE(usualArithmeticCommonType(in, b20s, b40s, off).valid());
}

// D-LANG-TYPE-IDENTITY-VOCABULARY + C 6.3.2.1p2: the usual arithmetic conversions
// yield the UNQUALIFIED type. The float branch used to return the WINNING OPERAND
// VERBATIM, so a `volatile`/`_Atomic` skin rode into the common type — and since a
// qualifier CHANGE is the one thing the same-representation re-tag refuses, the
// coercion then materialized a synthetic Cast that lowers to a REAL `Bitcast`.
// Reachable exactly where two DISTINCT float vocabulary entries share a core: the
// f64 long-double axis (pe64 / apple-arm64), where `long double` IS F64.
//
// RED-ON-DISABLE: with the verbatim return this returns the qualified operand, so
// `qualifierBits` is non-zero and the TypeId is the operand's own.
TEST(TypeRules, UsualArithmeticCommonTypeDropsQualifiersOnTheFloatBranch) {
    auto in = makeInterner();
    ResolvedArithmeticRules rules;
    rules.minRank = TypeKind::I32;
    // The language declares `long double` a NAMED entry out-ranking `double`;
    // on an f64 axis both resolve to the SAME core.
    in.declareVocabularyRank("long double", 3);
    TypeId const dbl    = in.primitive(TypeKind::F64);                  // `double`
    TypeId const ld     = in.primitive(TypeKind::F64, "long double");
    TypeId const vld    = in.volatileQualified(ld);
    TypeId const aLd    = in.atomicQualified(ld);

    for (TypeId const qualified : {vld, aLd}) {
        // Both operand orders — the winner is picked by declared RANK, and the
        // result must be the bare, UNQUALIFIED `long double` either way.
        for (auto const [a, b] : {std::pair{dbl, qualified}, std::pair{qualified, dbl}}) {
            TypeId const got = usualArithmeticCommonType(in, a, b, rules);
            EXPECT_EQ(got.v, ld.v)
                << "the common type is the bare `long double` vocabulary entry";
            EXPECT_EQ(in.qualifierBits(got), 0u)
                << "C 6.3.2.1p2: the usual arithmetic conversions yield the "
                   "UNQUALIFIED type — a skin here becomes a spurious Bitcast";
        }
    }
    // The mixed float/integer arm takes the same path (the float side wins).
    TypeId const i32 = in.primitive(TypeKind::I32);
    EXPECT_EQ(usualArithmeticCommonType(in, vld, i32, rules).v, ld.v);
    EXPECT_EQ(usualArithmeticCommonType(in, i32, vld, rules).v, ld.v);
    // And the UNNAMED control is byte-identical to the historic behavior — in
    // BOTH orders. `(dbl, volatileQualified(dbl))` alone would be vacuous: the
    // pre-fix branch was `fa >= fb ? a : b`, which on an equal float rank returns
    // the LEFT operand, so putting the plain `double` on the left passes even
    // with the fix reverted. The qualified operand must lead for the anonymous
    // case to be pinned at all.
    for (auto const [a, b] : {std::pair{dbl, in.volatileQualified(dbl)},
                              std::pair{in.volatileQualified(dbl), dbl}}) {
        TypeId const got = usualArithmeticCommonType(in, a, b, rules);
        EXPECT_EQ(got.v, dbl.v)
            << "an anonymous same-core pair still yields the bare primitive";
        EXPECT_EQ(in.qualifierBits(got), 0u)
            << "C 6.3.2.1p2 applies to the UNNAMED case too — the skin must not "
               "ride into the common type just because no vocabulary entry is "
               "involved";
    }
}

// D-LANG-TYPE-IDENTITY-VOCABULARY + C 6.3.2.1p2, the COMPLEX arm. The `_Complex`
// branch runs BEFORE the float branch and used to build `interner.complex(elem)`
// from an operand taken VERBATIM — so the SAME qualifier skin rode into the
// element. `volatileDouble + complexF64` (the qualified operand FIRST, the order
// the `rea >= reb` tie resolves toward) yielded `Complex<volatile f64>`.
//
// RED-ON-DISABLE: with the verbatim element this returns a complex whose element
// carries the qualifier, so it is a DIFFERENT TypeId from `complex(f64)`.
TEST(TypeRules, UsualArithmeticCommonTypeDropsQualifiersOnTheComplexBranch) {
    auto in = makeInterner();
    ResolvedArithmeticRules rules;
    rules.minRank = TypeKind::I32;
    in.declareVocabularyRank("long double", 3);
    TypeId const dbl  = in.primitive(TypeKind::F64);
    TypeId const ld   = in.primitive(TypeKind::F64, "long double");
    TypeId const cf64 = in.complex(dbl);
    TypeId const cld  = in.complex(ld);
    TypeId const i32  = in.primitive(TypeKind::I32);

    // A qualified REAL operand, both orders — the element must be the bare f64.
    for (TypeId const q : {in.volatileQualified(dbl), in.atomicQualified(dbl)}) {
        for (auto const [a, b] : {std::pair{q, cf64}, std::pair{cf64, q}}) {
            TypeId const got = usualArithmeticCommonType(in, a, b, rules);
            ASSERT_EQ(in.kind(got), TypeKind::Complex);
            EXPECT_EQ(got.v, cf64.v)
                << "the complex element is the UNQUALIFIED f64 (C 6.3.2.1p2)";
            EXPECT_EQ(in.qualifierBits(in.complexElement(got)), 0u);
        }
    }
    // A qualified COMPLEX operand — the skin is on the complex itself.
    for (auto const [a, b] : {std::pair{in.volatileQualified(cf64), dbl},
                              std::pair{dbl, in.volatileQualified(cf64)}}) {
        TypeId const got = usualArithmeticCommonType(in, a, b, rules);
        EXPECT_EQ(got.v, cf64.v);
    }
    // Identity is preserved through the element, and the equal-core tie is broken
    // by declared RANK exactly as the real float branch does: `long double` wins
    // over `double` on an f64 axis, in BOTH orders.
    for (auto const [a, b] : {std::pair{cld, dbl}, std::pair{dbl, cld}}) {
        EXPECT_EQ(usualArithmeticCommonType(in, a, b, rules).v, cld.v)
            << "`_Complex long double` + `double` on an f64 axis is "
               "`_Complex long double`, not `_Complex double`";
    }
    // A real INTEGER operand still takes the complex's element, unqualified.
    EXPECT_EQ(usualArithmeticCommonType(in, in.volatileQualified(cf64), i32,
                                        rules).v, cf64.v);
}

// C99 _Complex (D-CSUBSET-COMPLEX §6.3.1.8 / design test #7): the usual arithmetic
// conversions when EITHER operand is complex — the result is complex over the WIDER
// float element; a REAL float operand contributes its own rank as the element; a
// real INTEGER operand takes the complex's element. Both operand orders pinned (the
// arm keys on `ka == Complex || kb == Complex`, so order must not matter).
TEST(TypeRules, UsualArithmeticCommonTypeComplex) {
    auto in = makeInterner();
    ResolvedArithmeticRules rules;
    rules.minRank = TypeKind::I32;
    auto UAC = [&](TypeId a, TypeId b) {
        return usualArithmeticCommonType(in, a, b, rules);
    };
    auto const f32  = in.primitive(TypeKind::F32);
    auto const f64  = in.primitive(TypeKind::F64);
    auto const i32  = in.primitive(TypeKind::I32);
    auto const cf32 = in.complex(f32);
    auto const cf64 = in.complex(f64);
    // complex(F32) + double → complex(F64): the REAL's rank wins the element
    // (the rea>=reb wider-element branch). BOTH orders.
    EXPECT_EQ(UAC(cf32, f64).v, cf64.v);
    EXPECT_EQ(UAC(f64, cf32).v, cf64.v);
    // complex(F64) + complex(F64) → complex(F64) (identity).
    EXPECT_EQ(UAC(cf64, cf64).v, cf64.v);
    // complex(F32) + complex(F64) → complex(F64) (wider element). BOTH orders.
    EXPECT_EQ(UAC(cf32, cf64).v, cf64.v);
    EXPECT_EQ(UAC(cf64, cf32).v, cf64.v);
    // complex + INTEGER → the complex unchanged (the int converts to the element;
    // it contributes NO float rank). BOTH orders, both elements.
    EXPECT_EQ(UAC(cf64, i32).v, cf64.v);
    EXPECT_EQ(UAC(i32, cf32).v, cf32.v);
    // float + float stays REAL (the complex arm must not over-fire).
    EXPECT_EQ(UAC(f32, f64).v, f64.v);
}
