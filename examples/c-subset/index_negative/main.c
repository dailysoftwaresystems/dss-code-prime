/* c42 (D-CSUBSET-INDEX-NEGATIVE-WIDEN) + c93 (D-AS4-ARM64-NEGATIVE-DISP-LEA)
 * C 6.5.2.1/6.5.6: a NEGATIVE array subscript `p[-n]` = *(p + (-n)) must step
 * -n * sizeof(*p) bytes.
 *
 * TWO distinct frontiers converge here, so this corpus now runs on the
 * BASELINE (debug) *and* the RELEASE / full-release-like arms (all 4 targets):
 *
 *   c42 (the RUNTIME path, DEBUG witness): a 32-bit `int` subscript scaled by a
 *   runtime stride Mul reaches the 64-bit `lea`/ADD as a SUB-64-bit value; a
 *   negative one (high bit set) was consumed UN-widened -> upper 32 bits garbage
 *   -> a +4GiB address -> SIGSEGV. lowerGep sign-extends the runtime index. The
 *   variable-subscript cases exercise this via the register path on every target
 *   (incl. arm64 SXTW under qemu).
 *
 *   c93 (the CONSTANT-disp path, RELEASE witness): under ConstFold a constant
 *   `p[-N]` folds to a bare NEGATIVE Const byte-offset -> the const-disp Gep ->
 *   `lea [base + MemOffset(<0)]`. x86 encodes that as a hardware-sign-extended
 *   `lea [base+disp32]` (one instruction). arm64's every base+disp lea/ADD-imm
 *   variant is UNSIGNED-magnitude-bounded, so a negative disp matched NO variant
 *   (A_NoMatchingEncodingVariant opcode 'lea' width 64) and the function was
 *   dropped -- THE gap this corpus used to dodge by declaring no optimized arm.
 *   c93's config-gated fold (targetHasSignedDispLea() false on arm64)
 *   materializes the negative disp into a fresh GPR and emits the base+index
 *   `ADD Xd,Xn,Xm` (Xm = sign-extended -N) = base - N. So the RELEASE arms are
 *   now declared and GREEN on arm64 too. sqlite uses `p[-1]` lookback ->
 *   run-green-critical.
 *
 * Covers stride 1 (char*, no Mul), 4 (int*), and 12 (struct Trip*, a non-pow2
 * Mul -> the canonical -1*12 = 0xFFFFFFF4 offset), each with a CONSTANT
 * subscript AND a RUNTIME subscript, PLUS constant-negative-index STOREs (int +
 * char) that round-trip the written bytes (value-verifying the fold writes the
 * element BEFORE the base). The pointers are set mid-array via `base + k`
 * (c41's now-correct pointer+int) so the setup is decoupled from the
 * negative-subscript path under test. Each failing sub-case returns its own
 * code; exit 42 iff all pass.
 *
 * RED-ON-DISABLE:
 *   - revert the lowerGep c42 index-widen -> the un-widened I32 negative offset
 *     zero-extends in the 64-bit lea -> a +4GiB address -> SIGSEGV (non-42) on
 *     the first runtime-stride-Mul subscript (debug).
 *   - revert the c93 negative-disp fold -> the arm64 RELEASE arm fails to
 *     compile with `A_NoMatchingEncodingVariant opcode 'lea' width 64` on the
 *     first constant `p[-N]` (x86 stays green via signed disp32). */
struct Trip { int x; int y; int z; };   /* sizeof 12 -> a non-pow2 stride */

int main(void) {
    int a[6];
    a[0] = 10; a[1] = 20; a[2] = 30; a[3] = 40; a[4] = 50; a[5] = 60;
    int *abase = a;                  /* array decay to a pointer value */
    int *p = abase + 4;              /* pointer+int (c41): point mid-array */

    /* int* (stride 4), CONSTANT negative subscript: in debug the `-k * 4`
     * stride Mul is a runtime vreg -> the crash path pre-fix. */
    if (p[-1] != 40) return 1;       /* a[3] */
    if (p[-4] != 10) return 2;       /* a[0] */

    /* int* (stride 4), RUNTIME negative subscript: a variable index -> the
     * runtime path in debug AND release. */
    int k = -2;
    if (p[k] != 30) return 3;        /* a[2] */
    int k3 = -3;
    if (p[k3] != 20) return 4;       /* a[1] */

    /* char* (stride 1): the index IS the byte offset (no Mul). The CONSTANT
     * case folds to a disp (already correct); the RUNTIME case hits the
     * un-widened runtime path. */
    char buf[4];
    buf[0] = 'A'; buf[1] = 'B'; buf[2] = 'C'; buf[3] = 'D';
    char *bbase = buf;               /* decay */
    char *cp = bbase + 3;
    if (cp[-1] != 'C') return 5;
    int ci = -3;
    if (cp[ci] != 'A') return 6;

    /* struct* (stride 12, non-pow2): the canonical -1*12 = 0xFFFFFFF4 case. */
    struct Trip trips[4];
    trips[0].x = 1; trips[1].x = 3; trips[2].x = 5; trips[3].x = 7;
    struct Trip *tbase = trips;      /* decay */
    struct Trip *sp = tbase + 3;
    if (sp[-1].x != 5) return 7;     /* trips[2] */
    int si = -3;
    if (sp[si].x != 1) return 8;     /* trips[0] */

    /* c93 (D-AS4-ARM64-NEGATIVE-DISP-LEA): a CONSTANT-negative-index STORE
     * round-trip. Under the RELEASE/full-release-like arms ConstFold folds
     * each `p[-N]` to a bare negative Const byte-offset -> the const-disp Gep
     * -> `lea [base + MemOffset(<0)]`. On arm64 that has no encoding; the c93
     * fold materializes the negative disp into a fresh GPR + base+index ADD.
     * Writing THROUGH the negative index and reading the SAME element back
     * value-verifies the written bytes land on the element BEFORE the base
     * (int stride-4 and char stride-1 store legs). */
    p[-2] = 999;                     /* store to a[2] via a negative const */
    if (a[2] != 999) return 9;       /* the store landed on the right element */
    if (p[-2] != 999) return 10;     /* the negative-index READ agrees */
    cp[-2] = 'Z';                    /* char store to buf[1] via -2 */
    if (buf[1] != 'Z') return 11;
    if (cp[-2] != 'Z') return 12;

    return 42;
}
