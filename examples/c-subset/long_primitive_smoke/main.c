// D-CSUBSET-LONG-PRIMITIVE (cycle 10h, 2026-06-04):
// Smoke-pin the `long` keyword as an I64-core primitive in
// c-subset. Pre-10h the language declared only `int` (I32),
// `char` (Char), and `void` (Void); `long` was unrecognised by
// the tokenizer + parser + builtinTypes lookup.
//
// Why a new primitive opens the alias arc capstone: under
// c-subset's existing `strictAliasingOnDistinctTypes: true`,
// `mirMayAlias` Rule 6 (distinct non-char primitives → No alias)
// requires two non-char primitive pointee types. Pre-10h the
// only available pair was `int*` vs `char*`, which Rule 5 (char-
// exception) handles first. Adding `long`→I64 yields the `int*`
// vs `long*` pair — distinct non-char primitives — which is the
// substrate Rule 6 actually targets. Cycle 10i
// (D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT) builds the two-armed
// CSE capstone on this pair.
//
// Witness-value discipline (post-fold 10h, multi-reviewer
// convergent FOLD-NOW): the sentinel is `0 - 1` (= -1) NOT a
// small positive literal. The reason is discrimination:
//   * Small positive witness (e.g., 42): sext(I32, 0x2A) and
//     zext(I32, 0x2A) both yield 0x000000000000002A; a bug that
//     swaps sext for zext is undetectable. Also, an ABI bug
//     that returns only the low-32 of the return register still
//     yields 42.
//   * Negative witness via `0 - 1`: sext extends 0xFFFFFFFF →
//     0xFFFFFFFFFFFFFFFF; zext (wrong) extends 0xFFFFFFFF →
//     0x00000000FFFFFFFF. Same for ABI: a 32-bit return-reg
//     truncation leaves upper-32 as garbage; the I64==I64
//     compare against a same-shape sentinel surfaces the
//     mismatch as exit 0.
//
// Shape: `get_neg_one_long` materialises -1 via `0 - 1` (no
// negative-literal syntax in c-subset) and returns it. `main`
// computes the same sentinel locally and compares; matching
// sentinels drive exit 42, any mismatch (caused by any of: long
// keyword admit, I64 ABI return, I64 local lowering, I64==I64
// comparison, sext I32→I64 encoding) drives exit 0.
//
// CPU-agnostic: the comment names "return register" without
// nailing it to rax/x0/etc — the substrate's calling convention
// (Win64 / SysV / AAPCS64) selects the actual platform register.

long get_neg_one_long() {
    long x;
    x = 0 - 1;
    return x;
}

int main() {
    long y;
    long w;
    int r;
    y = get_neg_one_long();
    w = 0 - 1;
    r = 0;
    if (y == w) {
        r = 42;
    }
    return r;
}
