// FC3 c1 — D-CSUBSET-UDIV-RUNTIME-HIGH-BIT-PIN (closed at 64-bit width;
// the unsigned-INT form upgrades when c2 ships the 32-bit ALU forms).
//
// FIRST runtime witness that an UNSIGNED division routes through the
// target's unsigned divide (MIR UDiv → x86 div_op / arm64 udiv), proven
// by a HIGH-BIT dividend: n = 65535^4 = 18445618199572250625 (bit 63
// SET, and < 2^64 so the construction is exact, no wrap needed).
//
//   unsigned:  n / 3 = 6148539399857416875  → sign bit CLEAR → exit 42
//   signed(!): (n as -1125874137300991) / 3 = -375291379100330
//                                           → sign bit SET   → exit 7
//
// The discriminator reinterprets the quotient as `long long` and tests
// its sign — exit-DIVERGENT if the division were mis-routed through
// sdiv (the red-on-disable lever at runtime).
//
// **Type discipline**: `unsigned long long` (U64 under EVERY shipped
// dataModel) — under the pe64 format's LLP64 a plain `unsigned long`
// is U32, which the D-CSUBSET-32BIT-ALU-FORMS gate stages out in c1.
//
// **Encoding discipline**: every literal fits the narrowest shipped
// immediate slot (arm64 movz imm16 ≤ 65535; x86 imm32): the high-bit
// dividend is BUILT at runtime by two multiplications of the 65535
// seed. No shifts/bitwise ops (no shipped encodings) and no wide
// immediates.
//
// **Fold resistance**: the seed and divisor reach `quotientTag` as
// FUNCTION ARGS, so the baseline (unoptimized) arm keeps a live runtime
// UDIV — the established `division/` fixture discipline. The optimized
// arm may fold the whole computation; ConstFold is signedness-aware, so
// the folded exit must AGREE (the differential harness compares arms).

unsigned long long quotientTag(unsigned long long a, unsigned long long d) {
    unsigned long long n;
    n = a * a;          // 65535^2 = 0xFFFE0001
    n = n * n;          // 65535^4 — bit 63 SET (exact, < 2^64)
    unsigned long long q;
    q = n / d;          // MIR UDiv — THE witness
    long long s;
    s = (long long)q;   // same-width reinterpret (Bitcast)
    if (s < 0ll) {
        return 7ull;    // a signed-routed quotient has the sign bit set
    }
    return 42ull;
}

int main() {
    unsigned long long r;
    r = quotientTag(65535ull, 3ull);
    if (r == 42ull) {
        return 42;
    }
    return 7;
}
