/* `long double` across a DYNAMIC-LIBRARY boundary: every call below crosses into
   `dsslib`, built as the target's shared library (a `.dll` here).
   Exit 42 = both round trips hold; 11 = the first call or its conversion, 12 =
   the call after the library's global was rewritten through its own address. */
extern long double ld_scale_add(long double, long double);
extern int ld_to_int(long double);
extern long double *ld_scale_addr(void);

int main(void) {
    long double const r = ld_scale_add(20.0L, 2.0L); /* 20 * 2 + 2 */
    if (ld_to_int(r) != 42) return 11;
    *ld_scale_addr() = 3.0L;
    if (ld_to_int(ld_scale_add(10.0L, 12.0L)) != 42) return 12; /* 10 * 3 + 12 */
    return 42;
}
