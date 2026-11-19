/* P69 round 4 (lane `lm`): the C library's `long` functions carry the TYPE `long`, not only its width.
 *
 * `long` and `int` are two types where both are 32 bits wide (LLP64: Windows), and `long` and `long long` are two
 * types where both are 64 (LP64: Linux, macOS). A prototype that states only the width is the wrong type on EVERY
 * target, and it shows wherever a program asks the type instead of converting a value: `_Generic`, `typeof`, and a
 * pointer to the function. The shipped descriptors wrote `labs`, `atol`, `strtol`, `strtoul`, `ftell` and `fseek`
 * that way (`fn(i32) -> i32` / `fn(i64) -> i64`), and <windows.h>'s `ExitThread`, `Sleep`, `GetLastError` and
 * `GetCurrentThreadId` over a bare `u32` where the SDK says DWORD, which is `unsigned long`.
 *
 * WHAT IS CHECKED
 *   at compile time, on every target: each of the six, by its DESIGNATOR (the whole function type) and by a CALL (the
 *   result type); `ldiv`, `llabs` and `abs` beside them as the controls that were always right; on Windows the four
 *   kernel32 functions by their designators;
 *   at run time: `labs` of a value whose magnitude needs every bit of `long` (the WIDTH, which was already right and
 *   must stay so), the same call through a `long (*)(long)`, an object declared `typeof (labs(x))` reached through a
 *   `long *`, and `strtol` / `strtoul` / `atol` at the edge of 32 bits;
 *   on Windows also: a thread that leaves through <threads.h>'s `thrd_exit` and one that leaves through the
 *   header's own `ExitThread`, each joined for its code; the header's `Sleep` beside `thrd_sleep`; `GetLastError`
 *   and `Sleep` held by pointers over `unsigned long`. On that target `thrd_exit`, `thrd_join` and `thrd_sleep` are
 *   built by the compiler over the same kernel32 functions this file also names through the header, so the two ways
 *   of reaching `ExitThread` and `Sleep` meet in one program.
 *
 * Each check returns its own code; 42 = all held. */
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <threads.h>
#include <time.h>
#include <windows.h>
#endif

/* The designator: the function's whole type. */
_Static_assert(_Generic(labs, long (*)(long): 1, default: 0), "labs is long (long)");
_Static_assert(_Generic(atol, long (*)(const char *): 1, default: 0), "atol is long (const char *)");
_Static_assert(_Generic(strtol, long (*)(const char *, char **, int): 1, default: 0), "strtol returns long");
_Static_assert(_Generic(strtoul, unsigned long (*)(const char *, char **, int): 1, default: 0),
               "strtoul returns unsigned long");
_Static_assert(_Generic(ftell, long (*)(FILE *): 1, default: 0), "ftell returns long");
_Static_assert(_Generic(fseek, int (*)(FILE *, long, int): 1, default: 0), "fseek takes a long offset");

/* A call: the result type (the operand of `_Generic` is not evaluated). */
_Static_assert(_Generic(labs(1L), long: 1, default: 0), "labs(...) is a long");
_Static_assert(_Generic(atol("1"), long: 1, default: 0), "atol(...) is a long");
_Static_assert(_Generic(strtol("1", 0, 0), long: 1, default: 0), "strtol(...) is a long");
_Static_assert(_Generic(strtoul("1", 0, 0), unsigned long: 1, default: 0), "strtoul(...) is an unsigned long");
_Static_assert(_Generic(ftell(stdin), long: 1, default: 0), "ftell(...) is a long");

/* The controls: these three were always the type their header names. */
_Static_assert(_Generic(ldiv(1L, 1L).quot, long: 1, default: 0), "ldiv_t.quot is a long");
_Static_assert(_Generic(llabs(1LL), long long: 1, default: 0), "llabs(...) is a long long");
_Static_assert(_Generic(abs(1), int: 1, default: 0), "abs(...) is an int");

