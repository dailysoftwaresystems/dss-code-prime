/* FC7 (D-FC7-MEMBER-ACCESS): struct member read+write through a pointer
 * parameter — `p->x = a` is Gep(p,offset)+Store; `p->x + p->y` is two
 * Gep+Load. The operands (10, 32) arrive as runtime function arguments
 * across the non-inlined set_fields/sum_fields calls, so the fields
 * genuinely round-trip through the struct's frame slot — no pass folds
 * them away. 10 + 32 = 42. RED-ON-DISABLE: a wrong field byte-offset
 * (e.g. y resolved to offset 0) yields 10+10=20, not 42. */
typedef struct { int x; int y; } Point;
void set_fields(Point* p, int a, int b) { p->x = a; p->y = b; }
int sum_fields(Point* p) { return p->x + p->y; }
int main(void) {
    Point p;
    set_fields(&p, 10, 32);
    return sum_fields(&p);
}
