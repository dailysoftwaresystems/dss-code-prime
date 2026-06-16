/* FC7 (D-FC7-NESTED-STRUCT-FIELD): a NESTED brace initializer
 * `Outer o = { { pick(10), pick(20) }, pick(12) };` — the inner `{...}`
 * initializes the struct-typed field `in` (an Inner), which lowers
 * RECURSIVELY (lowerAggregateInitIntoSlot recurses element-wise into the
 * field's sub-slot; there is no aggregate-width store). The pick() calls are
 * non-inlined identity functions, so the init values are runtime — no pass
 * folds the initializer to a constant 42. 10 + 20 + 12 = 42. RED-ON-DISABLE:
 * revert the nested-init recursion and the Inner field's stores never emit
 * (the field reads back zero/garbage), dropping it off 42. */
typedef struct { int x; int y; } Inner;
typedef struct { Inner in; int z; } Outer;
int pick(int v) { return v; }
int main(void) {
    Outer o = { { pick(10), pick(20) }, pick(12) };
    return o.in.x + o.in.y + o.z;
}
