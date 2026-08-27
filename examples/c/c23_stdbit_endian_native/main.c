/*
 * D-FULLC-STDBIT-BIG-ENDIAN-NATIVE — the coherence witness for C23 7.18.2's
 * three endianness macros, and specifically for the fact that the NATIVE one is
 * DERIVED FROM THE TARGET rather than restated in the header descriptor.
 *
 * WHAT WAS WRONG, AND WHY A CORRECT ANSWER STILL NEEDED FIXING. From 2026-07-15
 * `shippedLibs/stdbit.json` carried `__STDC_ENDIAN_NATIVE__` as the literal
 * 1234. Every DSS target is little-endian, so that answer was CORRECT — and it
 * was correct the way a hardcoded constant is correct: by coincidence of the
 * current target set, with nothing anywhere that would make anyone revisit it.
 * The first big-endian target would have reported the wrong native order with no
 * diagnostic, no failing test, and no reason for the person adding that target
 * to look in a header descriptor. It now names `__BYTE_ORDER__`, which every
 * `<arch>.target.json` already declares per-CPU (D-PP-ENDIANNESS-PREDEFINES),
 * so the answer follows the machine automatically.
 *
 * ★ THIS IS ALSO WHAT THE REFERENCES DO, MEASURED not reasoned: `gcc -dM -E` and
 * `clang -dM -E` after `#include <stdbit.h>` BOTH report
 * `#define __STDC_ENDIAN_NATIVE__ __BYTE_ORDER` — glibc likewise derives it
 * from its byte-order predefine instead of writing a number.
 *
 * ★ WHY THE CROSS-LAYER PART IS THE INTERESTING PART. The value travels through
 * THREE config layers before it is an integer: a shipped-library MACRO names a
 * TARGET predefine, whose value names a LANGUAGE predefine, which is finally a
 * literal. Each hop is a rescan of a materialized replacement. Leg 2 is the
 * standing proof that the whole chain resolves; it is red if any layer stops
 * rescanning, and red if any layer's row is dropped.
 *
 * ★ NON-VACUITY. Five INDEPENDENTLY FALSIFIABLE compile-time legs plus a runtime
 * arm; each leg is red on its own.
 *
 *   LEG 1  PRESENCE   — all three C23 names exist and the two CONSTANTS carry
 *                       their standard values. Red if stdbit.json's rows go.
 *   LEG 2  DERIVATION — ★ THE LEG THAT MATTERS, AND ITS FALSIFIABILITY IS NOT
 *                       WHAT IT LOOKS LIKE. NATIVE must compare equal to
 *                       `__BYTE_ORDER__`, i.e. it must be the TARGET's answer.
 *                       ⚠ A C preprocessor can compare macro VALUES and cannot
 *                       inspect a macro's replacement LIST, so on a
 *                       little-endian target this leg CANNOT tell the
 *                       derivation from the literal 1234 — they have the same
 *                       value. ✔MEASURED: reverting stdbit.json to the literal
 *                       leaves this example GREEN on every shipped target, and
 *                       an earlier draft of this very comment claimed otherwise.
 *                       ★ WHAT MAKES IT REAL is the SECOND axis: with the
 *                       target's `__BYTE_ORDER__` mutated to
 *                       `__ORDER_BIG_ENDIAN__`, the DERIVED native follows and
 *                       leg 3 fires, while the LITERAL native does not follow
 *                       and THIS leg fires. Both halves ✔MEASURED; the full
 *                       two-axis matrix is in expected.json's $comment. That is
 *                       the regression a one-axis check cannot see.
 *   LEG 3  NOT-BIG    — NATIVE does not compare equal to BIG. Red if a target
 *                       ever declares a big-endian `__BYTE_ORDER__` without the
 *                       rest of the big-endian work landing with it.
 *   LEG 4  IS-LITTLE  — NATIVE compares equal to LITTLE on every shipped leg.
 *                       Together with leg 3 this brackets the answer from both
 *                       sides, so a NATIVE that resolved to 0 (the value C
 *                       6.10.1p4 gives an undefined identifier — the silent
 *                       failure this whole macro family keeps producing) fails
 *                       BOTH, rather than passing leg 3 by accident.
 *   LEG 5  VOCABULARY — the C23 constants agree with the GNU `__ORDER_*`
 *                       vocabulary they numerically coincide with. That
 *                       coincidence is what makes the derivation legal at all:
 *                       if `__ORDER_BIG_ENDIAN__` ever stopped being 4321 the
 *                       derived NATIVE would be silently wrong on a big-endian
 *                       target, and this leg is what stands in front of that.
 *
 *   RUNTIME           — `endianWitness()` reads the low byte of a `volatile`
 *                       unsigned through a `char` lens, so it reports the byte
 *                       order the CODE GENERATOR actually produced, independent
 *                       of the macro table. It is multiplied into the exit code,
 *                       so a macro table that disagreed with the emitted code
 *                       cannot exit 77.
 */

