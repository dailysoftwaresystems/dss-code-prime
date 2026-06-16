/* FC7 C1a+C1b (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): struct passed BY VALUE under
 * the x86_64 System V ABI, across all three register/memory classes:
 *   - two()   — 8-byte struct  → ONE eightbyte, passed in a single register (rdi).
 *   - three() — 12-byte struct → TWO eightbytes (C1b multi-register), passed in
 *               rdi:rsi; the far field t.z@8 lives in the SECOND register.
 *   - big()   — 24-byte struct → >16B MEMORY class, passed BY REFERENCE (the
 *               caller copies to a temp and passes a pointer); far field b.c@16.
 * Each callee reconstructs into its frame slot. Values flow through the
 * non-inlined mk() so nothing folds; every far field must survive the transfer.
 * (3+4) + (5+6+7) + (8+9+0) = 7 + 18 + 17 = 42. SysV runtime closes on
 * x86_64-ELF only (Win64/AAPCS64 fail loud — C2/C3; Mach-O-x86_64 is also SysV
 * but has no CI leg). RED-ON-DISABLE: a dropped eightbyte / missed far field
 * drops it off 42. */
struct Pair { int x; int y; };
struct Tri  { int x; int y; int z; };
struct Big  { long a; long b; long c; };
int mk(int v) { return v; }
int two(struct Pair p)  { return p.x + p.y; }
int three(struct Tri t) { return t.x + t.y + t.z; }
int big(struct Big b)   { return (int)(b.a + b.b + b.c); }
int main(void) {
    struct Pair p;
    p.x = mk(3); p.y = mk(4);
    struct Tri t;
    t.x = mk(5); t.y = mk(6); t.z = mk(7);
    struct Big b;
    b.a = mk(8); b.b = mk(9); b.c = mk(0);
    return two(p) + three(t) + big(b);
}
