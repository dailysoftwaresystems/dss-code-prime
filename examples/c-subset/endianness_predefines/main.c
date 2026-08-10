/*
 * TF-C115 (D-PP-ENDIANNESS-PREDEFINES) — the runnable witness for the
 * byte-order predefined macros.
 *
 * WHAT IS DECLARED WHERE, and why this example has to span two config layers:
 *
 *   LANGUAGE  (src/dss-config/sources/c-subset.lang.json)
 *       __ORDER_LITTLE_ENDIAN__ 1234 / __ORDER_BIG_ENDIAN__ 4321 /
 *       __ORDER_PDP_ENDIAN__ 3412 — the NAMING VOCABULARY. MEASURED identical
 *       on every triple DSS targets AND on the big-endian aarch64_be, so they
 *       vary by nothing and live on the layer where they are invariant.
 *
 *   TARGET    (src/dss-config/targets/<arch>.target.json)
 *       __LITTLE_ENDIAN__ 1 and __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__ — the
 *       per-CPU ANSWER. Endianness varies by CPU but NOT across the object
 *       formats one target serves, which is exactly the test __LP64__ failed
 *       (it had to move to the format), so these stay on the target, UNGATED.
 *
 *   NOWHERE
 *       __BIG_ENDIAN__. DSS ships no big-endian target; MEASURED, clang
 *       defines it only for aarch64_be-linux-gnu. Its ABSENCE is a
 *       declaration, and leg 3 below is what keeps it absent.
 *
 * ★ WHY __BYTE_ORDER__ IS INTERESTING AT ALL: its value is a MACRO REFERENCE,
 * not a literal. Both target files used to record it as unshippable for that
 * reason ("needs a different mechanism than a literal Constant body"). The
 * claim was never tested and is false — a materialized predefine is spliced
 * over its invocation and RESCANNED like any other replacement — so leg 2
 * below is also the standing proof that the reference body resolves.
 *
 * ★ NON-VACUITY. Five INDEPENDENTLY FALSIFIABLE compile-time legs plus a
 * runtime arm. Every leg is red on its own; the observed red-on-disable matrix
 * is recorded in expected.json's $comment.
 *
 *   LEG 1  VOCABULARY   — the three __ORDER_* names exist and carry their
 *                         measured values. Red if the language rows are gone
 *                         or mis-valued.
 *   LEG 2  REFERENCE    — __BYTE_ORDER__ exists and its body RESOLVES, both
 *                         against the name it references and against that
 *                         name's literal value. Red if the target row is gone,
 *                         and red if a materialized Constant body ever stops
 *                         rescanning.
 *   LEG 3  NEGATIVE     — __BIG_ENDIAN__ is NOT defined, and __BYTE_ORDER__
 *                         does NOT compare equal to __ORDER_BIG_ENDIAN__.
 *                         ★ THIS IS THE LEG THAT MATTERS. Before TF-C115 the
 *                         second half was TRUE on every leg (0 == 0 for two
 *                         undefined identifiers, C 6.10.1p4), i.e. a
 *                         big-endian-first ladder silently took the wrong arm
 *                         on a little-endian machine. It is red again the
 *                         moment either declaration layer is dropped.
 *   LEG 4  APPLE SPELLING — __LITTLE_ENDIAN__ is defined, with value 1.
 *                         This is the spelling Apple's libkern/OSByteOrder.h
 *                         requires; without it that header is a bare
 *                         `#error Unknown endianess.`.
 *   LEG 5  UNGATED      — the two target rows reach EVERY object format, not
 *                         just Mach-O. Asserted by cross-checking against
 *                         __APPLE__, which IS macho-gated: whichever side of
 *                         that gate this TU is on, the endianness macros must
 *                         still be present. Red if either row acquires an
 *                         availableObjectFormats filter.
 *
 * The macro names below are the C-source side of a config fact; the compiler
 * never hardcodes them (the merge compares config strings to config strings).
 */

#include <stdio.h>   /* `puts` — the shipped descriptor, resolved per format */

/* ── LEG 1: THE VOCABULARY, BY VALUE ──────────────────────────────────────
 * MEASURED 2026-08-04 (`clang-19 -dM -E -x c /dev/null -target <triple>`):
 * 1234 / 4321 / 3412 on arm64-apple-darwin, x86_64-apple-darwin,
 * aarch64-linux-gnu, x86_64-unknown-linux-gnu, x86_64-pc-windows-msvc and
 * aarch64_be-linux-gnu. Definedness alone is not enough: a program that
 * compares against these numbers needs the numbers. */
#if !defined(__ORDER_LITTLE_ENDIAN__) || !defined(__ORDER_BIG_ENDIAN__) \
 || !defined(__ORDER_PDP_ENDIAN__)
#error "TF-C115 leg 1: the __ORDER_* byte-order vocabulary is not predefined -- the language config's rows are not reaching the preprocessor"
#endif
#if __ORDER_LITTLE_ENDIAN__ != 1234 || __ORDER_BIG_ENDIAN__ != 4321 \
 || __ORDER_PDP_ENDIAN__ != 3412
#error "TF-C115 leg 1: an __ORDER_* constant does not carry its measured value (1234/4321/3412)"
#endif

