/* FC7 C2 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): struct passed AND returned BY VALUE
 * under the x86_64 Microsoft x64 (Win64) ABI, across its by-SIZE classes:
 *   - P     — 8-byte struct (power of 2 ≤ 8) → passed/returned in ONE GPR
 *             (Win64 treats a small aggregate as an integer of its size).
 *   - T     — 12-byte struct (> 8) → passed BY REFERENCE (caller-allocated copy)
 *             and returned via SRET (a hidden result pointer in RCX, the pointer
 *             back in RAX); the far field t.c@8 must survive.
 *   - Three — 3-byte struct (NOT a power of 2) → also BY REFERENCE / SRET, even
 *             though it is < 8 bytes (the Win64 size rule, distinct from SysV
 *             which would pass a 3-byte struct in a register).
 * Values flow through the non-inlined mk() so nothing folds.
 *   add2(P{20,1}) + sum3(T{6,7,0}) + sumc(Three{2,3,3}) = 21 + 13 + 8 = 42.
 * Win64 runtime closes on x86_64-PE NATIVELY on Windows (the loop's host). The
 * aggregate types use `typedef` because a top-level `struct Tag` return specifier
 * is the pre-FC4 grammar residue (D-CSUBSET-STRUCT-BODY-VARDECL-POSITION); the ABI
 * codegen is identical. RED-ON-DISABLE: a mis-classified size or a dropped far
 * field knocks the exit off 42. */
typedef struct { int x; int y; }            P;      /* 8B  → 1 GPR */
typedef struct { int a; int b; int c; }     T;      /* 12B → by-ref / sret */
typedef struct { char a; char b; char c; }  Three;  /* 3B  → by-ref / sret (non-pow2) */

int mk(int v) { return v; }

P mkP(int x, int y) { P p; p.x = x; p.y = y; return p; }
int add2(P p) { return p.x + p.y; }

T mkT(int a, int b, int c) { T t; t.a = a; t.b = b; t.c = c; return t; }
int sum3(T t) { return t.a + t.b + t.c; }

Three mkThree(int a, int b, int c) { Three t; t.a = a; t.b = b; t.c = c; return t; }
int sumc(Three t) { return t.a + t.b + t.c; }

int main(void) {
    P p      = mkP(mk(20), mk(1));            /* 8B in-register return */
    T t      = mkT(mk(6), mk(7), mk(0));      /* 12B sret return */
    Three th = mkThree(mk(2), mk(3), mk(3));  /* 3B non-pow2 sret return */
    return add2(p)         /* 8B in-register arg          → 21 */
         + sum3(t)         /* 12B by-reference arg        → 13 */
         + sumc(th);       /* 3B non-pow2 by-reference arg →  8 */
}
