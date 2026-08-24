/* FC7 (D-FC7-MEMBER-ACCESS): struct COPY by value (`struct Big t = s`)
 * copies FIELD-WISE — every field, not just the low register-width bytes.
 * A 20-byte struct copy must preserve the FAR field (e at byte offset 16),
 * which an aggregate-width 8-byte Load+Store would TRUNCATE to 0 (the
 * silent miscompile this closes). `s` is filled through a pointer (runtime,
 * non-foldable); `t = s` copies; the exit is the far field.
 * RED-ON-DISABLE: revert struct copy to the aggregate Load+Store and t.e
 * reads 0 → exit 0 instead of 99. */
struct Big { int a; int b; int c; int d; int e; };
void fill(struct Big* p, int base) { p->a = base; p->e = base + 77; }
int main(void) {
    struct Big s;
    fill(&s, 22);
    struct Big t = s;
    return t.e;
}
