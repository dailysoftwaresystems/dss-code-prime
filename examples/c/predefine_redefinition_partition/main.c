/* D-PP-PREDEFINE-REDEFINITION-PARTITION — the RUNTIME witness.
 *
 * A program `#define`s or `#undef`s a name this implementation predefined.
 * ★★ IT ALWAYS TAKES EFFECT; the only question is whether it is DIAGNOSED.
 *
 * C23 6.10.10.1p2 says such a name "shall not be the subject of a #define or a
 * #undef" — but 6.10.10 carries NO `Constraints` heading (6.10.5 Macro
 * replacement does), so C23 4p2 makes the violation UNDEFINED BEHAVIOUR, and
 * 5.1.1.3 requires a diagnostic only for a syntax-rule or CONSTRAINT violation.
 * Nothing is required. ✔MEASURED 2026-08-26 on gcc 13.3.0 and clang 18.1.3,
 * probed SEPARATELY: both ACCEPT and APPLY every case below, warning for the
 * ISO/derived names and staying silent for the implementation-supplied ones.
 * DSS used to refuse all of them outright.
 *
 * ★ WHY ALMOST EVERY VALUE IS DERIVED FROM THE MACRO'S OWN ORIGINAL. The
 * obvious spelling — `#define __BYTE_ORDER__ 4321` then `== 4321` — is a check
 * that can pass for the wrong reason on a target whose real byte order already
 * equals the literal (the trap a sibling lane hit this same cycle). Deriving
 * the new value from the old (`+7`, `+76`, `+1000`) makes each check
 * discriminating on every target DSS has or will have, big-endian included, and
 * every arm ALSO asserts the result DIFFERS from the original, so no arm is
 * vacuous. The one literal is `__LINE__`'s 4242: a derived line number is not
 * meaningful, and this file is nowhere near 4242 lines, so no genuine `__LINE__`
 * can produce it.
 *
 * ★ RED-ON-DISABLE BY CONSTRUCTION. Before this row, EVERY `#define`/`#undef`
 * here was a hard P_PreprocessorPredefinedMacro error, so this file did not
 * compile at all. Restore the refusal and the example breaks the BUILD rather
 * than shifting its score.
 *
 * ⓘ Deliberately no `if (enumConstant)` anywhere: an enum-typed value used
 * directly as a controlling expression is refused today by
 * `I_TerminatorTypeMismatch` (see the lane report's separate production
 * finding). Plain `int` is used instead — that is a limitation being reported,
 * not a style choice.
 *
 * exit = 1 + 2 + 4 + 8 + 16 + 32 + 64 + 128 = 255
 */

/* Freeze the shipped values BEFORE touching any name. */
static const int kOriginalByteOrder = __BYTE_ORDER__;
static const int kOriginalGnuc      = __GNUC__;
static const int kOriginalStdc      = __STDC__;

/* (1) A predefine this program has not touched still materializes to its
 * configured value. ⚠ Deliberately `> 0` and NOT `>= 202311L`: the reference
 * differential for this example compares DSS against gcc and clang on the SAME
 * source, and gcc 13.3 reports `__STDC_VERSION__` as 202000L even under
 * `-std=c2x`, so a final-C23 literal here would make the example score 254 on
 * gcc for a reason that has nothing to do with the subject under test. An arm
 * that diverges on an unrelated axis is an arm that will be misread. */
#if __STDC_VERSION__ > 0
#  define RESERVED_STILL_LIVE 1
#else
#  define RESERVED_STILL_LIVE 0
#endif

/* ══ the IMPLEMENTATION-SUPPLIED half — accepted SILENTLY by both references ══ */

#undef __GNUC__
/* (2) the `#undef` really removed the name. */
#ifdef __GNUC__
#  define UNDEF_TOOK_EFFECT 0
#else
#  define UNDEF_TOOK_EFFECT 1
#endif
/* (3) `#if defined()` must agree with `#ifdef` — one predicate backs both in
 * this preprocessor, and a partition that moved only one would show up here. */
#if defined(__GNUC__)
#  define DEFINED_AGREES 0
#else
#  define DEFINED_AGREES 1
#endif

#define __GNUC__ (kOriginalGnuc + 7)
/* (4) redefining brought the name back. */
#ifdef __GNUC__
#  define REDEFINE_RESTORED 1
#else
#  define REDEFINE_RESTORED 0
#endif

/* A second implementation-supplied name, so (5) is a CLASS and not one
 * special-cased row. */
#undef __BYTE_ORDER__
#define __BYTE_ORDER__ (kOriginalByteOrder + 1000)

/* ══ the ISO / ENGINE-DERIVED half — accepted with a WARNING by both ══════════
 * ✔MEASURED: `#undef __STDC__` + `#define __STDC__ 77` prints 77, and
 * `#undef __LINE__` + `#define __LINE__ 4242` prints 4242, on gcc AND clang. */

#undef __STDC__
#define __STDC__ (kOriginalStdc + 76)

#undef __LINE__
#define __LINE__ 4242

/* Fold-resistant: every compared value arrives through a function ARGUMENT, so
 * the `release` (shippedPipeline) arm proves the redefined values survive
 * inlining and the full optimizer rather than being folded at parse time. */
static int eq(int a, int b) { return a == b ? 1 : 0; }

int main(void) {
    int score = 0;

    if (RESERVED_STILL_LIVE) score += 1;
    if (UNDEF_TOOK_EFFECT)   score += 2;
    if (DEFINED_AGREES)      score += 4;
    if (REDEFINE_RESTORED)   score += 8;

    /* (5) the redefined value is the NEW one AND differs from the original.
     * Both halves are asserted: `== original + 7` alone would still pass if
     * the engine had produced the original and 7 were somehow 0. */
    if (eq(__GNUC__, kOriginalGnuc + 7) && !eq(__GNUC__, kOriginalGnuc)) {
        score += 16;
    }
    if (eq(__BYTE_ORDER__, kOriginalByteOrder + 1000)
        && !eq(__BYTE_ORDER__, kOriginalByteOrder)) {
        score += 32;
    }

    /* (7) an ISO 6.10.10 name: warned about, and APPLIED. */
    if (eq(__STDC__, kOriginalStdc + 76) && !eq(__STDC__, kOriginalStdc)) {
        score += 64;
    }

    /* (8) an ENGINE-DERIVED name: the static replacement now wins over the
     * line-map value. 4242 cannot be a real line number in this file. */
    if (eq(__LINE__, 4242)) score += 128;

    return score;
}
