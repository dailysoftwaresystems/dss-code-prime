/* C23 6.4.4.1 — the BARE ZERO bit-precise literal `0wb` / `0uwb`
 * (D-CSUBSET-BITINT-ZERO-LITERAL-EATEN-BY-THE-OCTAL-PREFIX).
 *
 * THE DEFECT THIS EXAMPLE EXISTS TO PIN. C spells the octal prefix `0`, so for the
 * literal `0` the longest-declared-prefix scan consumed the ONLY digit and left an
 * EMPTY digit body. The two sibling decoders of that one grammar then disagreed
 * about what an empty body means: `decodeInteger` has no "no digits" verdict and
 * returned 0, while `decodeBigInteger` — the `wb`/`uwb` path — reported nullopt,
 * which its caller renders as S_IntegerLiteralTooLarge. So DSS refused a
 * well-formed ZERO with "no declared type can hold it", MEASURED rc=1 in EVERY
 * context: initializer, cast, arithmetic operand, file-scope initializer, array
 * dimension, enumerator and case label.
 *
 * ⚠ ONLY THE BARE DECIMAL SPELLING FAILED, which is what makes this worth a corpus
 * example rather than a unit test alone: `00wb`, `0x0wb`, `0b0wb` and `010wb` all
 * compiled and ran correctly, so every neighbouring spelling in the ladder looked
 * healthy. The break was invisible from anywhere except the one shape a programmer
 * is most likely to write.
 *
 * REFERENCE GROUND TRUTH, probed separately: clang 18.1.3 `-std=c23` compiles and
 * runs every line below (`0wb`, `0uwb`, `0WB`, `0Uwb`, `0wbu`, and `010wb` as EIGHT).
 * gcc 13.3.0 has no `_BitInt` at all (`-std=c2x` reports `implicit declaration of
 * function '_BitInt'`), so clang alone is the witness here and that is sufficient
 * under `DSS = (gcc ∪ clang ∪ MSVC) ∪ ISO C` — the DISJUNCTION.
 *
 * EXIT 42 IS LOAD-BEARING AND RED-ON-DISABLE. Every term below is exercised, and
 * the arithmetic is chosen so that the fix's TWO failure modes are distinguishable
 * rather than both landing on "not 42":
 *   * revert the decoder and the program does not COMPILE at all (S0012 on `0wb`);
 *   * "fix" it by returning zero for any empty body — the tempting shortcut — and
 *     `oct` below decodes wrong, because that shortcut cannot tell the standalone
 *     octal `0` from a prefix whose digits were thrown away.
 * A `volatile` seed keeps the whole computation runtime-dependent so the release
 * optimizer cannot fold the example into a constant and pass vacuously.
 *
 *   z(0) + u(0) + wide(0) + oct(8) + hi(0) + arr[1] slot + enum(0) + case(0)
 *   => 8 + 34 = 42
 */

enum ZeroEnum { ZERO_E = (int)0wb };            /* enumerator const-expr path */

static _BitInt(17) g_signed_zero = 0wb;         /* file-scope initializer path */
static unsigned _BitInt(17) g_unsigned_zero = 0uwb;

/* Array dimension: a constant expression evaluated by the const-eval tier, a
 * DIFFERENT caller of the same decoder — masked before the fix only because the
 * semantic tier reported first. */
static int g_arr[1 + (int)0wb];

int main(void) {
    volatile int seed = 1;

    _BitInt(17)          z    = 0wb;            /* the shape that was refused */
    unsigned _BitInt(17) u    = 0uwb;
    _BitInt(17)          upper = 0WB;           /* declared uppercase suffix */
    unsigned _BitInt(17) mixed = 0Uwb;
    unsigned _BitInt(17) tail  = 0wbu;          /* suffix-order twin */

    /* +0 at runtime (seed is volatile) so nothing folds away. */
    z = z + (_BitInt(17))(seed - 1);

    /* A WIDE zero: the bignum path with more than one limb's worth of type. */
    unsigned _BitInt(100) wide = 0uwb;
    wide = wide + (unsigned _BitInt(100))(seed - 1);

    /* THE DISCRIMINATOR between the real fix and the return-zero shortcut:
     * `010wb` is the octal EIGHT. The prefix `0` is consumed as a radix marker
     * here because a body follows it — the standalone reading applies only when
     * removing the prefix would empty the body. */
    _BitInt(17) oct = 010wb;

    /* Case label: a fourth const-expr consumer of the same decoder. */
    int hit = 0;
    switch ((int)z) {
        case (int)0wb: hit = 1; break;
        default:       hit = 9; break;
    }

    g_arr[0] = (int)g_signed_zero + (int)g_unsigned_zero;   /* both file-scope */

    int total = (int)z + (int)u + (int)upper + (int)mixed + (int)tail
              + (int)wide + (int)oct + g_arr[0] + (int)ZERO_E;
    /* total == 8 (oct) — every other term is a zero that had to survive. */
    return total + 33 + hit;                                 /* 8 + 33 + 1 == 42 */
}
