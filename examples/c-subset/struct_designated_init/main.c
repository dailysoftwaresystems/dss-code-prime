/* FC7 (D-FC7-MEMBER-ACCESS): a DESIGNATED initializer with zero-fill.
 * `T t = {.b = 42}` lowers ELEMENT-WISE — Store 0 → a, Store 42 → b,
 * Store 0 → c (one Gep+Store per field, at the FC6 byte offsets). Read
 * back through a pointer so nothing folds it away. a(0)+b(42)+c(0)=42.
 * RED-ON-DISABLE: a wrong field offset for `.b`, or a missed zero-fill of
 * a/c, shifts the sum off 42. */
typedef struct { int a; int b; int c; } T;
int read_all(T* t) { return t->a + t->b + t->c; }
int main(void) {
    T t = {.b = 42};
    return read_all(&t);
}