#include <stdbit.h>

/* ---- LEG 1: presence, and the two constants' standard values ------------- */
#if !defined(__STDC_ENDIAN_LITTLE__)
#error "LEG 1: <stdbit.h> must define __STDC_ENDIAN_LITTLE__ (C23 7.18.2)"
#endif
#if !defined(__STDC_ENDIAN_BIG__)
#error "LEG 1: <stdbit.h> must define __STDC_ENDIAN_BIG__ (C23 7.18.2)"
#endif
#if !defined(__STDC_ENDIAN_NATIVE__)
#error "LEG 1: <stdbit.h> must define __STDC_ENDIAN_NATIVE__ (C23 7.18.2)"
#endif
#if __STDC_ENDIAN_LITTLE__ != 1234
#error "LEG 1: __STDC_ENDIAN_LITTLE__ must be 1234"
#endif
#if __STDC_ENDIAN_BIG__ != 4321
#error "LEG 1: __STDC_ENDIAN_BIG__ must be 4321"
#endif

/* ---- LEG 2: NATIVE is DERIVED from the target, not restated -------------- */
#if !defined(__BYTE_ORDER__)
#error "LEG 2: the target must declare __BYTE_ORDER__ for NATIVE to derive from"
#endif
#if __STDC_ENDIAN_NATIVE__ != __BYTE_ORDER__
#error "LEG 2: __STDC_ENDIAN_NATIVE__ must BE the target's __BYTE_ORDER__, not a literal that happens to match it today"
#endif

/* ---- LEG 3: NATIVE is not BIG on any shipped target ---------------------- */
#if __STDC_ENDIAN_NATIVE__ == __STDC_ENDIAN_BIG__
#error "LEG 3: no shipped DSS target is big-endian"
#endif

/* ---- LEG 4: ...and it IS little, so a NATIVE of 0 fails both sides ------- */
#if __STDC_ENDIAN_NATIVE__ != __STDC_ENDIAN_LITTLE__
#error "LEG 4: every shipped DSS target is little-endian"
#endif

/* ---- LEG 5: the C23 constants agree with the GNU naming vocabulary ------- */
#if !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__)
#error "LEG 5: the language must declare the __ORDER_* naming vocabulary"
#endif
#if __STDC_ENDIAN_LITTLE__ != __ORDER_LITTLE_ENDIAN__
#error "LEG 5: C23 LITTLE and GNU __ORDER_LITTLE_ENDIAN__ must agree"
#endif
#if __STDC_ENDIAN_BIG__ != __ORDER_BIG_ENDIAN__
#error "LEG 5: C23 BIG and GNU __ORDER_BIG_ENDIAN__ must agree - the derivation in leg 2 is only sound while they do"
#endif

/* ---- RUNTIME: the byte order the CODE GENERATOR produced ----------------- */
static int endianWitness(void) {
    volatile unsigned probe = 0x01020304u;
    /* Reading the object's first byte through a character lens is the one
     * aliasing C explicitly permits, and it answers with the machine's real
     * byte order rather than with anything the preprocessor believes. */
    unsigned char const first = *(volatile unsigned char const *)&probe;
    return first == 0x04u ? 1 : 0;   /* 1 == little-endian */
}

int main(void) {
    /* 77 only if the macro table AND the emitted code agree. */
    return 77 * endianWitness();
}
