/* D-ASM-DUPLICATE-SYMBOLIC-NAME-BINDS-THE-WRONG-OPERAND: ONE symbolic name used
 * TWICE in a single inline-asm statement. All three collisions are S006C,
 * because the operands' `[name]` labels and the `asm goto` label list are ONE
 * name space per statement.
 *
 * ★★★ THIS FILE PINS A REFUSAL THAT DID NOT EXIST ONE CYCLE AGO, AND THE CYCLE
 * THAT MADE IT NECESSARY IS THE ONE THAT ADDED `%[name]` BINDING. Before that,
 * a repeated name failed the build by accident — the form matched no binding and
 * was refused by name. ✔MEASURED through the shipped CLI at the commit before
 * the fix, `[out] "=r"(r), [v] "=r"(d) : [v] "r"(a)` with `a == 20` compiled
 * rc=0 at debug AND release and the program returned 0: `%[v]` bound the OUTPUT,
 * because every spelling lookup below the front end is a FIRST-MATCH scan.
 *
 * ★★ WHY ALL THREE STATEMENTS ARE HERE AND NOT JUST THE MISCOMPILING ONE.
 * ✔MEASURED 2026-08-19 on gcc 13.3.0 and clang 19.1.1, each shape probed
 * separately, `-std=gnu17`: all three are "duplicate 'asm' operand name" /
 * "duplicate use of asm operand name". DSS accepted all three; only the FIRST
 * produced a wrong answer, and the other two would have been classified as
 * harmless by anyone who probed the operand pair alone. NOT ONE reference
 * accepts them, which is what makes acceptance the defect.
 *
 * ⚠ THE THIRD STATEMENT IS THE ONE A NARROWER FIX WOULD MISS. `%[v]` and
 * `%l[v]` are DIFFERENT template forms, so nothing about it is ambiguous inside
 * DSS — it is refused because GNU keeps one name space, not because a lookup
 * could go wrong. A check written over the operand list alone stays green here.
 *
 * ★ EVERY NAME IS OTHERWISE WELL-FORMED: `hit`/`again` are real defined labels,
 * every local is read, and no constraint letter is undeclared — so no diagnostic
 * below is about anything except the repeated name.
 *
 * RED-on-disable: delete the duplicate-name scan -> all three statements compile
 * clean, the golden goes EMPTY, and this harness refuses an empty golden
 * outright. Narrow the scan to operands only -> the second and third lines
 * disappear while the first survives, which is the asymmetry that names which
 * half broke. Drop the `related` location -> every line loses its `related=[…]`
 * and all three mismatch at once.
 */
int main(void) {
    int r;
    int d;
    int a;
    r = 0;
    d = 1;
    a = 2;
    /* two OPERANDS: the shape that miscompiled. */
    __asm__ ("movl %[v], %[out]" : [out] "=r"(r), [v] "=r"(d) : [v] "r"(a));
    /* two `asm goto` LABELS. */
    __asm__ goto ("jmp %l[hit]" : : "r"(a) : : hit, hit);
    /* an OPERAND and a LABEL: two distinct template forms, one name space. */
    __asm__ goto ("jmp %l[again]" : : [again] "r"(a) : : again);
    return r + d + a;
hit:
    return 1;
again:
    return 2;
}
