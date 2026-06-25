/* Cluster-F F5 (agg_string_index): string-literal indexing `"abc"[i]`.
 *
 * A string literal in INDEX-base position decays to its rodata address (C
 * 6.3.2.1) and is indexed. `idx()` uses a RUNTIME index (a function arg the
 * optimizer cannot fold to a constant), so the actual lowering executes:
 * materialize the rodata global → GEP base+byteIdx → width-8 char load. The
 * `release` arm runs the shipped pipeline over the same source.
 *
 * Red-on-disable: remove the `HirKind::Literal` arm in lowerLvalueAddressNode
 * and `"hello"[i]` fails to compile (H0009 "lvalue kind ... not supported").
 * exit 215 == "hello"[1] ('e'=101) + "world"[2] ('r'=114).
 */
int idx(int i) { return "hello"[i]; }   /* runtime index into a string literal */

int main(void) {
    int x = idx(1);       /* "hello"[1] = 'e' = 101 */
    int y = "world"[2];   /* "world"[2] = 'r' = 114 */
    return x + y;         /* 101 + 114 = 215 */
}
