// D-CSUBSET-INT128-DATA-GLOBAL — a file-scope 128-bit integer global with an
// initializer, emitted as a real 16-byte little-endian image.
//
// ★ WHY EVERY VALUE HERE HAS A NON-ZERO HIGH WORD, AND WHY ONE DELIBERATELY
// DOES NOT. The original defect was that `appendLE` shifted a `std::uint64_t`
// by 64..120 bits — undefined behaviour that, on both shipped host arches,
// masks the shift count to 6 bits and REPEATS the low 8 bytes into the high 8.
// A global whose value fits in 64 bits therefore round-trips CORRECTLY even
// when completely broken (low = value, high = repeat of a zero-ish word), so a
// fits-in-64 probe is vacuous for the high half. `gWide`/`gSigned` carry
// distinct high and low words so a repeat, a zero-fill, or a swap all show up.
// `gSmall` is kept precisely because it is the case a naive gate MISSES: its
// value folds into a plain u64 literal arm rather than the 128-bit bignum arm,
// so a dispatch keyed on the value's VARIANT instead of the declared TYPE never
// fires for it.

// High word 0x1122334455667788, low word 0x99aabbccddeeff00 — every byte
// distinct in position, so a byte-order error cannot cancel out.
__uint128_t gWide = ((__uint128_t)0x1122334455667788ull << 64)
                  | (__uint128_t)0x99aabbccddeeff00ull;

// Folds into the PLAIN u64 arm, not the bignum arm. Its high 8 bytes must be
// ZERO; the UB shift produced a repeat of the low word here.
__uint128_t gSmall = 5;

// A NEGATIVE signed 128-bit global: its high limb must be sign-extended to all
// ones, not zero-filled. `-1` is the strongest form — every one of the 16 bytes
// must be 0xFF.
__int128 gSigned = -1;

// Zero, the control that must stay all-zero bytes.
__uint128_t gZero = 0;

int main(void) {
    int total = 0;

    // 12: the high word survived, distinct from the low word.
    if ((unsigned long long)(gWide >> 64) == 0x1122334455667788ull) total += 6;
    if ((unsigned long long)gWide == 0x99aabbccddeeff00ull)         total += 6;

    // 12: the fits-in-64 global has a ZERO high half — the arm a variant-keyed
    // dispatch misses, and the one the UB shift corrupted by repetition.
    if ((unsigned long long)gSmall == 5ull)         total += 6;
    if ((unsigned long long)(gSmall >> 64) == 0ull) total += 6;

    // 12: the signed global sign-extended rather than zero-filled.
    if ((unsigned long long)gSigned == 0xffffffffffffffffull)         total += 6;
    if ((unsigned long long)(gSigned >> 64) == 0xffffffffffffffffull) total += 6;

    // 6: the zero control.
    if ((unsigned long long)gZero == 0ull
        && (unsigned long long)(gZero >> 64) == 0ull) total += 6;

    return total;   // 6*7 = 42
}
