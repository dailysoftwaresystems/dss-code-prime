// FC3.5 sweep-c1 — the U32 -> U64 ZERO-extend witness
// (D-CSUBSET-ZEXT-32-TO-64 closed).
//
// `(unsigned long long)big` where big == 0xFFFFFFFF must produce
// 0x00000000FFFFFFFF — upper 32 bits ZERO. The two miscompile shapes
// this discriminates:
//
//   * SEXT misroute: sign-extending the 0xFFFFFFFF bit pattern gives
//     0xFFFFFFFFFFFFFFFF -> w / 2^32 == 0xFFFFFFFF != 0 -> exit 7.
//   * BYTE-form misroute (the pre-fix hazard): routing the U32
//     through the movzx r64, r/m8 byte widener reads ONE byte ->
//     w == 0xFF -> the `w == big64` check fails -> exit 7.
//
// Encoding discipline (the u32_wraparound precedent): 0xFFFFFFFF and
// 2^32 are BUILT at runtime from imm16-safe seeds — no literal wider
// than 65535 (arm64 MOVZ imm16 wall). Fold resistance: the seeds are
// FUNCTION ARGS, so the baseline arm keeps a live runtime zext.
//
//   big  = a*(b*b) + a            = 0xFFFFFFFF       (u32, exact)
//   two32= (c*c)*(c*c)            = 4294967296ULL    (u64, runtime)
//   w    = (unsigned long long)big                   (THE zext)
//   big64= two32 - 1ULL           = 0xFFFFFFFF as u64 (independent)
//
//   zext: w/two32 == 0  AND  w == big64  -> exit 42
//   sext: w/two32 != 0                   -> exit 7
//   byte: w != big64                     -> exit 7

unsigned long long widen(unsigned int a, unsigned int b,
                         unsigned long long c) {
    unsigned int big;
    big = b * b;                  // 65536
    big = a * big + a;            // 0xFFFFFFFF — built, never spelled
    unsigned long long two32;
    two32 = (c * c) * (c * c);    // 2^32 — u64 compute, runtime-built
    unsigned long long w;
    w = (unsigned long long)big;  // THE witness: U32 -> U64 zext
    unsigned long long big64;
    big64 = two32 - 1ULL;         // 0xFFFFFFFF, derived u64-side
    if (w / two32 == 0ULL) {      // upper 32 bits must be ZERO
        if (w == big64) {         // and ALL 32 low bits must survive
            return 42ULL;
        }
    }
    return 7ULL;
}

int main() {
    if (widen(65535u, 256u, 256ULL) == 42ULL) {
        return 42;
    }
    return 7;
}
