/* AN OVER-ALIGNED OBJECT KEEPS ITS TYPE'S ALIGNMENT WHEREVER IT LIVES (P69 round 1, lane cs,
 * fold f26c). C 6.2.8 and 6.7.5: an object has the alignment its type states. DSS stated a
 * frame slot's alignment for a NAMED local and a VLA but not for an anonymous temp, and its
 * frame promises a slot that states nothing only the stack's 16 bytes -- so a temp of a type
 * aligned past 16 sat on a 16-byte boundary: the copy Win64 and AAPCS64 hand a callee as its
 * by-value parameter (MEASURED, probe w2: `&s` misaligned at 64, 128 and 256 on pe64, where cl
 * 19.51 aligns its 32- and 64-aligned copies -- probe w4, the address read through a volatile
 * object, as gcc answers a plain test from the type), a compound literal, and the temporary of a
 * call's and of a conditional's aggregate result (probe w3: misaligned on pe64 and ELF, debug
 * and release, where gcc, clang, Apple clang, aarch64 gcc and mingw-w64 gcc align every one at
 * -O0).
 * Each form is checked at 32, 64 and 256; every return before 42 names the first that failed:
 * 10 * form + the alignment's index (1, 2, 3). The by-value calls go once through a volatile
 * pointer (a real call) and once directly (the release arm inlines it, and the spliced body's
 * parameter is the caller's copy); values route through mutable globals. */
#include <stdint.h>

struct AL32 { _Alignas(32) long long v; long long w[3]; };
struct AL64 { _Alignas(64) long long v; long long w[7]; };
struct AL256 { _Alignas(256) long long v; long long w[31]; };

static int off(void const *p, unsigned long n) { return ((uintptr_t)p & (uintptr_t)(n - 1)) != 0; }

volatile int one = 1;
long long g = 10;

#define FORMS(N)                                                                                       \
    long long take##N(long long a, long long b, long long c, long long d, long long e, long long f,   \
                      long long h, struct AL##N s) {                                                   \
        if (off(&s, N)) return -1;                                                                     \
        return a + b + c + d + e + f + h + s.v;                                                        \
    }                                                                                                  \
    long long first##N(struct AL##N s, long long k) { return off(&s, N) ? -1 : s.v + k; }              \
    struct AL##N make##N(long long v) {                                                                \
        struct AL##N r = {0};                                                                          \
        r.v = v;                                                                                       \
        return r;                                                                                      \
    }                                                                                                  \
    long long (*volatile take##N##p)(long long, long long, long long, long long, long long, long long, \
                                     long long, struct AL##N) = take##N;                               \
    struct AL##N (*volatile make##N##p)(long long) = make##N;                                          \
    static int check##N(int idx) {                                                                     \
        struct AL##N named = {0};                                                                      \
        named.v = g;                                                                                   \
        if (off(&named, N)) return 10 + idx;                                                           \
        if (take##N##p(1, 2, 3, 4, 5, 6, 7, named) != 38) return 20 + idx;                              \
        if (take##N(1, 2, 3, 4, 5, 6, 7, named) != 38) return 30 + idx;                                 \
        if (first##N(named, 1) != 11) return 40 + idx;                                                 \
        if (off(&(struct AL##N){ .v = 1 }, N)) return 50 + idx;                                        \
        if (off((char const *)make##N##p(g).w - sizeof(long long), N)) return 60 + idx;                \
        if (off((char const *)(one ? named : named).w - sizeof(long long), N)) return 70 + idx;         \
        return 0;                                                                                      \
    }

FORMS(32)
FORMS(64)
FORMS(256)

int main(void) {
    int r;
    if ((r = check32(1)) != 0) return r;
    if ((r = check64(2)) != 0) return r;
    if ((r = check256(3)) != 0) return r;
    return 42;
}
