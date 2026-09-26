// P68 round 10 (lane `cs`) — C 6.3.1.8's FIFTH usual arithmetic conversion. A
// mixed-signedness pair of the SAME width whose SIGNED operand has the higher
// conversion rank (C 6.3.1.1 ranks by name: `long long` > `long` > `int`) cannot be
// represented by the signed type, so both operands convert to the UNSIGNED
// COUNTERPART of the signed type. On LP64 that is `long long + unsigned long` ->
// `unsigned long long`; on LLP64 (Windows) `long + unsigned int` -> `unsigned long`.
// The other pair on each model has DIFFERENT widths, where the wider signed type
// represents every value (the fourth conversion). The type only differs by NAME from
// the unsigned operand's own type, so `_Generic` is what can tell; the values are
// the controls (their signedness is the same either way). `bad` counts the checks
// that fail.

static int check(void) {
    long long a = -2;
    unsigned long b = 1;
    long c = -2;
    unsigned int d = 1;
    int const lp64 = sizeof(long) == sizeof(long long);
    int bad = 0;

    // The TYPES: C's answer for whichever data model this target has.
    bad += _Generic(a + b, unsigned long long: 1, unsigned long: 2, long long: 3, default: 4) != (lp64 ? 1 : 3);
    bad += _Generic(b + a, unsigned long long: 1, unsigned long: 2, long long: 3, default: 4) != (lp64 ? 1 : 3);
    bad += _Generic(c + d, unsigned long: 1, unsigned int: 2, long: 3, default: 4) != (lp64 ? 3 : 1);
    bad += _Generic(d + c, unsigned long: 1, unsigned int: 2, long: 3, default: 4) != (lp64 ? 3 : 1);
    bad += _Generic(d ? a : b, unsigned long long: 1, unsigned long: 2, long long: 3, default: 4) != (lp64 ? 1 : 3);

    // The VALUES: an unsigned result wraps -1 to its maximum; a signed one stays -1.
    bad += ((a + b) > 0) != lp64;
    bad += ((c + d) > 0) != !lp64;
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
