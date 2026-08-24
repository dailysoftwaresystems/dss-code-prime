/* FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): copy a UNION value. A union's variants
 * OVERLAP, so a field-wise copy of one variant would miss the others' bytes —
 * lowerAggregateCopy copies a union BYTE-WISE (the whole object representation,
 * here 8 bytes = one I64 chunk). `b = a` round-trips both halves of the struct
 * variant. The values (30, 12) arrive as runtime args across the non-inlined
 * fill() call, so no pass folds them away. 30 + 12 = 42. RED-ON-DISABLE: revert
 * the union byte-wise arm and the union copy fails loud (no field-wise copy of
 * an overlapping union is correct). */
struct P { int x; int y; };
union U { int n; struct P p; };
void fill(union U* u, int a, int b) { u->p.x = a; u->p.y = b; }
int main(void) {
    union U a;
    fill(&a, 30, 12);
    union U b = a;            /* union copy → byte-wise */
    return b.p.x + b.p.y;     /* 30 + 12 = 42 */
}
