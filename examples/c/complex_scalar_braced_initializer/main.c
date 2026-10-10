/* P69 (lane `cs`, found closing D-C-A-INCOMPATIBLE-BRACE-ELEMENT-IS-REFUSED-BY-AN-INTERNAL-VERIFIER-FAILURE):
 * the initializer of a SCALAR is a single expression "optionally enclosed in braces" (C 6.7.9p11) or
 * the empty initializer (C23 6.7.10p11), and a `_Complex` is a scalar (an arithmetic type). DSS refused
 * every braced `_Complex` initializer: the closed list of scalar kinds a brace list may target had left
 * `_Complex` out (H_UnsupportedLoweringForKind). Exit 42 when every object holds its value, otherwise a
 * bitmask of the ones that do not:
 *   1 `{ 42.0 }`, 2 the empty `{}`, 4 a `float _Complex` from `{ 2.0f }`, 8 a static one,
 *   16 a `_Complex` member's own braces, 32 a compound literal. */
int main(int argc, char **argv) {
    double _Complex z = { 42.0 };
    double _Complex e = {};
    float _Complex f = { 2.0f };
    static double _Complex s = { 1.5 };
    struct { double _Complex m; int n; } t = { { 3.0 }, 7 };
    double _Complex c = (double _Complex){ 5.0 };
    double const *pz = (double const *)&z;
    double const *pe = (double const *)&e;
    float const *pf = (float const *)&f;
    double const *ps = (double const *)&s;
    double const *pt = (double const *)&t.m;
    double const *pc = (double const *)&c;
    int bad = 0;
    (void)argv;
    if (pz[0] != 42.0 || pz[1] != 0.0) bad |= 1;
    if (pe[0] != 0.0 || pe[1] != 0.0) bad |= 2;
    if (pf[0] != 2.0f || pf[1] != 0.0f) bad |= 4;
    if (ps[0] != 1.5 || ps[1] != 0.0) bad |= 8;
    if (pt[0] != 3.0 || pt[1] != 0.0 || t.n != 7) bad |= 16;
    if (pc[0] != 5.0 || pc[1] != 0.0) bad |= 32;
    return bad == 0 ? 42 + (argc - argc) : bad;
}
