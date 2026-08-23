/* [[D-CSUBSET-INLINE-ASM-SPELLING]] — the BARE `asm` spelling, end to end.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Every other inline-asm fixture in this corpus
 * writes `__asm__`. That spelling is in C 7.1.3's implementation-reserved
 * namespace and is accepted by gcc and clang in EVERY standard mode, which is
 * exactly why it was the one DSS shipped first — and exactly why it cannot
 * witness this row. The bare word is the one whose status depends on which
 * dialect the compiler claims to be, and until 2026-08-17 DSS got it backwards
 * in BOTH directions at once.
 *
 * ✔MEASURED 2026-08-17, gcc 13.3.0 / clang 18.1.3 / clang 19.1.1 across
 * `c17` / `c23` / `gnu17` / `gnu23`, every arm carrying a positive control
 * (`__asm__`, accepted everywhere) and a negative control (`__asm__zz`,
 * rejected everywhere):
 *
 *              asm("…" : "=r"(x))      int asm = 7;
 *   ISO  mode        REJECT               ACCEPT
 *   GNU  mode        ACCEPT               REJECT
 *
 * The two answers are complementary — never both, in any compiler, in any
 * mode. And `-std=gnu*` is BOTH compilers' default.
 *
 * ★★ WHICH MODE IS DSS? IT ALREADY ANSWERED, IN THE MACHINE-READABLE WAY gcc
 * AND clang ANSWER IT. `__STRICT_ANSI__` is defined by all three under
 * `-std=c17` and by NONE of them under `-std=gnu17` or by default (✔MEASURED,
 * `-dM -E`). DSS defines `__GNUC__` and `__clang__` and does NOT define
 * `__STRICT_ANSI__` — so it declares GNU mode, and in GNU mode `asm` is a
 * keyword with no flag involved. That is why the fix is ONE keyword row and
 * not a standard-mode axis: nothing here is mode-conditional, because DSS does
 * not have two modes to be conditional on.
 *
 * ★★★ WHAT THIS FILE PINS, AND THE PART IT DELIBERATELY DOES NOT CLAIM.
 * `AsmKeyword` has TWO grammar consumers, and a spelling that reached only one
 * of them would look completely correct from either side alone — the
 * multi-site-contract trap. This file exercises both, but they are NOT equally
 * observable at runtime and the comment says so rather than implying parity:
 *
 *   SHAPE 1 — bare `asm` as a STATEMENT (`asmStmt`), lowered directly in
 *     `main`. FULLY runtime-observable: `m` is produced by the template, so if
 *     the input never reaches it or the block is dropped, the exit code is not
 *     42. (It is lowered in `main` rather than behind a call for the reason
 *     `c_inline_asm_operands` measured the hard way: routing an asm through a
 *     call let a live mutant survive the baseline arm, because the allocator
 *     happened to leave the right value in the register the template read.)
 *
 *   SHAPE 2 — bare `asm` as an ASM LABEL after a declarator (`asmLabel`).
 *     ⚠ ITS RUNTIME OBSERVABILITY IS WEAKER AND IS STATED HONESTLY: in a
 *     single translation unit the RENAME itself has no witness, because the
 *     call resolves internally whatever the symbol ends up called. What this
 *     shape does pin is that the bare spelling REACHES the after-declarator
 *     rule at all — without the keyword row the file does not compile, and if
 *     the label were mis-parsed (as an initializer, say) the returned value
 *     would be wrong rather than 22. Its 22 IS in the arithmetic, so it is not
 *     decoration; it is simply not a rename oracle. `examples/c-subset/
 *     asm_label` remains the fixture that pins renaming, in the `__asm__`
 *     spelling.
 *
 * ★ THE ARITHMETIC IS 20 + 22 AND NEITHER HALF IS A CONSTANT THE OPTIMIZER CAN
 * REACH PAST. `dss_seed` is `volatile`, so the release pipeline cannot fold the
 * template's input away and turn shape 1 into a materialised literal — the
 * same seeding `c_inline_asm_operands` documents. Any dropped or mis-lowered
 * half changes the exit code.
 *
 * ⛔ NOT EXERCISED HERE, AND NOT A GAP THIS ROW OWNS: `asm goto` with a
 * `%l[label]` operand. ✔MEASURED 2026-08-17 that it fails
 * `L_UnsupportedLoweringForOpcode` IDENTICALLY under BOTH `asm goto` and
 * `__asm__ goto`, so it is orthogonal to the spelling and is already carried by
 * [[D-ASM-DIALECT-DECLARES-NO-OPERAND-PLACEHOLDER]] (narrowed to its LABEL
 * half). Writing it into this file would make this example red for another
 * row's reason.
 */

volatile int dss_seed = 20;

/* SHAPE 2 — the bare spelling in the after-declarator position. ✔MEASURED that
 * gcc 13.3.0 and clang 19.1.1 both accept this bare form in GNU mode, on a
 * function declarator and on a global alike. */
static int dssBareLabelled(void) asm("dss_bare_renamed");

static int dssBareLabelled(void) {
    return 22;
}

int main(void) {
    int a;
    int r;
#if defined(__x86_64__)
    int m;
#elif defined(__aarch64__)
    long mx;
    long m;
#else
#error "asm_bare_spelling: no arm for this architecture — add one rather than \
letting the example pass without exercising a bare `asm` statement"
#endif

    a = dss_seed;   /* 20, through a volatile load the optimizer must keep */

    /* SHAPE 1 — bare `asm` as a statement, lowered HERE, not behind a call. */
#if defined(__x86_64__)
    m = 0;
    asm ("movl %1, %0" : "=r"(m) : "r"(a));
#else
    /* ★ WHY THE aarch64 ARM WIDENS TO `long`: the natural 32-bit spelling is
     * `mov %w0, %w1`, and `%w` is an operand MODIFIER no shipped target
     * declares a width-view vocabulary for — the semantic tier refuses it
     * fail-loud rather than running the move at the wrong width. Plain `%0` on
     * 64-bit operands is what is expressible today. Same reasoning, same
     * measurement, as `c_inline_asm_operands`. */
    mx = a;
    m  = 0;
    asm ("mov %0, %1" : "=r"(m) : "r"(mx));
#endif

    if ((int)m != 20) return 1;

    r = (int)m + dssBareLabelled();  /* 20 + 22 */
    return r;
}
