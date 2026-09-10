// D-CSUBSET-PER-MEMBER-PACKED: GNU `packed` written on ONE struct MEMBER-DECLARATOR
// packs THAT member and nothing else — a DIFFERENT layout from the whole-composite
// `__attribute__((packed))` that `examples/c/packed_struct` covers.
//
//   struct P { char c; unsigned v __attribute__((packed)); double d; };
//     packed member: c@0, v@1, d@8 — sizeof 16, _Alignof 8
//     undecorated:   c@0, v@4, d@8 — sizeof 16, _Alignof 8
//
// ★★ THE SIZE AND THE ALIGNMENT ARE IDENTICAL EITHER WAY. Only `v`'s offset moves,
// 4 → 1. That is what makes this defect a SILENT miscompile rather than a visible
// one, and it is why this example asserts OFFSETS and not merely `sizeof`.
//
// ✔MEASURED, references probed SEPARATELY, each compiling AND RUNNING this file:
//   gcc 13.3.0 x86_64-linux · gcc 13.3.0 aarch64 (qemu) · clang 18.1.3 x86_64-linux
//   · clang 18.1.3 aarch64 (qemu) · mingw-w64 gcc 13.2.0 (PE) — all exit 42.
//   MSVC 19.51 ABSTAINS (it implements no `__attribute__` in any position).
// The reference is the oracle; the exit code alone is not.
//
// It also READS AND WRITES the deliberately misaligned 4-byte member at offset 1 at
// RUNTIME — the empirical proof that codegen needs no per-member-packed fork (arm64
// unaligned-tolerant LDUR/STUR, x86 plain mov), exactly as the whole-composite
// witness does.
//
// Offsets are read via the manual `&((struct P*)0)->m` idiom (== offsetof), so no
// <stddef.h> preprocessing is needed. char/unsigned/double widths and these offsets
// are identical under LP64 and LLP64, so ONE exit code holds on all four targets and
// under the shipped `release` pipeline.
//
// exit = sizeof(P)(16) + offsetof(P,v)(1) + offsetof(P,d)(8) + 17 = 42.
//        an unpacked member would give 16 + 4 + 8 + 17 = 45.

struct P { char c; unsigned v __attribute__((packed)); double d; };

// `seed` is a runtime argument, so the unaligned store+load cannot be constant-folded
// away in the baseline pipeline — the misaligned 4-byte accesses at offset 1 really
// execute.
int run(int seed) {
    struct P s;
    s.c = 7;
    s.v = 0xDEADBEEFu ^ (unsigned)seed;                     // unaligned 4-byte store @1
    s.d = 2.5 + (double)seed;
    if (s.v != (0xDEADBEEFu ^ (unsigned)seed)) return 100;  // unaligned 4-byte load  @1
    if (s.d != 2.5 + (double)seed)             return 101;
    if (s.c != 7)                              return 102;
    return 0;
}

int main(void) {
    if (run(1) != 0) return 1;   // the misaligned read+write must round-trip
    int sz = (int)sizeof(struct P);                  // 16
    int ov = (int)(long long)&((struct P *)0)->v;    // offsetof(P,v) == 1  (RED: 4)
    int od = (int)(long long)&((struct P *)0)->d;    // offsetof(P,d) == 8
    return sz + ov + od + 17;                        // 16 + 1 + 8 + 17 = 42
}
