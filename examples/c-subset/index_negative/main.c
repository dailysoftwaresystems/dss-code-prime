/* c42 (D-CSUBSET-INDEX-NEGATIVE-WIDEN) C 6.5.2.1/6.5.6: a NEGATIVE array
 * subscript `p[-n]` = *(p + (-n)) must step -n * sizeof(*p) bytes. The Gep
 * index (a 32-bit `int` subscript, possibly scaled by a stride Mul) reaches the
 * 64-bit `lea`/ADD as a SUB-64-bit value; a negative one (high bit set) was
 * consumed UN-widened, so its upper 32 bits read as garbage -> a huge positive
 * address -> SIGSEGV. The crash is DEBUG-only: release folds a constant
 * `Mul(-1, stride)` to a Const that takes the disp32 path (hardware
 * sign-extends disp32), masking it -- so the BASELINE (debug) run is the
 * witness. (On arm64 that folded negative displacement has no `lea` encoding
 * -- a PRE-EXISTING gap, deferred D-AS4-ARM64-NEGATIVE-DISP-LEA / c43 -- so
 * this corpus declares NO optimized arm and witnesses via baseline on all 4
 * targets; the runtime variable-subscript cases still exercise the widen via
 * the register path, incl. arm64 SXTW under qemu.) sqlite uses `p[-1]`
 * lookback, so this is run-green-critical.
 *
 * Covers stride 1 (char*, no Mul), 4 (int*), and 12 (struct Trip*, a non-pow2
 * Mul -> the canonical -1*12 = 0xFFFFFFF4 offset), each with a CONSTANT
 * subscript (debug: a runtime stride Mul -> the crash) AND a RUNTIME subscript
 * (a variable index -> the runtime path in debug AND release). The pointers are
 * set mid-array via `base + k` (c41's now-correct pointer+int) so the setup is
 * decoupled from the c42 negative-subscript path under test. Each failing
 * sub-case returns its own code; exit 42 iff all pass.
 *
 * RED-ON-DISABLE: revert the lowerGep c42 index-widen -> the un-widened I32
 * negative offset zero-extends in the 64-bit lea -> a +4GiB address -> SIGSEGV
 * (a crash, i.e. a non-42 exit) on the first runtime-stride-Mul subscript. */
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

    return 42;
}
