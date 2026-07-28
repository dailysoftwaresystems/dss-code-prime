// D-CSUBSET-GNU-ATTRIBUTE (TF-C72/C73) witness: the `packed` / `aligned`
// COMPOSITE and UNION-MEMBER shapes that the real macOS SDK headers use, gated
// on `sizeof`, `_Alignof` and a runtime ADDRESS — never on merely compiling.
//
// ★ SCOPE — this file is the COMPLEMENT of `examples/c-subset/gnu_aligned_attribute/`,
// not a duplicate of it. That example owns the POSITION axis for a lone
// `aligned(N)`: leading, after-declarator, composite after-keyword, struct
// member, composite trailing. This file owns what that axis cannot reach:
//   * `packed` COMBINED with `aligned` in one composite clause — the two
//     interact (packed removes internal padding, aligned raises the whole
//     composite), and a sink that honors one and drops the other produces a
//     layout no single-attribute example can distinguish;
//   * `packed` ALONE, where the observable is a SHRUNK struct (5/1), the
//     opposite direction from every `aligned` row and therefore a different
//     failure mode;
//   * the UNION member slot, which is a different config row from the struct
//     member slot (`unionField` vs `structField`) and had no runtime witness;
//   * `_Alignof` as an observable. The sibling example reads `sizeof` and
//     addresses only. `_Alignof` is the one surface that sees a raised
//     alignment on a composite whose SIZE did not change, which is exactly
//     what `aligned(16)` on an 8-byte struct does.
//
// ★★ THE macOS SDK SHAPES, AND CLANG'S ANSWER FOR THEM. Both were taken from
// the prompt of the audit that ordered this witness and both were then VERIFIED
// against real clang rather than assumed:
//     struct S { char a; int b; } __attribute__((packed, aligned(16)));
//         → sizeof 16, _Alignof 16      (NOT 5/16 — the raised alignment pads
//                                        the packed 5 bytes back out to 16)
//     struct S { char a; int b; } __attribute__((packed));
//         → sizeof 5,  _Alignof 1
//     typedef unsigned long long T __attribute__((aligned(8)));
//         → sizeof 8,  _Alignof 8
// MEASURED with `clang -isysroot $(xcrun --show-sdk-path)`, and DSS agrees on
// every one.
//
// ★★ NON-VACUITY, PROVEN PER CHECK — the whole point of this file.
// Every check below was isolated into its OWN single-check program and built
// twice: once as written, once with EXACTLY the attribute that check depends on
// deleted from the source. A check that returns 42 in both columns cannot fail
// and does not belong here. None of the shipped ones does. MEASURED (dss and
// clang produce identical columns):
//
//   #  check                          ON   attr deleted   packed-only   aligned-only
//   1  sizeof(PA)   == 16             42   1              1             42
//   2  _Alignof(PA) == 16             42   2              2             42
//   3  &pa   % 16   == 0              42   3              3             42
//   4  sizeof(PK)   ==  5             42   4              -             -
//   5  _Alignof(PK) ==  1             42   5              -             -
//   6  offset of PK.b == 1            42   6              -             -
//   7  sizeof(UM)   == 32             42   7              -             -
//   8  _Alignof(UM) == 32             42   8              -             -
//   9  &um.m % 32   == 0              42   9              -             -
//
// Read the last two columns, not just the reds. Rows 1/2/3 stay 42 when only
// `packed` is dropped and `aligned(16)` is kept — correctly: a raised 16-byte
// alignment produces the same size and address whether or not the interior was
// packed. That is not a weakness of those rows, it is the reason rows 4/5/6
// exist: the packed-ONLY struct is the shape where dropping `packed` is
// observable, and there it moves all three.
//
// ★ THE TYPEDEF ROW CARRIES NO CHECK, DELIBERATELY. `sdk_u64` below is the
// literal SDK spelling and it is DECLARED, DEFINED and RETURNED, so the shape is
// exercised end to end — but no `if` gates on it, because `aligned(8)` on
// `unsigned long long` asks for exactly the alignment the aliasee already has.
// MEASURED: with the attribute deleted the program still exits 42. It is vacuous
// by construction, not by placement, and no arrangement of padding can fix that.
// An honest absence beats a check that cannot fail. DSS's real, observable
// behavior for a typedef that asks for MORE than its aliasee is compile-time and
// fail-loud, which no running binary can witness:
//     typedef int T __attribute__((aligned(16)));
//     error[S002F] ... on a typedef cannot be honored: the alias resolves to the
//                  same type as its aliasee, whose alignment is 4
// (MEASURED this cycle; clang accepts that form and reports _Alignof 16, so the
// divergence is real and belongs in a unit pin, not in a green corpus example.)
//
// ★★ THE OFFSET BREAKERS (`data_pad_a`, `data_pad_b`, `bss_pad_c`) ARE
// LOAD-BEARING. DO NOT DELETE THEM, AND DO NOT ADD AN OVER-ALIGNED GLOBAL
// WITHOUT ONE. The first over-aligned object in a section lands at the section
// START, and every object format page-aligns section starts — so an address
// check on it holds whether or not the alignment sink ran at all. That exact
// vacuity was measured in a sibling example and cost three of its five checks.
// Each breaker is one byte of the SAME section class as the object it precedes
// (`data_pad_*` initialized → .data, ahead of the initialized `pa`/`pk`;
// `bss_pad_c` uninitialized → .bss, ahead of the uninitialized `um`), so the
// natural placement is NOT the aligned placement.
//
// ★★ VALID C, VERIFIED, NOT ASSUMED. `clang -fsyntax-only -Wall -Wextra
// -isysroot $(xcrun --show-sdk-path)`: ZERO errors, ZERO warnings; and the
// clang-built binary independently EXITS 42, so the expected exit code is
// ground truth from a real toolchain rather than DSS agreeing with itself. This
// is a hard requirement: invalid C shipped in this very corpus area for a full
// cycle and made its example prove the OPPOSITE of what it existed to prove.
// Re-run both checks if you touch this file.
//
// Front-end feature (attribute → layout sink) carried through HIR→MIR→asm,
// target/format-agnostic: x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O
// macos leg), baseline AND the shipped `release` pipeline — the optimized arm is
// mandatory, since the point is that a real optimizer PRESERVES the layout.

