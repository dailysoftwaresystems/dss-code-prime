/* c23 (D-CSUBSET-STRUCT-MULTI-DECLARATOR): comma-separated declarators sharing
 * one head base type in a struct body. `struct P { int *a, b; }` -- the `*`
 * binds to a ONLY (a is int*, b is int); `struct Q { int x, y[2], *z; }` -- x
 * is int, y is int[2], z is int*. Each field is written then read back through
 * a non-inlined call so it genuinely round-trips through its own struct slot at
 * its own byte offset.
 *
 * Layout (LP64, natural alignment):
 *   P: a @0 (ptr,8) ; b @8 (int,4)              -- b is int, NOT a 2nd pointer
 *   Q: x @0 (int,4) ; y @4 (int[2],8) ; z @16 (ptr,8)
 *
 * Contribution map: P.b(=20) read directly + Q.x(=2) + Q.y[0](=3)
 *                 + Q.y[1](=4) + (*Q.z)(=13)  = 20 + 2 + 3 + 4 + 13 = 42.
 *
 * RED-ON-DISABLE: if the head `*` LEAKED onto b (b typed int*), b would occupy
 * an 8-byte slot at offset 8 with pointer semantics -- writing the int 20 into
 * it and reading it back through the int-typed field accessor would drift, and
 * the struct size/offsets shift. A y `[2]` or z `*` suffix leak shifts z's
 * offset so *q.z reads the wrong cell. Either flips the exit off 42. The writes
 * take RUNTIME arguments across non-inlined calls so no pass folds them. */

struct P { int *a, b; };
struct Q { int x, y[2], *z; };

/* Non-inlined writers: arguments arrive at runtime so no pass folds the
 * fields away. set_p writes BOTH P fields -- *p->a (the pointer field) and
 * p->b (the plain-int field, the slot a leak would corrupt). */
void set_p(struct P *p, int via_ptr, int bv) {
    *(p->a) = via_ptr;
    p->b = bv;
}
void set_q(struct Q *q, int x, int y0, int y1, int zv) {
    q->x = x;
    q->y[0] = y0;
    q->y[1] = y1;
    *(q->z) = zv;
}
int read_p_b(struct P *p) { return p->b; }
int read_q(struct Q *q) { return q->x + q->y[0] + q->y[1] + *(q->z); }

int main(void) {
    int pcell;            /* P.a points here */
    int qcell;            /* Q.z points here */
    struct P p;
    struct Q q;
    p.a = &pcell;
    q.z = &qcell;
    set_p(&p, 99, 20);             /* *p.a = 99 (unused in sum) ; p.b = 20 */
    set_q(&q, 2, 3, 4, 13);        /* q.x=2 q.y={3,4} *q.z=13 */
    return read_p_b(&p) + read_q(&q);   /* 20 + (2+3+4+13) = 42 */
}
