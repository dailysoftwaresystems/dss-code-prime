// D-CSUBSET-INT128-ICE-CONTEXT-REFUSED — THE INVARIANT HALF.
//
// The sibling example `examples/c/c_int128_ice_slots` pins that a 128-bit constant
// whose VALUE fits an int64 is now admitted in a 64-bit integer-constant-expression
// slot. This file pins the property that makes that safe: a value that does NOT fit
// must STILL FAIL LOUD, and must NEVER silently become the low 64 bits of itself.
//
// ★★ THE BOUND BELOW IS THE TRUNCATION TRAP, AND IT IS WHY THIS FILE EXISTS RATHER
// THAN A ROUNDER `1 << 100`. Its true value is 2^100 + 3; its LOW 64 BITS ARE
// EXACTLY 3. A bridge that narrowed by taking `low64()` — the obvious and wrong way
// to widen the old width test — would turn this declaration into `int a[3]`,
// compile clean, and ship an array four orders of magnitude too small with no
// diagnostic at all. That is a silent miscompile, and it is indistinguishable from
// correct behaviour in any test whose wide value happens to have zero low bits.
// The value here is chosen so that the two answers DIFFER: refused, or `3`.
//
// ✔MEASURED: gcc 13.3.0 (`-std=c2x`) and clang 18.1.3 (`-std=c23`), probed
// SEPARATELY, both REFUSE this ("size of array is too large" / "array is too
// large"). They refuse on the OBJECT SIZE where DSS refuses on the constant not
// fitting the slot — a different reason, the same direction, and both loud.
//
// RED-ON-DISABLE: replace the magnitude test in `asInt64`
// (src/hir/const_eval_arith.hpp) with a `low64()` narrowing and this file COMPILES
// instead of erroring, so the runner no longer finds S_NonConstantArrayLength.

int a[((__int128)1 << 100) + 3];

int main(void) {
    return (int)(sizeof(a) / sizeof(a[0]));
}