char data_pad_a = 1;                                    /* offset breaker (.data) */
struct PA { char a; int b; } __attribute__((packed, aligned(16)));
struct PA pa = {1, 2};

char data_pad_b = 1;                                    /* offset breaker (.data) */
struct PK { char a; int b; } __attribute__((packed));
struct PK pk = {1, 2};

union UM { char c; int m __attribute__((aligned(32))); };
char bss_pad_c;                                         /* offset breaker (.bss)  */
union UM um;

typedef unsigned long long sdk_u64 __attribute__((aligned(8)));
sdk_u64 sv = 42;

int main(void) {
    /* composite trailing: packed AND aligned(16) together */
    if (sizeof(struct PA) != 16u)                       return 1;
    if (_Alignof(struct PA) != 16u)                     return 2;
    if (((unsigned long long)(&pa) & 15ull) != 0ull)    return 3;

    /* composite trailing: packed ALONE — the shrink direction */
    if (sizeof(struct PK) != 5u)                        return 4;
    if (_Alignof(struct PK) != 1u)                      return 5;
    if ((unsigned long long)((char *)&pk.b - (char *)&pk) != 1ull) return 6;

    /* union MEMBER slot */
    if (sizeof(union UM) != 32u)                        return 7;
    if (_Alignof(union UM) != 32u)                      return 8;
    if (((unsigned long long)(&um.m) & 31ull) != 0ull)  return 9;

    return (int)sv;   /* 42 — the SDK typedef shape, exercised, not asserted */
}
