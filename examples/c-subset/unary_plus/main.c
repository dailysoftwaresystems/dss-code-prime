/* c12 D-CSUBSET-UNARY-PLUS runtime witness.
 *
 * The unary `+` operator (C 6.5.3.3p2): applies the integer promotions and
 * yields the value — an IDENTITY (`+x` == `x`). There is no `Plus`/`Identity`
 * HIR op, so CST→HIR lowers `+x` directly to its OPERAND (the promotion is
 * already realized by the lazy-consumer model; sub-int values live promoted in
 * 32-bit registers). Unary `-` already parsed; only `+` was missing from the
 * prefix-operator list. Exercises `+literal`, `+(parenthesized arith expr)`,
 * and a redundant double `++` (i.e. `+ +x`, NOT pre-increment) → exit 42.
 *
 * RED-ON-DISABLE: drop `"+"` from the prefix-operator list in
 * c-subset.lang.json (or the `PlusOp → Pos` unaryOps row) and `int x = +1;`
 * fails to PARSE (P0009 at the `+`). A pointer/struct operand of `+` fails LOUD
 * (H0009 "operand of unary '+' must have arithmetic type") — covered by the
 * strict semantic tests, not runnable here. */
int main(void) {
    int x = +1;                 /* +literal */
    int y = + +x;               /* two unary pluses (identity), NOT ++ */
    return +(x + 40) + y;       /* +(parenthesized) + identity  = 41 + 1 = 42 */
}
