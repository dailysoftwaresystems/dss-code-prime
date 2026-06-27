/* D-OPT-VARIADIC-RELEASE-MISCOMPILE (p18 SQLite-readiness Cluster G c2) — the
 * RELEASE-pipeline witness that a VARIADIC function is not inlined.
 *
 * `vsum` is static + called exactly once → a prime inlining candidate. Its
 * `va_start` lowers to the frame-relative leaves VaRegSaveAreaAddr /
 * VaOverflowArgAreaAddr (SysV/AAPCS64) or VaHomeArgAreaAddr (Win64), which
 * materialize to `lea reg, [sp + offset]` against VSUM's OWN variadic-prologue
 * register-save / overflow area — set up only by vsum's prologue. The release
 * pipeline (`release.pipeline.json`) runs Inlining; without the
 * inlineLegalityGate va_start-leaf refusal the optimizer splices vsum's body
 * into main, where the va_start leaves bind to MAIN's frame (which never spilled
 * any varargs) → `ap` reads garbage → wrong exit (0/127, not 42).
 *
 * The baseline arm runs the no-op debug pipeline (no Inlining) — it was always
 * correct, which is exactly why FC12 shipping the variadic corpus BASELINE-ONLY
 * masked this. The `{"shippedPipeline":"release"}` arm is the witness.
 *
 * RED-on-disable: remove the `VaRegSaveAreaAddr || VaOverflowArgAreaAddr ||
 * VaHomeArgAreaAddr` refusal in src/opt/passes/inlining.cpp and the release arm
 * exits garbage instead of 42. exit 42 == 10 + 13 + 19. */
#include <stdarg.h>

static int vsum(int n, ...) {
    va_list ap;
    int acc = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i = i + 1) {
        acc = acc + va_arg(ap, int);
    }
    va_end(ap);
    return acc;
}

int main(void) {
    return vsum(3, 10, 13, 19);   /* 10 + 13 + 19 = 42 */
}
