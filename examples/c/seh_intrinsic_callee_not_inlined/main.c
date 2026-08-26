/* D-OPT7-INLINE-FRAME-SENSITIVE-INTRINSIC: the RUNTIME witness that a callee
 * containing a FRAME-SENSITIVE INTRINSIC survives the shipped `release`
 * pipeline — which runs `Inlining` — by being kept OUT OF LINE.
 *
 * WHY A CALLEE, AND WHY THIS IS NOT seh_catch_av. The existing SEH examples put
 * the `__try` in `main` itself, so the inliner never has a decision to make and
 * nothing about frame-sensitivity is under test. Here the guarded region lives
 * in `guarded_read` — `static`, tiny, and called from exactly ONE site, which is
 * the precise shape the cost model and the single-call-site heuristic most want
 * to inline. The gate must refuse it anyway.
 *
 * WHAT MAKES IT FRAME-SENSITIVE. `GetExceptionCode()` is a compiler intrinsic,
 * not a callable symbol: `c.lang.json` binds it (via `_exception_code`) to a
 * dedicated 0-operand MIR value op. ZERO operands is the whole point — it takes
 * nothing from the instruction stream, so its entire meaning comes from the
 * frame it executes in, recovered from the __C_specific_handler dispatch context
 * of the ESTABLISHING frame. Splice that into `main` and it reads main's frame,
 * which established no handler.
 *
 * WHY THE ANSWER IS UNFORGEABLE. `main` commits a PAGE_NOACCESS page and passes
 * it in; the read faults with EXCEPTION_ACCESS_VIOLATION, the filter funclet
 * compares the code, and the handler yields 42. A wrongly-inlined callee cannot
 * quietly return a slightly-wrong number: the scope table would describe a
 * region in the wrong function, so the AV escapes and the process dies. The
 * grader sees a crash, never a silent pass.
 *   10  VirtualAlloc failed — an environment failure, not an SEH result.
 *   42  the fault was caught in the CALLEE's own frame (SUCCESS).
 *
 * The `release` arm is the load-bearing half: on the baseline pipeline nothing
 * inlines, so a baseline-only example would prove nothing about the gate.
 *
 * pe64-ONLY: x64 SEH + __C_specific_handler + windows.h are Windows
 * (windows.json is availableObjectFormats:["pe"]).
 */
#include <windows.h>

/* The CALLEE under test: static, single call site, small body — everything the
 * inliner looks for — but its `__except` filter uses the frame-sensitive
 * GetExceptionCode() intrinsic, so `inlineLegalityGate` must refuse it. */
static int guarded_read(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;                              /* → access violation */
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;     /* reached iff the fault dispatched to THIS frame's handler */
    }
    return rc;
}

int main(void) {
    /* A single no-access page: reading it raises EXCEPTION_ACCESS_VIOLATION. */
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;   /* environment failure — not an SEH result */
    }

    return guarded_read(p);   /* 42 iff the callee kept its own frame */
}
