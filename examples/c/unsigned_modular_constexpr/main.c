// D-HIR-CONSTEVAL-UNSIGNED-WRAPAROUND-NOT-MODULAR — C 6.2.5p9 modular unsigned
// arithmetic, C 6.3.1.3p2 modular integer conversion, C 6.5.7p5 logical `>>`,
// and C 6.3.1.8 usual arithmetic conversions, all exercised in CONSTANT-
// EXPRESSION context, where DSS folded them in the host's SIGNED int64 domain.
//
// ★ WHY THE `!` FORMS ARE HERE AND NOT JUST THE PLAIN ONES. An assertion
// written in the positive fails LOUDLY when the fold is wrong, which is safe.
// The same fact written in the negative SILENTLY PASSES — the fold yields the
// wrong value, the negation of a wrong value is "true", and translation
// continues. Every pin below is therefore stated BOTH ways: `X` and `!(not X)`.
// A regression that reverts the modular reduction fails the positive arm; a
// regression that disables const-folding altogether fails the negative arm.
//
// Cross-checked against host clang AND host gcc, probed separately: both
// accept every line below.

// ── 1. Unsigned arithmetic wraps at the RESULT type's width (6.2.5p9) ──────
_Static_assert(0u - 1u == 0xffffffffu, "u32 wrap below zero");
_Static_assert(!(0u - 1u != 0xffffffffu), "u32 wrap below zero, negated");
_Static_assert(1u - 2u == 0xffffffffu, "u32 wrap below zero, offset");
_Static_assert(0xffffffffu + 1u == 0u, "u32 wrap above max");
_Static_assert(0xffffffffu * 2u == 0xfffffffeu, "u32 multiply wrap");
_Static_assert(0ull - 1ull == 0xffffffffffffffffull, "u64 wrap below zero");
_Static_assert(0xffffffffffffffffull + 1ull == 0ull, "u64 wrap above max");

// ── 2. Comparisons run in the operation's OWN signedness (6.3.1.8) ─────────
// These are the arms that silently passed while asserting the opposite.
_Static_assert(0u - 1u > 0u, "wrapped u32 is LARGE, not negative");
_Static_assert(!(0u - 1u < 0u), "wrapped u32 is LARGE, negated");
_Static_assert(0ull - 1ull > 0ull, "wrapped u64 is LARGE, not negative");
_Static_assert(0xffffffffffffffffull > 0ull, "a u64 literal above INT64_MAX");
_Static_assert(0xf501u - 0xf502 > 0, "mixed-sign operands convert to unsigned");
_Static_assert((unsigned int)0xf501 - 0xf502 > 0, "the cast spelling of the same");
_Static_assert(-1 == 0xffffffffu, "a negative signed operand converts modularly");

// ── 3. A cast records its DECLARED width, not a generic 64 (6.3.1.3p2) ────
_Static_assert((unsigned int)0 - 1u == 0xffffffffu, "cast result is 32-bit wide");
_Static_assert((unsigned int)0 - 1u != 0xffffffffffffffffull, "and NOT 64-bit wide");
_Static_assert((unsigned long)(-1) > 0, "negative widened to unsigned long");
_Static_assert((unsigned int)(-1) == 4294967295u, "negative widened to unsigned int");

// ── 4. `/`, `%` and `>>` are UNSIGNED operations on unsigned operands ──────
// The signed forms give 0, -1 and -1 respectively.
_Static_assert(0xffffffffffffffffull / 2ull == 9223372036854775807ull, "unsigned /");
_Static_assert(0xffffffffffffffffull % 2ull == 1ull, "unsigned %");
_Static_assert(0xffffffffffffffffull >> 1 == 9223372036854775807ull, "logical >>");
_Static_assert(0xffffffffu / 2u == 2147483647u, "unsigned / at 32 bits");

// ── 5. CONTROLS — the signed domain must be untouched ─────────────────────
// A fix applied too broadly (reducing every result as unsigned, or comparing
// everything unsigned) breaks exactly these.
_Static_assert(0 - 1 == -1, "signed subtraction stays signed");
_Static_assert(0 - 1 < 0, "signed comparison stays signed");
_Static_assert(-1 >> 1 == -1, "signed >> stays ARITHMETIC");
// C 6.3.1.1: a sub-int operand promotes to signed `int`, so this really is -1
// in clang and gcc too. A fix that made it 255 would be wrong.
_Static_assert((unsigned char)0 - (unsigned char)1 == -1, "u8 promotes to int");

// ── 6. The same folds in a CONSTANT-EXPRESSION context that is not an assert.
// A `_Static_assert` failure aborts translation, so it can never be observed
// in an exit code. These two turn the fold into a RUNTIME-observable value:
// the array dimension and the enum value are chosen so a non-modular fold
// either fails to compile or yields a different number.
enum WrapCheck { kWrapped = (int)(0u - 1u == 0xffffffffu) };   // 1 iff modular
static unsigned int arr[(0u - 1u) > 0u ? 7 : 3];               // 7 iff unsigned cmp

int main(void) {
    int total = 0;
    // 1 * 5 = 5
    total = total + (int)kWrapped * 5;
    // 7 * 5 = 35  (3 would give 15, and the sum would not be 42)
    total = total + (int)(sizeof arr / sizeof arr[0]) * 5;
    // 2 — the runtime arm, which was ALREADY correct and must stay correct:
    // the wrong-fold bug was confined to constant expressions.
    volatile unsigned int a = 0, b = 1;
    unsigned int r = a - b;
    total = total + (r == 0xffffffffu ? 2 : 0);
    return total;   // 5 + 35 + 2 = 42
}
