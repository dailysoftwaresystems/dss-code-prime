/* D-MIR-STORAGE-ARRAY-INDEX-GEP + the latent pointer-index scale-1 miscompile
 * fix (Option A — agnostic MIR element scaling; user §B 2026-06-17).
 *
 * Three things this exercises that did NOT work before this cycle:
 *   (1) ARRAY-LOCAL brace-init      `int a[4] = {10,20,30,40}`  (element-wise
 *                                    Stores at byte offsets 0,4,8,12).
 *   (2) STORAGE-array index         `a[i]` with a RUNTIME index i  — fail-loud
 *                                    before (3-op GEP unsupported at LIR); now
 *                                    a 2-op byte-offset GEP `[&a, i*4]`.
 *   (3) POINTER index of a non-char  `p[2]` over `int*`  — the LATENT SILENT
 *                                    MISCOMPILE: it lowered to `lea [p+idx*1]`
 *                                    (scale 1), reading byte `idx` not `idx*4`.
 *
 *   a   = {10, 20, 30, 40}
 *   i   = pick(3)      a RUNTIME index (opaque across the un-inlined call)
 *   s   = a[i]         storage-array index, runtime i      -> 40
 *   p   = &a[0]        an int* over the array (no array-decay reliance)
 *   t   = p[2]         POINTER index — the miscompile witness -> 30
 *   u   = a[1]         storage-array, constant index        -> 20
 *   return s + t - u - 8                                    -> 40+30-20-8 == 42
 *
 * RED-ON-DISABLE: drop the element scaling (scaleIndexToBytes) and the int*
 * `p[2]` reads at byte offset 2 instead of 8 — a[2] (==30) becomes the int
 * straddling bytes [2..5] (==0x00140000), so the exit collapses to 12, not 42.
 * The storage-array `a[i]` cases simply would not have compiled at all. */
int pick(int x) { return x; }

int main(void) {
    int a[4] = {10, 20, 30, 40};
    int i = pick(3);
    int s = a[i];
    int* p = &a[0];
    int t = p[2];
    int u = a[1];
    return s + t - u - 8;
}