/* ── LEG 2: THE MACRO-REFERENCE BODY RESOLVES ─────────────────────────────
 * `__BYTE_ORDER__`'s configured value is the TEXT `__ORDER_LITTLE_ENDIAN__`.
 * Both comparisons must hold: against the referenced NAME (proving the value
 * is that name) and against its LITERAL (proving the name then expands). A
 * predefine that materialized inertly would fail the literal half. */
#if !defined(__BYTE_ORDER__)
#error "TF-C115 leg 2: __BYTE_ORDER__ is not predefined -- the target config's row is missing"
#endif
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "TF-C115 leg 2: __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__ -- the reference body did not resolve to the vocabulary name"
#endif
#if __BYTE_ORDER__ != 1234
#error "TF-C115 leg 2: __BYTE_ORDER__ does not expand through to the literal 1234 -- a materialized Constant body stopped rescanning"
#endif

/* ── LEG 3: THE NEGATIVE — NO BIG-ENDIAN CLAIM ANYWHERE ───────────────────
 * DSS ships no big-endian target. Two halves, both required:
 *   (a) the SPELLING must be absent — Apple's libkern/OSByteOrder.h:165 tests
 *       `defined(__BIG_ENDIAN__)` BEFORE the little-endian arm, so a stray row
 *       silently selects byte-swapping macros;
 *   (b) the COMPARISON must be false — this is the half that was WRONG before
 *       TF-C115, when both operands were undefined and `0 == 0` held. */
#if defined(__BIG_ENDIAN__)
#error "TF-C115 leg 3a: __BIG_ENDIAN__ is predefined on a little-endian target -- a big-endian claim leaked into the merged table"
#endif
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#error "TF-C115 leg 3b: __BYTE_ORDER__ compares EQUAL to __ORDER_BIG_ENDIAN__ on a little-endian target -- either the vocabulary or the answer is undefined and `#if` is comparing 0 to 0"
#endif

/* ── LEG 4: THE APPLE SPELLING, BY VALUE ──────────────────────────────────
 * MEASURED: clang defines `__LITTLE_ENDIAN__` as 1 on all five little-endian
 * triples DSS targets (gcc defines no such spelling — it is a clang/Apple
 * one, and DSS already claims `__clang__` unconditionally). */
#if !defined(__LITTLE_ENDIAN__)
#error "TF-C115 leg 4: __LITTLE_ENDIAN__ is not predefined -- Apple's libkern/OSByteOrder.h is a bare `#error Unknown endianess.` without it"
#endif
#if __LITTLE_ENDIAN__ != 1
#error "TF-C115 leg 4: __LITTLE_ENDIAN__ does not carry the value 1"
#endif

/* ── LEG 5: UNGATED ACROSS EVERY OBJECT FORMAT ────────────────────────────
 * `__APPLE__` is macho-gated, so it partitions the shipped legs. The
 * endianness rows carry NO availableObjectFormats, so they must be present on
 * BOTH sides of that partition. Written as two mutually exclusive arms so the
 * assertion is made once per side and neither side can be vacuous. */
#if defined(__APPLE__)
#  if !defined(__LITTLE_ENDIAN__) || !defined(__BYTE_ORDER__)
#    error "TF-C115 leg 5 (macho side): an endianness macro is missing on Mach-O"
#  endif
#  define DSS_ENDIAN_TAG "endian=little-apple"
#else
#  if !defined(__LITTLE_ENDIAN__) || !defined(__BYTE_ORDER__)
#    error "TF-C115 leg 5 (non-macho side): an endianness macro is missing off Mach-O -- the rows acquired an availableObjectFormats gate and no longer reach elf/pe"
#  endif
#  define DSS_ENDIAN_TAG "endian=little"
#endif

/* The runtime half. `byteOrderCode()` returns __BYTE_ORDER__'s VALUE, so leg 2
 * is re-proved at execution: if the reference body ever stopped resolving, the
 * program would compute something other than 1234 even with every `#error`
 * regressed. `endianWitness()` is the INDEPENDENT check — it reads the low
 * byte of a multi-byte object through a `char` lens, so it reports the byte
 * order the CODE GENERATOR actually produced rather than the one the macro
 * table claims. The two are multiplied together, so a disagreement between the
 * declared byte order and the emitted one cannot exit 42. */
int byteOrderCode(void) {
    return __BYTE_ORDER__;
}

/* 1 when the target really stores the least-significant byte first. `volatile`
 * keeps the store->load a real memory round trip, so the `release` arm
 * witnesses the optimizer over this shape instead of const-folding it. */
int endianWitness(void) {
    volatile unsigned int word = 0x01020304u;
    volatile unsigned char const* first = (volatile unsigned char const*)&word;
    return (*first == 0x04u) ? 1 : 0;
}

/* Fold-resistance: the operands cross a real call boundary, so the baseline
 * arm keeps a live runtime multiply-add rather than one folded immediate. */
int combine(int declared, int witnessed, int base) {
    return (declared / 1234) * witnessed * 41 + base;
}

int main(void) {
    /* Pinned per target in expected.json — the observable that tells the
     * macho leg apart from the elf/pe legs, since every leg exits 42. */
    puts(DSS_ENDIAN_TAG);

    volatile int live = 1;
    int declared  = byteOrderCode() * (int)live;   /* 1234 */
    int witnessed = endianWitness();               /* 1 on every shipped leg */
    return combine(declared, witnessed, 1);        /* 1*1*41 + 1 == 42 */
}
