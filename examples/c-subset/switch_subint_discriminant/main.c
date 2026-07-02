/* c78 (D-CSUBSET-NARROW-SWITCH-DISCRIMINANT-CMP): a `switch` on a sub-int
 * (char/unsigned char/short) discriminant must integer-PROMOTE the controlling
 * expression to int (C 6.8.4.2) before the dispatch compare. Pre-fix the
 * discriminant reached MIR→LIR at its narrow width and the sparse dispatch
 * emitted `cmp` at width 8/16 → error[A_NoMatchingEncodingVariant] (there is NO
 * sub-32-bit integer compare on x86 OR arm64 — hence a LOWERING promotion, not a
 * per-target encoding). The 313× dominant of the post-c77 sqlite encoder frontier.
 * `io()` defeats const-fold so the discriminant is a real runtime sub-int value.
 * RED-ON-DISABLE: without the promotion → A_NoMatchingEncodingVariant cmp width 8.
 * => 154. */
int io(int x){ return x; }
int du(unsigned char op){ switch(op){ case 1: return 11; case 200: return 22; default: return 99; } }
int ds(signed char op){ switch(op){ case -5: return 33; case 7: return 44; default: return 88; } }
int main(void){
    int r = 0;
    r += du((unsigned char)io(200));   /* 22 */
    r += ds((signed char)io(-5));      /* 33 */
    r += du((unsigned char)io(3));     /* 99 (default) */
    return r;                          /* 22+33+99 = 154 */
}
