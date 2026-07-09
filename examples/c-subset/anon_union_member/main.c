/* FC16 D-CSUBSET-ANON-MEMBER-PROMOTION (C11/C23 §6.7.2.1 ¶13): an ANONYMOUS
 * UNION member's fields are promoted into the enclosing struct's member
 * namespace AND share storage (offset 0). `s.a` and `s.b` name the SAME bytes.
 * Write s.a = 41, then s.b = s.b + 1: because a and b alias, s.b reads 41 and
 * writes 42 back into the same slot, so s.a is now 42. The named direct member
 * `tag` sits at offset 0 too? No — tag @ 0, the anon union @ 4 (int-aligned),
 * so a/b @ 4 and tag stays 1. Returns s.a = 42.
 * RED-ON-DISABLE: without promotion s.a/s.b are S0017/undeclared and this does
 * not compile; if a and b did NOT alias, s.a would stay 41 (off 42). */
struct S { int tag; union { int a; int b; }; };
int main(void) {
    struct S s;
    s.tag = 1;
    s.a = 41;
    s.b = s.b + 1;   /* aliases s.a: reads 41, writes 42 */
    return s.a;      /* 42 */
}
