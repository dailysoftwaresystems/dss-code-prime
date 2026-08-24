// D-CSUBSET-PACKED: a struct MEMBER `__attribute__((packed))` removes ALL inter-field
// padding END-TO-END (the frontend feeds the interner's packed flag → computeLayout
// lays every field out at its unpadded offset with the aggregate aligned to 1). This
// RUNS on every target, and — critically — READS+WRITES a deliberately MISALIGNED
// multi-byte field at runtime (v @ offset 1, w @ offset 5), the empirical proof that
// codegen needs NO packed fork: arm64 uses unaligned-tolerant LDUR/STUR, x86 plain mov.
//
// struct P { char c; unsigned v; int w; } packed;  // c@0, v@1, w@5 → sizeof 9
//   (unpacked would be c@0, pad, v@4, w@8 → sizeof 12 — a different exit code)
//
// Offsets are read via the manual `&((struct P*)0)->m` idiom (== offsetof(P, m)) —
// the same idiom the alignas_member example uses — so no <stddef.h> preprocessing is
// needed and the offset computation itself exercises the packed member layout.
//
// All values are data-model-INDEPENDENT (char/unsigned/int widths + packed offsets
// are identical under LP64 and LLP64), so the one exit code holds across all four
// targets AND the shipped release pipeline (which optimizes over packed's MIR shapes).
//
// exit = sizeof(P)(9) + offsetof(P,v)(1) + offsetof(P,w)(5) + 27 = 42.

struct P { char c; unsigned v; int w; } __attribute__((packed));

// `seed` is a runtime function argument, so the unaligned store+load cannot be
// constant-folded away in the baseline pipeline — the misaligned 4-byte accesses at
// offsets 1 and 5 really execute.
int run(int seed) {
    struct P s;
    s.c = 7;
    s.v = 0xDEADBEEFu ^ (unsigned)seed;                  // unaligned 4-byte store @1
    s.w = 0x01020304 + seed;                             // unaligned 4-byte store @5
    if (s.v != (0xDEADBEEFu ^ (unsigned)seed)) return 100;  // unaligned 4-byte load @1
    if (s.w != 0x01020304 + seed)              return 101;  // unaligned 4-byte load @5
    if (s.c != 7)                              return 102;
    return 0;
}

int main(void) {
    if (run(1) != 0) return 1;   // the misaligned read+write must round-trip
    int sz = (int)sizeof(struct P);                  // 9  (RED if padding kept: 12)
    int ov = (int)(long long)&((struct P *)0)->v;    // offsetof(P,v) == 1 (RED: 4)
    int ow = (int)(long long)&((struct P *)0)->w;    // offsetof(P,w) == 5 (RED: 8)
    return sz + ov + ow + 27;                        // 9 + 1 + 5 + 27 = 42
}
