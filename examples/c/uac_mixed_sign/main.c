// FC3 c1 — the usual-arithmetic-conversions runtime lever (plan 23).
//
// C 6.3.1.8: comparing `long long` (-1) against `unsigned long long`
// (0) converts BOTH operands to the unsigned type — (unsigned long
// long)(-1) is 0xFFFF...F, so  -1 > 0ull  is TRUE in C. The comparison
// must lower as ICmpUgt over U64 with the signed operand converted by a
// REAL cast (same-width I64→U64 = Bitcast).
//
//   UAC correct  → unsignedWins(-1, 0) == 1 → exit 42
//   UAC broken   → a SIGNED compare → -1 > 0 false → exit 7
//
// (The red-on-disable lever at runtime: route the mixed-signedness verb
// through "signed wins" — or skip comparison promotion — and the exit
// flips to 7.)
//
// `long long` (not `long`) keeps ALL FOUR arms in c1's runnable set:
// under the pe64 format's LLP64 dataModel a `long` is 32-bit, and the
// mixed U32 compare is exactly the sub-native form the
// D-CSUBSET-32BIT-ALU-FORMS gate stages out until c2 — `long long` is
// I64/U64 under EVERY shipped model. The minus-one is built by
// subtraction (`0ll - 1ll`, the long_primitive_smoke idiom) keeping
// immediates imm32-safe, and the helper's result is compared `== 1`
// explicitly (a bare-int condition would coerce I32→Bool, a Trunc with
// no shipped encoding).
//
// Fold resistance: the comparison operands arrive as FUNCTION ARGS so
// the baseline arm keeps the live runtime compare; the optimized arm
// may fold — ConstFold's compare folding is signedness-aware, so both
// arms must agree on 42.

int unsignedWins(long long s, unsigned long long u) {
    if (s > u) {
        return 1;
    }
    return 0;
}

int main() {
    long long minusOne;
    minusOne = 0ll - 1ll;
    if (unsignedWins(minusOne, 0ull) == 1) {
        return 42;
    }
    return 7;
}
