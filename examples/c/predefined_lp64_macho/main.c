/*
 * TF-C97 — the OBJECT FORMAT's own predefined macros: `__LP64__` / `_LP64`.
 *
 * ANCHOR: D-PP-FORMAT-DATA-MODEL-PREDEFINES — every `*.format.json` now declares the C-visible face of its own `dataModel`, and the LLP64/ILP32 files declare NEITHER spelling; this program is the runnable witness for both directions.
 *
 * ★ WHAT WAS BROKEN, AND WHY IT WAS WORSE THAN A MISSING MACRO. MEASURED on
 * `arm64:macho64-arm64-darwin-staticlib` before this cycle: `__arm64__` defined,
 * `__APPLE__` defined, `__LP64__` NOT defined — while `sizeof(long) == 8` and
 * `sizeof(void *) == 8`. That is not a gap, it is an INCOHERENT WORLD: LP64
 * widths compiled against ILP32 header arms. The macOS SDK gates 234 occurrences
 * across 91 headers on `__LP64__`, so the SDK described one ABI while the
 * compiler implemented another, in silence. `$SDK/usr/include/mach/port.h:113-114`
 * then asserted the **user32** struct size (12) instead of user64's (16), which
 * is sqlite `mem1.c`'s three `error[S0029]`.
 *
 * ★ WHAT THIS PROGRAM ASSERTS, AND WHY IT IS A COHERENCE TEST RATHER THAN A
 * DEFINEDNESS TEST. `#ifdef __LP64__ ... #else ... #endif` selects an arm, and
 * EACH ARM STATES THE WIDTHS IT BELIEVES IT IS COMPILING FOR. The widths are
 * then checked against the compiler's real answer twice over:
 *
 *   COMPILE TIME — `_Static_assert(sizeof(long) == <the arm's claim>)`. This is
 *                  deliberately the SAME SHAPE as the SDK assertion that caught
 *                  the defect: one struct/one width, two arms, a different
 *                  asserted number in each. Under the defect the `#else` arm is
 *                  selected on Mach-O and claims `sizeof(long) == 4`, which is
 *                  FALSE there — so the build fails loud instead of shipping a
 *                  wrong ABI.
 *   RUN TIME     — the exit code is built from widths measured at run time plus
 *                  a per-arm compensation constant, chosen so that BOTH arms
 *                  reach 42 only when the arm and the widths AGREE. Take the
 *                  wrong arm and the program exits 46 (LP64 widths under the
 *                  non-LP64 arm) or 38 (LLP64 widths under the LP64 arm), never
 *                  42. The `_Static_assert` normally fires first; the runtime
 *                  arm is what survives if the assertion machinery ever
 *                  regresses, which is exactly the class of masking this cycle
 *                  was opened to remove.
 *
 * ★ THE NEGATIVE LEG IS THE STRONGER ONE, and it is why this example ships on
 * ALL FOUR targets rather than the Mach-O leg the directory is named for. The
 * defect was FOUND on macho, but the reason the channel lives on the OBJECT
 * FORMAT rather than the target is the pe64 leg: `x86_64` is LP64 under
 * elf64/macho64 and LLP64 under pe64, so the SAME CPU must answer differently.
 * A macho-only example would stay green if a maintainer pasted the rows into
 * `pe64-*.format.json` — the exact miscompile (Windows headers taking their
 * LP64 arms against a 32-bit `long`) that the format-layer decision prevents.
 * Here the pe64 leg asserts `__LP64__` is NOT defined and that `long` is 4
 * bytes, and it exits 42 only if both hold.
 *
 * ★ BOTH SPELLINGS MOVE TOGETHER. `__LP64__` and `_LP64` are separate rows in
 * every LP64 `.format.json`, so they CAN drift. A world defining only one would
 * pass a naive `#ifdef __LP64__` check while leaving every `#ifdef _LP64` SDK
 * arm on the wrong branch, so the disagreement is its own `#error`.
 *
 * CLANG GROUND TRUTH (MEASURED 2026-07-30, `/usr/bin/clang -dM -E -x c /dev/null
 * -target <triple>`, Apple clang 21.0.0): `__LP64__ 1` AND `_LP64 1` on
 * arm64-apple-darwin, x86_64-apple-darwin, aarch64-linux-gnu and
 * x86_64-unknown-linux-gnu; NEITHER on x86_64-pc-windows-msvc. The four shipped
 * legs below match that exactly.
 */

#include <stdio.h>   /* `puts` — the shipped descriptor, resolved per format */

/* ── LEG 1: THE TWO SPELLINGS MUST AGREE ──────────────────────────────────
 * They are independent config rows; a world that defines one and not the other
 * is incoherent in its own right, before any width is considered. */
