/* D-LIR-ASM-MEMORY-RESIDENT-FLOAT-OPERAND-CARRIED-AS-ITS-ADDRESS — a
 * `long double` bound to an inline-asm SIMD&FP register (`"w"`) must reach the
 * template as its VALUE and come back out of it as a value.
 *
 * On aarch64 Linux a `long double` is IEEE binary128 and DSS keeps it in a
 * 16-byte memory home. Before the fix the asm expansion handed the template the
 * home's ADDRESS: `low_half` below compiled and returned the bits of a stack
 * address (rc=0, the wrong answer, debug and release), and every shape that
 * WRITES a long double register was refused at encode. gcc 13.3.0 and clang
 * 18.1.3 compile and run all five shapes to 42 at -O0 and -O2 under
 * qemu-aarch64. On Apple arm64 a `long double` IS a `double`, so the same file
 * runs there through the ordinary register path — a control, not the subject.
 *
 * Every check is BITWISE over the value's own bytes (`sizeof (long double)`),
 * and the value is built from a bit pattern whose low 64 bits are not zero, so
 * a carriage that moved only half of the register, or an address, cannot pass.
 */
#include <string.h>

static int same(long double a, long double b) {
    return memcmp(&a, &b, sizeof a) == 0;
}

/* 1. read-write through an empty template: the register must come back as it went in */
__attribute__((noinline)) long double roundtrip(long double x) {
    long double y = x;
    __asm__("" : "+w"(y));
    return y;
}

/* 2. an INPUT the template reads: the low 64 bits of the register */
__attribute__((noinline)) double low_half(long double x) {
    double r;
    __asm__("fmov %d0, %d1" : "=w"(r) : "w"(x));
    return r;
}

/* 3. an OUTPUT the template writes: a full 128-bit register move */
__attribute__((noinline)) long double copy_q(long double a) {
    long double r;
    __asm__("mov %0.16b, %1.16b" : "=w"(r) : "w"(a));
    return r;
}

/* 4. two outputs (the second read back as a result PIECE), a "+w" tie, and a
 *    double operand in the same statement */
__attribute__((noinline)) long double swap(long double a, long double b, long double *other) {
    long double x, y;
    long double z = a;
    double volatile dv = 2.0;
    double d = dv;
    __asm__("mov %0.16b, %3.16b\n\t"
            "mov %1.16b, %4.16b"
            : "=&w"(x), "=&w"(y), "+w"(z)
            : "w"(b), "w"(a), "w"(d));
    *other = y;
    return same(z, a) ? x : a;     /* x == b iff every route carried its value */
}

/* 5. `asm goto` with a long double output that BOTH edges read */
__attribute__((noinline)) long double sel(long double a, long c, int *which) {
    long double r;
    __asm__ goto ("mov %0.16b, %1.16b\n\tcmp %2, #0\n\tb.ne %l[taken]"
                  : "=w"(r) : "w"(a), "r"(c) : "cc" : taken);
    *which = 1;
    return r;
taken:
    *which = 2;
    return r;
}

int main(void) {
    unsigned long long const bitsA[2] = { 0x0123456789ABCDEFull, 0x4004123456789ABCull };
    unsigned long long const bitsB[2] = { 0x0FEDCBA987654321ull, 0x3FFF0F0F0F0F0F0Full };
    long double a, b;
    memcpy(&a, bitsA, sizeof a);
    memcpy(&b, bitsB, sizeof b);

    if (!same(roundtrip(a), a)) return 11;

    double const lo = low_half(a);
    double want;
    memcpy(&want, bitsA, sizeof want);       /* little-endian: the low 8 bytes */
    if (memcmp(&lo, &want, sizeof lo) != 0) return 12;

    if (!same(copy_q(a), a)) return 13;

    long double other = 0.0L;
    long double const x = swap(a, b, &other);
    if (!same(x, b)) return 14;
    if (!same(other, a)) return 15;

    int which = 0;
    if (!same(sel(a, 0, &which), a) || which != 1) return 16;
    if (!same(sel(b, 1, &which), b) || which != 2) return 17;

    return 42;
}
