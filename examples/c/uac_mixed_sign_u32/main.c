// FC3 c2 — the usual-arithmetic-conversions lever AT UNSIGNED-INT
// WIDTH (D-CSUBSET-32BIT-ALU-FORMS; the U32 sibling of c1's
// uac_mixed_sign, which had to use `long long`/`unsigned long long`
// because U32 compares were gated until c2).
//
// C 6.3.1.8: comparing `int` (-1) against `unsigned int` (0) converts
// BOTH operands to `unsigned int` — (unsigned int)(-1) is 0xFFFFFFFF,
// so  -1 > 0u  is TRUE in C. The comparison must lower as ICmpUgt over
// U32 (the conversion is a same-width Bitcast) and the cmp must be the
// 32-BIT form: the bitcast-from-negative operand sits sign-extended in
// its 64-bit register, and only a 32-bit compare reads exactly the
// window the C semantics define.
//
//   UAC + 32-bit unsigned compare -> unsignedWinsU32(-1, 0) == 1 -> exit 42
//   signed-routed compare         -> -1 > 0 false               -> exit 7
//
// The minus-one is built by subtraction (`0 - 1`, the established
// imm-safe idiom); the helper's result is compared `== 1` explicitly
// (a bare-int condition would coerce int -> Bool, a Trunc shape the
// width gate stages out).
//
// Fold resistance: the operands arrive as FUNCTION ARGS so the
// baseline arm keeps the live runtime compare.

int unsignedWinsU32(int s, unsigned int u) {
    if (s > u) {
        return 1;
    }
    return 0;
}

int main() {
    int minusOne;
    minusOne = 0 - 1;
    if (unsignedWinsU32(minusOne, 0u) == 1) {
        return 42;
    }
    return 7;
}