#if defined(_WIN32)
/* DWORD is `unsigned long`. */
_Static_assert(_Generic(ExitThread, void (*)(unsigned long): 1, default: 0), "ExitThread takes a DWORD");
_Static_assert(_Generic(Sleep, void (*)(unsigned long): 1, default: 0), "Sleep takes a DWORD");
_Static_assert(_Generic(GetLastError, unsigned long (*)(void): 1, default: 0), "GetLastError returns a DWORD");
_Static_assert(_Generic(GetCurrentThreadId, unsigned long (*)(void): 1, default: 0),
               "GetCurrentThreadId returns a DWORD");

static int leaves_through_the_shim(void *arg) {
    (void)arg;
    thrd_exit(7);
    return 0;
}

static int leaves_through_the_header(void *arg) {
    (void)arg;
    ExitThread(9);
    return 0;
}

static int win32_dwords(void) {
    thrd_t shim;
    thrd_t header;
    int fromShim = 0;
    int fromHeader = 0;
    if (thrd_create(&shim, leaves_through_the_shim, (void *)0) != thrd_success) return 20;
    if (thrd_create(&header, leaves_through_the_header, (void *)0) != thrd_success) return 21;
    if (thrd_join(shim, &fromShim) != thrd_success || fromShim != 7) return 22;
    if (thrd_join(header, &fromHeader) != thrd_success || fromHeader != 9) return 23;

    Sleep(1); /* the header's Sleep */
    struct timespec nap;
    nap.tv_sec = 0;
    nap.tv_nsec = 1000000;
    if (thrd_sleep(&nap, (void *)0) != 0) return 24; /* <threads.h>'s, over the same Sleep */

    void (*const sleeper)(unsigned long) = Sleep; /* a pointer over DWORD holds it */
    sleeper(0);

    unsigned long (*const lastError)(void) = GetLastError;
    if (CloseHandle((HANDLE)0) != 0) return 25; /* no such handle: the call fails */
    if (lastError() != 6ul) return 26;          /* ERROR_INVALID_HANDLE */

    typeof(GetCurrentThreadId()) self = GetCurrentThreadId();
    unsigned long *const selfAt = &self;
    if (*selfAt == 0) return 27;
    return 0;
}
#endif

static long through_a_pointer(long (*f)(long), long v) { return f(v); }

int main(void) {
    /* (1) THE WIDTH: a value whose magnitude needs every bit of `long`, whichever width that is. */
    volatile long big = -0x7ffffff1L;
    if (sizeof(long) == 8) big = (long)(big * 0x100000000LL - 1);
    long const want = -big;
    if (labs(big) != want) return 1;

    /* (2) A pointer to `long (long)` holds labs, and a call through it is the same call. */
    long (*const held)(long) = labs;
    if (through_a_pointer(held, big) != want) return 2;

    /* (3) An object of the call's own type is a long: a `long *` points at it. */
    typeof(labs(big)) magnitude = labs(big);
    long *const where = &magnitude;
    if (*where != want) return 3;

    /* (4) The conversions, at the edge of 32 bits (in range of `long` on every target). */
    char *end = 0;
    typeof(strtol("0", 0, 0)) parsed = strtol("-2147483000 tail", &end, 10);
    long *const parsedAt = &parsed;
    if (*parsedAt != -2147483000L || *end != ' ') return 4;
    typeof(strtoul("0", 0, 0)) uparsed = strtoul("4294967000", &end, 10);
    unsigned long *const uparsedAt = &uparsed;
    if (*uparsedAt != 4294967000UL || *end != '\0') return 5;
    typeof(atol("0")) plain = atol("-2147483001");
    long *const plainAt = &plain;
    if (*plainAt != -2147483001L) return 6;

#if defined(_WIN32)
    int const win = win32_dwords();
    if (win != 0) return win;
#endif
    return 42;
}
