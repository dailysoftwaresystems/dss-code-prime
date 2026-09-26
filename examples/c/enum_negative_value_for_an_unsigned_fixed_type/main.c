// P68 round 12 (lane `cs`) — a NEGATIVE enumerator value for an enumeration whose fixed
// underlying type is UNSIGNED is converted modulo 2^N, as a conversion to that type is,
// when the SIGNED type of the same width holds it (clang's extent). C23 6.7.3.3p3 makes
// the value a constraint violation, so each one draws a warning; the program means what
// clang builds it to mean. `bad` counts the checks that fail.

enum Byte : unsigned char {
    B_ALL_ONES = -1,
    B_TOP = -128
};
enum Wide : unsigned long long {
    W_ALL_ONES = -1
};

static int check(void) {
    int bad = 0;
    bad += B_ALL_ONES != 255;
    bad += B_TOP != 128;
    bad += W_ALL_ONES != 0xFFFFFFFFFFFFFFFFULL;
    bad += !(W_ALL_ONES > 0);
    return bad;
}

int main(void) { return check() == 0 ? 42 : 1; }
