/* D-MIR-VERIFIER-CALLSITE-QUALIFIED-POINTEE (TF-C112).
**
** `T*` -> `volatile T*` at a call argument is an IMPLICIT QUALIFICATION
** CONVERSION (C17 6.5.16.1p1): legal, cast-free, and bit-identical, so the
** tree emits no Cast for it and the MIR call operand legitimately arrives
** with the UNQUALIFIED pointee. Before the fix, `MirVerifier::
** checkCallSignatures` compared the two pointees by raw interned TypeId and
** rejected the shape with `I_CallSignatureMismatch`.
**
** REDUCED FROM THE REAL FAILURE: sqlite `src/func.c` declares
** kahanBabuskaNeumaierInit/Step/StepInt64 as `(volatile SumCtx *, ...)` and
** calls all three from sumStep/sumInverse holding a plain `SumCtx *p`. That
** was 11 errors across 3 callees in one TU and cost the whole
** testfixture.exe. gcc 13.2.0 accepts the shape at
** `-std=c17 -Wall -Wextra -pedantic` with zero diagnostics.
**
** Every check below is a DISTINCT non-42 exit so a regression names itself.
** The struct field layout mirrors sqlite's SumCtx exactly. */

typedef struct SumCtx SumCtx;
struct SumCtx {
    double        rSum;
    double        rErr;
    long long     iSum;
    long long     cnt;
    unsigned char approx;
    unsigned char ovrfl;
};

/* ── the three callees, all taking a VOLATILE-qualified pointer ───────────── */

static void kbnInit(volatile SumCtx *p, long long v) {
    p->rSum   = (double)v;
    p->rErr   = 0.0;
    p->iSum   = v;
    p->cnt    = 1;
    p->approx = 0;
    p->ovrfl  = 0;
}

static void kbnStepInt64(volatile SumCtx *p, long long v) {
    p->iSum += v;
    p->rSum += (double)v;
    p->cnt  += 1;
}

static void kbnStep(volatile SumCtx *p, double r) {
    double const s = p->rSum + r;
    /* Kahan-Babuska-Neumaier error term, as in the original. */
    if (p->rSum > r) {
        p->rErr += (p->rSum - s) + r;
    } else {
        p->rErr += (r - s) + p->rSum;
    }
    p->rSum   = s;
    p->cnt   += 1;
    p->approx = 1;
}

/* A plain `SumCtx *` parameter, reached from a `volatile SumCtx *` through an
** EXPLICIT cast. The cast is deliberate: the IMPLICIT discard is a C constraint
** violation that gcc warns on and DSS does not yet diagnose
** (D-CSUBSET-QUALIFIER-DISCARD-AT-CALL-ARG-UNDIAGNOSED), so writing it bare here
** would enshrine a divergence this corpus should not depend on — the day that
** anchor closes, this example must NOT be what breaks. */
static long long readCount(SumCtx *p) { return p->cnt; }

/* ── callers holding an UNQUALIFIED SumCtx*, exactly as sqlite's sumStep does ─ */

static SumCtx gCtx;

static long long sumStepAll(void) {
    SumCtx *p = &gCtx;          /* plain SumCtx*, fed to volatile-qualified params */
    kbnInit(p, 40);
    kbnStepInt64(p, 2);
    return p->iSum;             /* 42 */
}

static double sumStepFloat(void) {
    SumCtx *p = &gCtx;
    kbnInit(p, 0);
    kbnStep(p, 1.5);
    kbnStep(p, 0.5);
    return p->rSum;             /* 2.0 */
}

/* A LOCAL (Mem2Reg-promotable) receiver rather than a global, so the optimized
** arms exercise the same conversion after promotion/copy-propagation. */
static long long sumStepLocal(void) {
    SumCtx  local;
    SumCtx *p = &local;
    kbnInit(p, 7);
    kbnStepInt64(p, 3);
    kbnStepInt64(p, 5);
    return p->iSum;             /* 15 */
}

int main(void) {
    /* 1 — the integer path through two volatile-qualified callees. */
    if (sumStepAll() != 42) return 11;
    /* 2 — cnt accumulated through the same calls. */
    if (gCtx.cnt != 2) return 12;
    /* 3 — the floating path through kbnStep twice. */
    if (sumStepFloat() != 2.0) return 13;
    /* 4 — the volatile-qualified callee wrote its flag through the qualified
    **     pointer; the caller reads it through the unqualified one. */
    if (gCtx.approx != 1) return 14;
    /* 5 — a promotable LOCAL receiver, same conversion. */
    if (sumStepLocal() != 15) return 15;
    /* 6 — a plain-pointer callee reached from a volatile handle via an explicit
    **     cast: the object written through the qualified spelling reads back
    **     identically through the unqualified one. */
    {
        volatile SumCtx *vp = &gCtx;
        if (readCount((SumCtx *)vp) != 3) return 16;
    }
    /* 7 — the receiver address is unchanged by the qualification conversion:
    **     a qualifier changes no bits, so both spellings name one object. */
    {
        volatile SumCtx *vp = &gCtx;
        SumCtx          *up = &gCtx;
        if ((void *)vp != (void *)up) return 17;
    }
    return 42;
}
