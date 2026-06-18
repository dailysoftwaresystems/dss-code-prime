/* FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): struct RETURNED BY VALUE under the
 * x86_64 System V ABI, across all return classes:
 *   - mkTri() — 12-byte struct → TWO eightbytes, returned IN REGISTERS (rax:rdx);
 *               the far field t.z@8 comes back in the SECOND register (rdx).
 *   - mkBig() — 24-byte struct → >16B MEMORY class, returned via SRET (a hidden
 *               result pointer the caller allocates; the far field b.c@16 must
 *               survive the copy-through).
 *   - mkMix() — {double; long} 16-byte struct → MIXED eightbytes: d@0 returns in
 *               xmm0 (SSE), n@8 in rax (INTEGER) — exercises the per-class
 *               return-register split AND the cross-class move.
 * Values flow through the non-inlined mk()/mkl()/mkd() so nothing folds; every
 * field — especially the far one in the 2nd register / at offset 16 — must
 * survive the return transfer.
 *   (20+1+1) + (6+7+0) + ((int)5.0 + 2) = 22 + 13 + 7 = 42.
 * The aggregate types are declared via `typedef` because a top-level `struct Tag`
 * specifier as a function RETURN type is a pre-FC4 grammar residue
 * (D-CSUBSET-STRUCT-BODY-VARDECL-POSITION — `topLevelHead` admits keyword seqs +
 * typedef-names, not bare struct/union heads); the typedef-name return type is
 * the idiomatic reachable form and exercises the identical ABI codegen.
 * SysV runtime closes on x86_64-ELF only (Win64/AAPCS64 fail loud — C2/C3;
 * Mach-O-x86_64 is also SysV but has no CI leg). RED-ON-DISABLE: a dropped piece,
 * a missed sret copy, or the mixed class landing the wrong register knocks the
 * exit off 42. */
typedef struct { int x; int y; int z; }    Tri;
typedef struct { long a; long b; long c; }  Big;
typedef struct { double d; long n; }        Mix;

int    mk(int v)     { return v; }
long   mkl(long v)   { return v; }
double mkd(double v) { return v; }

Tri mkTri(int a, int b, int c) {
    Tri t;
    t.x = a; t.y = b; t.z = c;
    return t;
}
Big mkBig(long a, long b, long c) {
    Big r;
    r.a = a; r.b = b; r.c = c;
    return r;
}
Mix mkMix(double d, long n) {
    Mix m;
    m.d = d; m.n = n;
    return m;
}

int main(void) {
    Tri t = mkTri(mk(20), mk(1), mk(1));    /* 12B → rax:rdx; z@8 in rdx */
    Big b = mkBig(mkl(6), mkl(7), mkl(0));  /* 24B → sret; c@16 far field */
    Mix m = mkMix(mkd(5.0), mkl(2));        /* 16B → xmm0:rax mixed class */
    return (t.x + t.y + t.z)                /* 22 */
         + (int)(b.a + b.b + b.c)           /* 13 */
         + ((int)m.d + (int)m.n);           /*  7 */
}
