// c103 (D-CSUBSET-INTRINSIC-UMULH): end-to-end runtime witness for the `__umulh`
// compiler intrinsic -- the high 64 bits of the u64*u64 128-bit product. `__umulh`
// is a config-declared builtin (c-subset.lang.json builtinFunctions, lowering:
// "umulh") that lowers to a DEDICATED MIR `UMulH` op, then to x86-64 `mul r/m64`
// (capturing RDX, the 'high' output-role) or arm64 native `umulh` -- NOT an
// ordinary call or a linkable import (a compiler intrinsic is not a symbol).
//
// ConstFold-resistant: the operands reach `umulh_high` as function args (SysV
// rdi/rsi or MS rcx/rdx), so on the baseline pipeline ConstFold never sees the
// literals; and UMulH is deliberately NOT in ConstFold's fold set, so even when the
// release pipeline inlines `umulh_high` into main the UMulH stays LIVE -> the
// intrinsic lowering is exercised on every arm (baseline + release).
//
// ROLE-SWAP GUARD (the design-audit's required pin): the operands are chosen so the
// HIGH word is non-zero AND != the low word. a = 0x123456789ABCDEF0, b = 2^32:
//   128-bit product = a << 32 = 0x00000000_12345678_9ABCDEF0_00000000
//   high 64 = 0x0000000012345678 ; low 64 = 0x9ABCDEF000000000
// exit = high64 & 0xFF = 0x78 = 120. A RAX/RDX role swap (capturing the low word)
// would return low64 & 0xFF = 0x00 = 0 != 120; a missing lowering fails to compile.

typedef unsigned long long u64;

static u64 umulh_high(u64 a, u64 b) {
    return __umulh(a, b);
}

int main(void) {
    u64 a = 0x123456789ABCDEF0;   // high word 0x12345678, low word 0x9ABCDEF0
    u64 b = 0x100000000;          // 2^32 -- shifts `a` up by 32 bits in the product
    u64 hi = umulh_high(a, b);
    return (int)(hi & 0xFF);      // 0x12345678 & 0xFF = 0x78 = 120
}
