/*
 * Darwin's <stdint.h> aliases, by IDENTITY rather than by width.
 *
 * ANCHOR: D-FFI-DESCRIPTOR-TYPE-ALIAS-SPELLING-KEYED-ON-DATA-MODEL-ALONE — the
 * shipped descriptors selected each 64-bit alias's SPELLING on `when:{dataModel}`
 * alone, which describes a two-world universe that has three worlds in it.
 *
 * ★ WHAT WAS BROKEN, AND WHY NOTHING MISCOMPILED. Darwin is LP64, so DSS gave it
 * the LP64 arm and recorded `uint64_t` as `unsigned long`. Apple's SDK spells it
 * `unsigned long long` (`usr/include/_types/_uint64_t.h`), while keeping `size_t`
 * as `unsigned long` (`__darwin_size_t`, `arm/_types.h` / `i386/_types.h`). Both
 * candidates are 8 bytes, so no value was ever computed wrongly — what DSS held
 * was a false ABI SPELLING. Since type identity was split off representation, that
 * is a real defect: a `uint64_t` interned as `unsigned long` is a DIFFERENT TYPE
 * from `unsigned long long`, so `_Generic` takes no arm and
 * `unsigned long long *p = &u64var;` is a bare `S_TypeMismatch`.
 *
 * ★ WHY THIS PROGRAM CHECKS IDENTITY WITH POINTERS AND NOT WITH `sizeof`.
 * `sizeof` cannot see the defect: every alias below is 8 bytes on Darwin before
 * and after. The two discriminators that CAN see it are pointer-assignment
 * compatibility and `_Generic`, and this file uses both, at the two different
 * times they are answerable:
 *
 *   COMPILE TIME — each initializer below takes the address of one alias and
 *                  stores it in a pointer to an INDEPENDENTLY SPELLED named C
 *                  type. C requires the pointee types to be compatible, so the
 *                  file compiles only if every alias IS the type named beside it.
 *                  Under the defect the first initializer is `S_TypeMismatch`
 *                  and no artifact is produced. This is why the pin does not need
 *                  a Mac: every host that BUILDS the Mach-O legs runs it.
 *   RUN TIME     — `_Generic` re-derives each spelling as a string and the program
 *                  prints all five, so the manifest's `expectedStdout` is a second,
 *                  independent channel. A compile that somehow satisfied the
 *                  pointers while resolving `_Generic` differently would still be
 *                  caught here.
 *
 * ★ THE SPLIT IS THE POINT, so both sides of it are asserted. `uint64_t` and its
 * least/fast siblings move to `long long` on Darwin; `uintmax_t`, `uintptr_t` and
 * `size_t` STAY `long`/`unsigned long` there, exactly as on glibc. A fix that
 * flipped the whole header would satisfy the first three lines and fail the rest,
 * which is what makes this a table and not a sample.
 *
 * ✔MEASURED 2026-09-01 on the operator's physical Mac — macOS 26.6.2, build 25G83,
 * MacOSX26.5.sdk, Apple clang 21.0.0 — with a compile-only `_Generic` probe that
 * tested every alias against every candidate named type under BOTH `-arch arm64`
 * and `-arch x86_64`. The two arches answered byte-identically, which is why this
 * example ships on both Mach-O targets and expects the same output from each.
 *
 * FOLD-RESISTANCE: the exit code is built from `volatile` seeds read through the
 * pointers, so the adds stay live instead of folding to one immediate, and the
 * `release` arm in the manifest runs the shipped release pipeline over the same
 * source so optimizer x feature composition is witnessed rather than assumed.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/* The five aliases, each initialized from a volatile seed so the values below
 * cannot be folded into the return statement. 8 + 9 + 10 + 7 + 8 == 42. */
static volatile unsigned long long gSeedU64  = 8;
static volatile unsigned long      gSeedUmax = 9;
static volatile unsigned long      gSeedSize = 10;
static volatile long long          gSeedI64  = 7;
static volatile long               gSeedImax = 8;

static uint64_t  gU64;
static uintmax_t gUmax;
static size_t    gSize;
static int64_t   gI64;
static intmax_t  gImax;

/* ── COMPILE-TIME IDENTITY ────────────────────────────────────────────────
 * Each of these compiles only if the alias on the right IS the named C type on
 * the left. `unsigned long long *` accepting `&gU64` is the whole row: under the
 * defect `uint64_t` was `unsigned long` and this line did not compile. */
static unsigned long long *const kU64IsUnsignedLongLong  = &gU64;
static unsigned long      *const kUmaxIsUnsignedLong     = &gUmax;
static unsigned long      *const kSizeIsUnsignedLong     = &gSize;
static long long          *const kI64IsLongLong          = &gI64;
static long               *const kImaxIsLong             = &gImax;

/* ── RUN-TIME IDENTITY ────────────────────────────────────────────────────
 * The same question asked of `_Generic`, whose association matching is the other
 * place C makes type identity observable. */
#define SPELL(T) _Generic((T)0,                 \
    unsigned long:      "ul",                   \
    unsigned long long: "ull",                  \
    long:               "l",                    \
    long long:          "ll",                   \
    default:            "?")

int main(void) {
    *kU64IsUnsignedLongLong = gSeedU64;
    *kUmaxIsUnsignedLong    = gSeedUmax;
    *kSizeIsUnsignedLong    = gSeedSize;
    *kI64IsLongLong         = gSeedI64;
    *kImaxIsLong            = gSeedImax;

    printf("u64=%s umax=%s uptr=%s size=%s i64=%s imax=%s\n",
           SPELL(uint64_t), SPELL(uintmax_t), SPELL(uintptr_t),
           SPELL(size_t), SPELL(int64_t), SPELL(intmax_t));

    return (int)(*kU64IsUnsignedLongLong + *kUmaxIsUnsignedLong
                 + *kSizeIsUnsignedLong
                 + (unsigned long long)*kI64IsLongLong
                 + (unsigned long long)*kImaxIsLong);
}