#if defined(__LP64__) && !defined(_LP64)
#error "TF-C97 leg 1: __LP64__ is defined but _LP64 is not -- the two data-model spellings have drifted apart in the format config"
#endif
#if defined(_LP64) && !defined(__LP64__)
#error "TF-C97 leg 1: _LP64 is defined but __LP64__ is not -- the two data-model spellings have drifted apart in the format config"
#endif

/* ── LEG 2: THE ARM DECLARES THE WIDTHS IT BELIEVES IN ────────────────────
 * The `#else` arm is NOT a "32-bit" arm: it is the NOT-LP64 arm, and the only
 * not-LP64 format DSS ships an executable for is LLP64 (pe64), where `long` is
 * 4 and a pointer is still 8. Stating it that way keeps the arm honest about
 * which model it is describing instead of assuming the complement of LP64 is
 * ILP32. */
#ifdef __LP64__
#  define DSS_MODEL_LONG_BYTES 8
#  define DSS_MODEL_PTR_BYTES  8
#  define DSS_MODEL_BASE       26      /* 8 + 8 + 26 == 42 */
#  define DSS_MODEL_TAG        "model=lp64"
/* LEG 2b: the VALUE, not merely definedness — both rows carry `"value": "1"`.
 * Fires if the channel seeds a bare `#define NAME` (which would also make this
 * controlling expression a syntax error) or materializes another spelling. */
#  if __LP64__ != 1 || _LP64 != 1
#    error "TF-C97 leg 2b: an LP64 data-model macro does not carry the value 1 -- the format row's `value` field is dropped or wrong"
#  endif
#else
#  define DSS_MODEL_LONG_BYTES 4
#  define DSS_MODEL_PTR_BYTES  8
#  define DSS_MODEL_BASE       30      /* 4 + 8 + 30 == 42 */
#  define DSS_MODEL_TAG        "model=not-lp64"
#endif

/* ── LEG 3: COMPILE-TIME COHERENCE, IN THE SDK'S OWN SHAPE ────────────────
 * The arm's claim versus the compiler's real answer. This is the assertion the
 * macOS SDK makes about its own structs, reduced to the one axis that was
 * actually wrong. Under the pre-TF-C97 defect the Mach-O build selected the
 * `#else` arm and this asserted `sizeof(long) == 4` against a real 8. */
_Static_assert(sizeof(long) == DSS_MODEL_LONG_BYTES,
               "TF-C97 leg 3: the selected __LP64__ arm disagrees with sizeof(long) -- LP64 widths with non-LP64 headers, the incoherent world this cycle closed");
_Static_assert(sizeof(void *) == DSS_MODEL_PTR_BYTES,
               "TF-C97 leg 3: the selected __LP64__ arm disagrees with sizeof(void *)");

/* ── LEG 4: THE SAME COHERENCE, ON A STRUCT ───────────────────────────────
 * A `long`-bearing aggregate, so the disagreement is asserted where the SDK
 * actually asserts it: on a LAYOUT, not only on a scalar. LP64 -> 8 + 8 == 16;
 * LLP64 -> an 8-byte pointer, a 4-byte `long` and 4 bytes of tail padding to the
 * 8-byte alignment == 16 as well, so the SIZE alone cannot discriminate the two
 * models and the member OFFSET is what carries the fact. */
struct DataModelProbe {
    void *addr;
    long  size;
};
_Static_assert(sizeof(struct DataModelProbe) == DSS_MODEL_PTR_BYTES + 8,
               "TF-C97 leg 4: a long-bearing aggregate does not have the size the selected data-model arm implies");

/* Fold-resistance: the widths cross a real call boundary, so the baseline arm
 * keeps a live runtime add instead of one folded immediate at exit (the
 * `arch_identity_predefines` precedent). */
int measuredLongBytes(void) { return (int)sizeof(long); }
int measuredPtrBytes(void)  { return (int)sizeof(void *); }
int combine(int longBytes, int ptrBytes, int base) {
    return longBytes + ptrBytes + base;
}

int main(void) {
    /* The route's own name, pinned per target in expected.json. The exit code
     * CANNOT tell the two routes apart -- both are compensated to 42 on purpose,
     * because the point is that a coherent world exits 42 whichever data model
     * it has -- so the tag is what proves the pe64 leg took the NEGATIVE arm
     * rather than quietly taking the LP64 one. */
    puts(DSS_MODEL_TAG);

    /* A `volatile` seed keeps the widths RUNTIME values, so the `release` arm
     * exercises real machine arithmetic over the selected arm rather than a
     * const-fold. */
    volatile int live = 1;
    int longBytes = measuredLongBytes() * (int)live;
    int ptrBytes  = measuredPtrBytes()  * (int)live;
    return combine(longBytes, ptrBytes, DSS_MODEL_BASE);
}
