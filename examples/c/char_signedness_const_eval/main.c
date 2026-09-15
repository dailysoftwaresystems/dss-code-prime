/* D-CSUBSET-CONST-EVAL-CHAR-SIGNEDNESS + D-CSUBSET-CHAR-HIGHBYTE-ICE-SIGNEDNESS:
 * the COMPILE-TIME twin of examples/c/char_signedness, which witnesses only the
 * runtime half. C 6.2.5p15 leaves plain `char`'s signedness implementation-
 * defined and the TARGET declares it; C 6.4.4.4p10 then makes the VALUE of a
 * high-byte character constant follow that declaration. So `'\xff'` is -1 where
 * plain `char` is signed and +255 where it is unsigned, and the question
 * `'\xff' < 0` must get the SAME answer in every tier that can be asked it.
 *
 * ★★ THE POINT OF THIS EXAMPLE IS THAT FIVE TIERS ANSWER IT AND ONLY ONE OF
 * THEM USED TO READ THE DECLARATION. At b1f31420 the runtime tier was right and
 * the compile-time tiers were wrong in TWO OPPOSITE DIRECTIONS, so no single
 * hand-written number could have caught them:
 *   * the character-constant decoders (`#if` fold, const-expr fold, value
 *     lowering) took the raw CODE UNIT, i.e. always UNSIGNED — right on
 *     arm64-linux, wrong on every signed-`char` leg;
 *   * the const-expr CAST classifier hard-coded `char` as 8-bit SIGNED — right
 *     on the signed legs, wrong on arm64-linux;
 *   * the HIR/MIR arithmetic core modelled `char` as 32-bit UNSIGNED — wrong in
 *     BOTH fields on EVERY leg, and its wrong WIDTH masked its wrong SIGN.
 * Every one of those compiled rc 0 with no diagnostic at all.
 *
 * ✔MEASURED at b1f31420, references probed SEPARATELY, each control matching its
 * own target (gcc 13.3.0, clang 18.1.3, aarch64-linux-gnu-gcc 13.3.0 through
 * qemu-aarch64, and MSVC 19.51 cl.exe /c with a known-false control arm that
 * errors C2118 so its clean rc is not vacuous):
 *   * `int a[('\xff' < 0) ? 1 : 2];` — DSS built TWO elements on x86_64 where
 *     gcc and clang build one;
 *   * `#if '\xff' < 0` — DSS took the `#else` arm on x86_64 where gcc and clang
 *     take the `#if` arm;
 *   * `static int g = (char)300; g == 44` — DSS answered NO on BOTH legs where
 *     all three references answer yes;
 *   * `int a[((char)200 == -56) ? 1 : 2];` — DSS built ONE element on
 *     arm64-linux where aarch64-linux-gnu-gcc builds two.
 *
 * ── HOW TO READ THE EXIT CODE ────────────────────────────────────────────────
 * 150 plus one weighted bit per tier, so a regression in ANY SINGLE tier moves
 * the number by a distinct amount instead of being absorbed by its siblings:
 *
 *   +16  RUNTIME          `volatile` seed -> `char c; c < 0`. The machine
 *                         char->int extension (SExt vs ZExt). Fails to 0 if the
 *                         byte load / promotion stops consulting the target.
 *   + 8  ICE              an ARRAY DIMENSION sized by `('\xff' < 0) ? 2 : 1`.
 *                         Fails to 0 if the const-expr char leaf goes back to
 *                         the raw code unit — which is what it did.
 *   + 4  PREPROCESSOR     `#if '\xff' < 0`. Fails to 0 if the `#if` fold stops
 *                         being told the target's answer. This arm is the one
 *                         that made the row a P0: a wrong `#if` compiles a
 *                         DIFFERENT PROGRAM, silently.
 *   + 2  STATIC INIT      `static int gsign = '\xff';` compared against -1. The
 *                         HIR const-eval / global-initializer path, which is a
 *                         different evaluator from the two above.
 *   + 1  WIDTH            `static int gwide = (char)300;` compared against 44.
 *                         TARGET-INDEPENDENT — 300 truncated to ANY 8-bit
 *                         `char` is 44 under either signedness — so this bit is
 *                         1 on EVERY leg and it is the arm that is NOT vacuous
 *                         on arm64-linux. Fails to 0 if `char` goes back to
 *                         being modelled 32 bits wide.
 *
 * ⚠⚠ WHY THE arm64 x elf ROW IS DELIBERATELY DIFFERENT, AND WHY IT IS STILL
 * WORTH RUNNING. On an unsigned-`char` leg the buggy answer and the correct
 * answer to `'\xff' < 0` COINCIDE, so the four signedness bits above are
 * VACUOUS THERE BY CONSTRUCTION — no test on that leg can ever see the
 * signedness defect. That is exactly why the defect survived, and it is why the
 * expectation for that row is 151 rather than a copy of the others. The WIDTH
 * bit is what keeps the row non-vacuous, and the row's own value is what proves
 * the fix is CONFIG-DRIVEN rather than a hard-coded flip: make the compiler
 * assume signed everywhere and this leg reads 181 instead of 151.
 *
 * ★ THE PER-LEG EXPECTATION IS DERIVED FROM THE ONE DECLARATION, NOT WRITTEN
 * TWICE. The target configuration declares plain `char`'s signedness per
 * (architecture x object format) in a single place; every row in expected.json
 * is that resolved answer and nothing else. This example does NOT restate the
 * spelling of the key that carries it — the declaration has been reshaped once
 * already and these rows must keep asserting the same OBSERVABLE behaviour
 * across the next reshape.
 *
 * ★ THE `release` ARM IS LOAD-BEARING. Three of the five bits are compile-time
 * constants, so the optimizer's own ConstFold sees them; the runtime bit's
 * `volatile` seed survives to machine code. Both arms must produce the SAME
 * number on a given leg: the const-eval tiers and the codegen tier are separate
 * implementations of one rule, and this example fails if optimizing changes the
 * answer — which is precisely how a target-blind ConstFold would surface.
 *
 * All values are clear of the smoke-pin 42 and of the 131/132 the sibling
 * char_value uses, so attribution falls on this example. */

/* The STATIC-INITIALIZER tier: folded by the HIR const-eval, a different
 * evaluator from the array-dimension one below. `gsign` is target-keyed;
 * `gwide` is not — 300 truncated to eight bits is 44 either way. */
static int gsign = '\xff';
static int gwide = (char)300;

int main(void) {
    /* PREPROCESSOR tier (C 6.10.1p4 + 6.4.4.4p10). */
#if '\xff' < 0
    int const pp = 1;
#else
    int const pp = 0;
#endif

    /* INTEGER-CONSTANT-EXPRESSION tier: the array's LENGTH is the answer. */
    int ice[('\xff' < 0) ? 2 : 1];
    ice[0] = 0;

    /* RUNTIME tier: the `volatile` seed keeps the byte out of reach of every
     * const-fold, so this bit exercises the real machine char->int extension. */
    volatile int seed = 0xff;
    char c = (char)seed;

    return 150
         + 16 * (c < 0 ? 1 : 0)
         +  8 * ((sizeof(ice) / sizeof(ice[0]) == 2) ? 1 : 0)
         +  4 * pp
         +  2 * (gsign == -1 ? 1 : 0)
         +  1 * (gwide == 44 ? 1 : 0);
}
