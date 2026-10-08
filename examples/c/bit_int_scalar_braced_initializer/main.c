/* P69 (lane `cs`, found closing D-C-A-INCOMPATIBLE-BRACE-ELEMENT-IS-REFUSED-BY-AN-INTERNAL-VERIFIER-FAILURE):
 * the initializer of a SCALAR is a single expression "optionally enclosed in braces" (C 6.7.9p11) or
 * the empty initializer (C23 6.7.10p11), and a `_BitInt(N)` is a scalar (an integer type). DSS refused
 * every braced `_BitInt` initializer: the closed list of scalar kinds a brace list may target had left
 * `_BitInt` out (H_UnsupportedLoweringForKind). Exit 42 when every object holds its value, otherwise a
 * bitmask of the ones that do not:
 *   1 `{ 42 }`, 2 the empty `{}`, 4 a wide (N > 64) one from `{ 42 }`, 8 a wide empty `{}`,
 *   16 a static one, 32 a `_BitInt` member's own braces. */
int main(int argc, char **argv) {
    _BitInt(8) b = { 42 };
    _BitInt(8) zb = {};
    unsigned _BitInt(100) w = { 42 };
    unsigned _BitInt(100) zw = {};
    static _BitInt(24) s = { 40 };
    struct { _BitInt(12) m; int n; } t = { { 2 }, 7 };
    int bad = 0;
    (void)argv;
    if (b != 42) bad |= 1;
    if (zb != 0) bad |= 2;
    if (w != 42) bad |= 4;
    if (zw != 0) bad |= 8;
    if (s != 40) bad |= 16;
    if (t.m != 2 || t.n != 7) bad |= 32;
    return bad == 0 ? 42 + (argc - argc) : bad;
}
