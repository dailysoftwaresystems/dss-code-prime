/* FC17.9(c) (D-CSUBSET-SETJMP + D-OPT-SETJMP-RETURNS-TWICE-INLINE) - the C11
 * <setjmp.h> non-local-jump witness across 4 legs / 3 realizations, RUN on BOTH the
 * baseline (debug) AND the release pipeline (Mem2Reg + Inlining active).
 *
 * On elf (x86_64 + arm64) setjmp/longjmp are DIRECT libc FFI (glibc libc.so.6
 * exports them); on pe64 the source `setjmp(env)` expands via a pe-only descriptor
 * MACRO to MSVC's 2-arg `_setjmp(env, 0)` (msvcrt export, frame=0 = the plain non-SEH
 * restore); on macho (arm64) they are DIRECT libSystem FFI. The `jmp_buf` typedef is a
 * per-(arch,format) opaque buffer sized from each platform's setjmp.h (elf-x86_64 200B,
 * elf-arm64 312B, pe-x86_64 256B/16-aligned so `_setjmp`'s `movaps` xmm saves don't
 * #GP, macho-arm64 over-sized to 224B).
 *
 * ── The release-pipeline hardening this example proves at RUNTIME ──
 * setjmp/longjmp are FRAME-SENSITIVE: the non-local jump re-enters `run`'s setjmp over
 * an edge no CFG models. Two optimizer passes had to be taught this, and BOTH are
 * exercised here on the `release` arm:
 *   (1) mem2reg must NOT promote a local live across the setjmp Call. `modified` is
 *       stored (= g_bump) on the first pass, BEFORE the longjmp; the resume returns to
 *       setjmp with r != 0 and the post-resume read must observe that store. The static
 *       CFG's only edge into the post-resume block comes from BEFORE the store, so a
 *       PROMOTED `modified` would read its stale entry value (0) -> exit 40. Kept in
 *       memory (mem2reg's returns-twice whole-function bail), longjmp's SP + callee-
 *       saved restore makes the last store (2) observable -> exit 42.
 *   (2) the inliner must NOT inline `run` (a returns-twice callee) into `main`; the
 *       spliced setjmp would capture main's frame, not run's. The gate refuses it.
 *   (M1) `determinate` is computed BEFORE setjmp and never modified -> the Call-barrier
 *       + longjmp-restore keep it correct (40) after the resume: the determinate half.
 * exit 40 (determinate) + 2 (modified) = 42.
 *
 * ANTI-FOLD: g_base / g_bump are mutable globals (runtime-opaque) so no pass can const-
 * fold the setjmp/longjmp round-trip away to `return 42` (the cond_init_release idiom).
 *
 * RED-on-disable (mem2reg): revert the returns-twice whole-function bail in
 * src/opt/passes/mem2reg.cpp -> the release arm promotes `modified` -> exits 40 not 42.
 * RED-on-disable (header): delete shippedLibs/setjmp.json -> `#include <setjmp.h>`
 * fires F_ShippedHeaderNotFound and the compile fails. The macho arm RUNS natively on a
 * macOS-arm64 CI host; a wrong pe64 jmp_buf alignment crash-catches on the local pe64
 * run. */
#include <setjmp.h>

int g_base = 40;   /* mutable global: runtime-opaque anti-fold */
int g_bump = 2;    /* mutable global: runtime-opaque anti-fold */

/* setjmp lives in a CALLEE of main so the release pipeline's inliner sees a returns-
 * twice callee and must REFUSE to inline it (frame-sensitive: the spliced setjmp would
 * capture main's frame, not run's). */
static int run(void) {
    jmp_buf env;                /* local buffer (setjmp + longjmp both live in run)   */
    int determinate = g_base;   /* computed BEFORE setjmp; never modified after (M1)  */
    int modified    = 0;        /* modified BETWEEN setjmp and longjmp                 */

    int r = setjmp(env);        /* returns 0 first, then 1 on the longjmp resume      */
    if (r == 0) {
        modified = g_bump;      /* store before the longjmp (first pass only)         */
        longjmp(env, 1);        /* noreturn: jumps back into setjmp with value 1      */
    }

    /* post-resume (r == 1): determinate is still 40 (unmodified across the jump);
     * modified is 2 ONLY IF mem2reg left it in memory (else the stale entry value 0). */
    return determinate + modified;   /* 40 + 2 = 42 */
}

int main(void) {
    return run();
}
