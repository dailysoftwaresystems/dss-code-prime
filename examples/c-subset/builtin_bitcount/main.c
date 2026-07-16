// FC17.9(b) (D-CSUBSET-BITCOUNT-INTRINSICS): end-to-end runtime witness for the 3
// hardware bit-count primitives, exposed as 6 GCC-compatible builtins — the
// walking skeleton for C23 <stdbit.h>. Each builtin is a config-declared intrinsic
// (c-subset.lang.json builtinFunctions, lowering "popcount"/"clz"/"ctz") that
// lowers to a DEDICATED pure-unary MIR op (Popcount/Clz/Ctz), then to a NATIVE
// instruction where the target declares the mnemonic, else a branchless SWAR
// bit-trick sequence — NOT an ordinary call/import (a compiler intrinsic is not a
// symbol). Per target this arm witnesses:
//   x86-64 : POPCNT / LZCNT / TZCNT           (all native)
//   arm64  : SWAR popcount + native CLZ + RBIT-then-CLZ ctz
// So the arm64-elf (qemu) arm is the RUNTIME closure for BOTH the SWAR popcount
// fallback AND the native CLZ / RBIT+CLZ path in one run.
//
// Fold-resistant: each builtin runs inside a wrapper fn taking the value as an ARG
// (SysV rdi / MS rcx), so on the BASELINE pipeline ConstFold never sees a literal;
// and Popcount/Clz/Ctz are deliberately NOT in ConstFold's fold set (like UMulH),
// so even when the RELEASE pipeline inlines the wrappers into main the ops stay
// LIVE -> the lowering is exercised on every arm (baseline + release).
//
// WIDTH + ROLE GUARD (the design-audit's required discrimination):
//   pc32(0x0000FFFF)          = 16   (16 low bits set)
//   pc64(0xFFFFFFFF00000000)  = 32   (32 HIGH bits set — a 32-bit popcount = 0)
//   clz32(0x00008000)         = 16   (bit 15 → 31-15 leading zeros, 32-bit)
//   clz64(0x0000000080000000) = 32   (bit 31 → 63-31 leading zeros; a 32-bit clz = 0)
//   ctz32(0x00800000)         = 23   (bit 23 → 23 trailing zeros)
//   ctz64(0x0000008000000000) = 39   (bit 39; a 32-bit ctz truncates to 0 -> 32)
// exit = 16+32+16+32+23+39 = 158. A width confusion (…ll run at 32-bit) or a
// clz<->ctz swap changes the sum; clz/ctz DEFINED at 0 = width (LZCNT/CLZ/TZCNT),
// a safe superset of GCC's UB-at-0.

typedef unsigned int       u32;
typedef unsigned long long u64;

static int pc32(u32 x) { return __builtin_popcount(x); }
static int pc64(u64 x) { return __builtin_popcountll(x); }
static int clz32(u32 x) { return __builtin_clz(x); }
static int clz64(u64 x) { return __builtin_clzll(x); }
static int ctz32(u32 x) { return __builtin_ctz(x); }
static int ctz64(u64 x) { return __builtin_ctzll(x); }

int main(void) {
    u32 a = 0x0000FFFFu;              // popcount 16
    u64 b = 0xFFFFFFFF00000000ull;    // popcount 32 (high half)
    u32 c = 0x00008000u;              // clz 16
    u64 d = 0x0000000080000000ull;    // clz 32
    u32 e = 0x00800000u;              // ctz 23
    u64 f = 0x0000008000000000ull;    // ctz 39

    int total = 0;
    total += pc32(a);
    total += pc64(b);
    total += clz32(c);
    total += clz64(d);
    total += ctz32(e);
    total += ctz64(f);
    return total;   // 158
}
