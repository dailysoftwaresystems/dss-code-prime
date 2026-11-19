/* A STACKED ARGUMENT IS PLACED ON ITS OWN ALIGNMENT, WHATEVER THE STACK POINTER'S IS AT THE CALL (P69 round 1,
 * lane cs, fold f26c). Under SysV a by-value aggregate over 16 bytes travels as its bytes on the stack, begun on
 * its own alignment, and the callee may use those bytes in place (gcc and clang do); the stack pointer itself is
 * promised only 16. DSS placed the bytes at their alignment RELATIVE to the stack pointer and never realigned
 * it, so a 32-, 64- or 256-aligned argument sat misaligned in absolute terms (MEASURED, the native witness in
 * run f26c-s22: a gcc or clang callee's `&s` misplaced, every value right). A caller that passes such an
 * argument now realigns its stack pointer once, on the dynamic frame a VLA function uses. Here a VARIADIC callee
 * reads every argument where the caller put it -- `va_arg`'s object is the caller's bytes under SysV and the
 * caller's copy under a pointer convention -- and checks its address, read through a volatile object. Five named
 * `long long`s take the integer registers first, so the `long long` passed after the aggregates is stacked too
 * and the caller's outgoing area is not a multiple of 256 until the caller rounds it. Three callers: a plain one,
 * one with a VLA (its descent keeps the boundary), and one that is itself variadic (its `va_start` areas are the
 * frame pointer's in that frame); and the frame alone: a VLA in a variadic function, then all three at once.
 * Every return before 42 names the first that failed: 10 * caller + the alignment's index (1, 2, 3) for a
 * misplaced argument, 9 for a wrong value, 49 for the VLA-and-variadic function's values. */
#include <stdarg.h>
#include <stdint.h>

struct AL32 { _Alignas(32) long long v; long long w[3]; };
struct AL64 { _Alignas(64) long long v; long long w[7]; };
struct AL256 { _Alignas(256) long long v; long long w[31]; };

static int off(void const *p, unsigned long n) {
    void const *volatile at = p;
    return ((uintptr_t)at & (uintptr_t)(n - 1)) != 0;
}

/* r1..r5 are 1..5; the aggregates' first members 10, 20 and 12, their last words 0; the last argument 7. */
static int seen(int caller, long long r1, long long r2, long long r3, long long r4, long long r5, ...) {
    va_list ap;
    va_start(ap, r5);
    long long *w32 = va_arg(ap, struct AL32).w;
    int bad = off((char const *)w32 - sizeof(long long), 32) ? 1 : 0;
    long long v32 = w32[-1];
    long long *w64 = va_arg(ap, struct AL64).w;
    if (!bad && off((char const *)w64 - sizeof(long long), 64)) bad = 2;
    long long v64 = w64[-1];
    long long *w256 = va_arg(ap, struct AL256).w;
    if (!bad && off((char const *)w256 - sizeof(long long), 256)) bad = 3;
    long long v256 = w256[-1];
    long long last = va_arg(ap, long long);
    va_end(ap);
    if (bad) return 10 * caller + bad;
    return v32 + v64 + v256 == 42 && r1 + r2 + r3 + r4 + r5 == 15 && last == 7 ? 0 : 9;
}

struct AL32 a32 = { 10 };
struct AL64 a64 = { 20 };
struct AL256 a256 = { 12 };
volatile int three = 3;

static int plain(void) { return seen(1, 1, 2, 3, 4, 5, a32, a64, a256, 7LL); }

static int withVla(int n) {
    char buf[n];
    buf[0] = 0;
    return seen(2, 1, 2, 3, 4, 5, a32, a64, a256, 7LL) + buf[0];
}

static int withVaStart(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int k = va_arg(ap, int);
    va_end(ap);
    return seen(3, 1, 2, 3, 4, 5, a32, a64, a256, 7LL) + k - 7;
}

/* The same dynamic frame with nothing over-aligned to pass: a VLA in a variadic function (it was refused, the
 * `va_start` areas then being stack-pointer-relative). 0 when every argument arrived. */
static int vlaVariadic(int n, ...) {
    long long buf[n];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; ++i) buf[i] = va_arg(ap, long long);
    va_end(ap);
    return buf[0] + buf[1] + buf[2] == 42 ? 0 : 49;
}

/* All three at once: a variadic caller with a VLA passing the over-aligned arguments. */
static int all(int n, ...) {
    char buf[n];
    buf[0] = 0;
    va_list ap;
    va_start(ap, n);
    int k = va_arg(ap, int);
    va_end(ap);
    return seen(5, 1, 2, 3, 4, 5, a32, a64, a256, 7LL) + buf[0] + k - 7;
}

int main(void) {
    int r = plain();
    if (r == 0) r = withVla(three);
    if (r == 0) r = withVaStart(1, 7);
    if (r == 0) r = vlaVariadic(three, 10LL, 20LL, 12LL);
    if (r == 0) r = all(three, 7);
    return r == 0 ? 42 : r;
}
