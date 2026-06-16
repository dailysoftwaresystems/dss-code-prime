/* FC7 C1a (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): struct passed BY VALUE under the
 * x86_64 System V ABI. `two()` takes an 8-byte struct → ONE SysV eightbyte
 * (INTEGER) passed in a single register (rdi); `three()` takes a 24-byte struct
 * → >16 bytes → MEMORY class → passed BY REFERENCE (the caller copies to a temp
 * and passes a pointer). Both reconstruct into the callee's frame slot. The
 * values flow through the non-inlined mk() so nothing folds; the FAR fields
 * (p.y@4, b.c@16) must survive the by-value transfer. (10+11) + (7+6+8) =
 * 21 + 21 = 42. SysV runtime closes on x86_64-ELF only (Win64/AAPCS64 fail
 * loud — C2/C3; Mach-O-x86_64 is also SysV but has no CI leg). RED-ON-DISABLE:
 * a dropped piece / missed far field drops it off 42. The 2-eightbyte
 * (9–16 byte) in-register case is fail-loud this phase
 * (D-FC7-SYSV-STRUCT-ARG-MULTIREG). */
struct Pair { int x; int y; };
struct Big  { long a; long b; long c; };
int mk(int v) { return v; }
int two(struct Pair p) { return p.x + p.y; }
int three(struct Big b) { return (int)(b.a + b.b + b.c); }
int main(void) {
    struct Pair p;
    p.x = mk(10); p.y = mk(11);
    struct Big b;
    b.a = mk(7); b.b = mk(6); b.c = mk(8);
    return two(p) + three(b);
}
