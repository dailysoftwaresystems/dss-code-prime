/* Inline-asm P20 — `asm goto`: the label placeholder, the fall-through edge, the
 * symbolic operand name, and the phi copies on the label edge.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. Until this cycle the corpus had ZERO end-to-end
 * coverage of `asm goto`, and two of its siblings say so in writing:
 * `c_inline_asm` records that the label section is "deliberately absent", and
 * `asm_bare_spelling` records that `%l[label]` is "DELIBERATELY ABSENT … it would
 * redden this example for another row's reason". Both notes retire here.
 *
 * ★★★ THE SHAPE THAT MATTERS MOST IS THE ONE THAT LOOKS LIKE NOTHING: the
 * FALL-THROUGH. ✔MEASURED before this cycle, through the shipped CLI, the MIR
 * `asm goto` terminator recorded successors = the LABELS ONLY — one label gave
 * "has 1 label edge(s)", two gave 2, and the statements AFTER the asm contributed
 * no successor at all. They were built into a block `hir_to_mir` called dead and
 * the mandatory unreachable-prune deleted them. ✔MEASURED on gcc 13.3.0, clang
 * 19.1.1 and aarch64-linux-gnu-gcc 13.3.0, `asm goto` FALLS THROUGH when the
 * template does not branch (exit 7 on the fall-through, 3 on the label). So the
 * fall-through path's code was silently dropped — invisible only because the LIR
 * tier refused to lower the statement at all, which is what turns a wrong-code
 * bug into a failed compile.
 *
 * ⇒ SHAPE 1 below is worth more than its four lines: an `asm goto` whose template
 * does NOT branch, whose fall-through path is the ONLY producer of its value.
 * Delete the fall-through successor and this example does not merely change
 * answer, it loses the code that computes one.
 *
 * ★★ THE POSITIONAL FORM NEEDS OPERANDS PRESENT OR IT PROVES NOTHING.
 * ✔MEASURED on both references by EXECUTION at two operand counts: a label's
 * index is `#outputs + #inputs + labelPosition`, NOT its position among the
 * labels. The two rules AGREE when there are no operands — `%l0` really is the
 * first label then, and both compilers accept it — and DIVERGE the moment an
 * operand exists, where `%l0` is operand 0 and both hard-error with
 * "'%l' operand isn't a label". SHAPE 2 therefore carries one output and one
 * input so that `%l2` is the only spelling that can be right.
 *
 * ★ WHY THE OUTPUT IS READ ON THE FALL-THROUGH PATH AND NOT ON THE LABEL PATH.
 * The piece is placed at the head of EVERY successor, so either path exercises
 * it; the fall-through was chosen because ✔the reference measurement settled
 * ACCEPTANCE of an output read on the jump path but NOT its validity, and an
 * example must not pin behaviour its own references leave open.
 *
 * ★★ SHAPE 4 IS THE ONE THAT NEEDS THE OPTIMIZER TO EXIST. It is a loop whose
 * LATCH is an `asm goto` branching back to its own block, so the block carries
 * loop phis AND has two successors. The phi-destination copies for the back edge
 * belong to that edge alone; placed inline before the terminator they also run
 * on the EXIT edge, which is the lost-copy miscompile reached through an
 * `asm goto` instead of through an ordinary conditional branch.
 * ✔MEASURED 2026-08-19 with `InlineAsmGoto` removed from
 * `terminatorOwnsEverySuccessorBranch`, this file against a copy of ITSELF with
 * only SHAPE 4 removed: this file exits 1 at `release` and 42 at the baseline,
 * while the SHAPE-4-less copy exits 42 at BOTH. ⇒ shape 4 is the sole
 * discriminator for that guard, and only the `release` arm sees it — a
 * baseline-only corpus would have shipped the defect green.
 *
 * ⚠ THE `volatile` SEEDS ARE LOAD-BEARING. Without them the release pipeline
 * folds the branch conditions to constants before lowering and the `asm goto`
 * stops being a branch at all — the example would still exit 42 while testing
 * nothing.
 */

volatile int dss_seed = 20;
volatile int dss_bound = 5;

#if defined(__x86_64__)

/* SHAPE 1 — THE FALL-THROUGH EDGE. `x == 0` ⇒ `jne` is not taken ⇒ control
 * reaches the statement after the asm. That statement is the only place `10`
 * comes from. */
static int dssFallsThrough(int x) {
    __asm__ goto ("cmpl $0, %0\n\tjne %l[jumped]" : : "r"(x) : "cc" : jumped);
    return 10;
jumped:
    return 20;
}

/* SHAPE 2 — THE POSITIONAL LABEL REFERENCE, with 1 output + 1 input so the index
 * base is a fact rather than a coincidence: the label is operand index 2.
 * ★ `"=&r"` is REQUIRED, not decoration: the template writes `%0` before it
 * reads `%1`, and both reference compilers share a register between a plain
 * `"=r"` output and an input — ✔MEASURED, and it destroyed the input in the
 * reference lane's own first probe. */
static int dssPositionalLabel(int x) {
    int r;
    r = 0;
    __asm__ goto ("movl $7, %0\n\tcmpl $0, %1\n\tjne %l2"
                  : "=&r"(r) : "r"(x) : "cc" : hit);
    return r;
hit:
    return 100;
}

/* SHAPE 3 — THE SYMBOLIC OPERAND NAME. ✔MEASURED before this cycle: this exact
 * statement was REFUSED — "'%[in]' names neither a register this target declares
 * nor one of the 2 operand(s) bound to this assembly template ('%0', '%1')" —
 * because the descriptor carried no name for an operand and the binding table
 * could only be keyed positionally. Both references accept it (GNU 6.47.2.3). */
static int dssSymbolicOperand(int a) {
    int out;
    out = 0;
    __asm__ ("movl %[in], %[out]" : [out] "=r"(out) : [in] "r"(a));
    return out;
}

/* SHAPE 4 — THE `asm goto` AS A LOOP LATCH: two successors, one of them THIS
 * block, so the block carries loop phis on both `e` and `i`. `dssLoopLatch(5)`
 * is 4 — `e` lags `i` by one iteration, and the copies that make it lag belong
 * to the BACK edge alone. */
static int dssLoopLatch(int n) {
    int i;
    int e;
    i = 0;
    e = -1;
loop:
    e = i;
    i = i + 1;
    __asm__ goto ("cmpl %1, %0\n\tjl %l[loop]" : : "r"(i), "r"(n) : "cc" : loop);
    return e;
}

/* SHAPE 5 — THREE LABELS AND TWO OPERANDS, WHICH IS WHAT MAKES THE INDEX BASE
 * FALSIFIABLE RATHER THAN MERELY CHECKED. `%l2` is `#outputs + #inputs + 0` =
 * the FIRST label; under a labels-only base it would be the THIRD. Both
 * spellings EXIST under both readings — that is the point — so a wrong base
 * does not refuse here, it BRANCHES SOMEWHERE ELSE. SHAPE 2's single label can
 * only produce a refusal under the same mutant, and a refusal cannot tell a
 * wrong index from an unimplemented one.
 * ✔MEASURED 2026-08-19 with `mintTemplateSpellings`' base forced to 0: this
 * shape, ISOLATED in its own file, exits 33 at BOTH configs — the THIRD label,
 * reached silently. ⚠ In THIS file the same mutant is caught earlier, by
 * SHAPE 2's `'%l2' … is none of the 2 labels bound` refusal, so the composite
 * example reddens for SHAPE 2's reason; SHAPE 5's distinct contribution is that
 * the defect it detects is a WRONG BRANCH rather than a failed build, and that
 * had to be measured in isolation to be claimed. */
static int dssThreeLabels(int x) {
    int r;
    r = 0;
    __asm__ goto ("movl $3, %0\n\tcmpl $0, %1\n\tjne %l2"
                  : "=&r"(r) : "r"(x) : "cc" : one, two, three);
    return r;
one:
    return 11;
two:
    return 22;
three:
    return 33;
}

/* SHAPE 6 — A **PINNED** OUTPUT ON AN **UNCONDITIONALLY BRANCHING** TEMPLATE,
 * AND IT IS x86-ONLY FOR A REASON THE TARGET DECLARES.
 *
 * ★★ THIS SHAPE WAS FOUND BY AN ADVERSARIAL AUDIT OF THE CYCLE THAT ADDED THE
 * OTHERS, WHICH IS WHY IT IS WORTH ITS SPACE. Nothing covered it: the two LIR
 * pins use either NO outputs or a CONDITIONAL template, and every other shape in
 * this file uses `"=&r"`, which is UNPINNED — the template writes the operand's
 * own vreg and no capture is emitted at all.
 *
 * ★ WHAT MAKES IT THE ODD ONE OUT: a pinned output must be read OUT of its
 * machine register, and on an `asm goto` the block ends with the template's own
 * branch — so the capture cannot follow the template, it has to sit on each
 * EDGE. When the template branches UNCONDITIONALLY the fall-through edge gets no
 * capture and no branch (there is nothing left to emit), while the MIR
 * fall-through landing block still holds the result piece. The capture register
 * therefore has a def on the label edge and none on the fall-through.
 * ✔MEASURED through the shipped CLI: compile 0 and **run 42 at debug AND
 * release** — the fall-through block is unreachable and nothing miscompiles.
 * The audit could not settle that by reading; this file settles it by running.
 *
 * ⚠ NO aarch64 ARM EXISTS AND THAT IS NOT AN OMISSION: `arm64.target.json`
 * declares the constraint letters `r`, `w`, `m`, `i` and **none of them pins a
 * register**, so a pinned output is unspellable on that target. AArch64 genuinely
 * has no register-pinning constraint letter — which is the same fact that makes
 * every aarch64 asm OUTPUT go through a placeholder.
 *
 * ★ The label path RETURNS the captured value rather than a constant, so the
 * capture is what the exit code depends on. A constant there would have made
 * this shape prove only that the program did not crash. */
static int dssPinnedUnconditional(void) {
    int r;
    r = 0;
    __asm__ goto ("movl $9, %%eax\n\tjmp %l[done]" : "=a"(r) : : : done);
    return 100 + r;   /* unreachable — the template always branches */
done:
    return r;         /* 9, iff the pinned capture happened on THIS edge */
}

#elif defined(__aarch64__)

/* ★ WHY THE OPERANDS ARE `long` ON THIS ARCHITECTURE. The natural aarch64
 * spelling for a 32-bit value is `%w0`, and `%w` is an operand MODIFIER — a
 * narrower VIEW of the bound register — which no shipped target declares a
 * vocabulary for, so the semantic tier refuses it rather than silently running
 * at the wrong width. Widening the operands is the shape that is actually
 * expressible here; the sibling `c_inline_asm_operands` carries the same note
 * and the same reason. */
static int dssFallsThrough(long x) {
    __asm__ goto ("cmp %0, #0\n\tb.ne %l[jumped]" : : "r"(x) : "cc" : jumped);
    return 10;
jumped:
    return 20;
}

static int dssPositionalLabel(long x) {
    long r;
    r = 0;
    __asm__ goto ("mov %0, #7\n\tcmp %1, #0\n\tb.ne %l2"
                  : "=&r"(r) : "r"(x) : "cc" : hit);
    return (int)r;
hit:
    return 100;
}

static int dssSymbolicOperand(long a) {
    long out;
    out = 0;
    __asm__ ("mov %[out], %[in]" : [out] "=r"(out) : [in] "r"(a));
    return (int)out;
}

static int dssLoopLatch(long n) {
    long i;
    long e;
    i = 0;
    e = -1;
loop:
    e = i;
    i = i + 1;
    __asm__ goto ("cmp %0, %1\n\tb.lt %l[loop]" : : "r"(i), "r"(n) : "cc" : loop);
    return (int)e;
}

static int dssThreeLabels(long x) {
    long r;
    r = 0;
    __asm__ goto ("mov %0, #3\n\tcmp %1, #0\n\tb.ne %l2"
                  : "=&r"(r) : "r"(x) : "cc" : one, two, three);
    return (int)r;
one:
    return 11;
two:
    return 22;
three:
    return 33;
}

#else
#error "asm_goto_labels: no arm for this architecture — add one rather than \
letting the example pass without exercising an `asm goto` label edge"
#endif

int main(void) {
    int seed;
    int bound;
    int a;
    int b;
    int c;
    int d;
    int e;
    int f;

    seed  = dss_seed;           /* 20 */
    bound = dss_bound;          /*  5 */

    /* SHAPE 1, both directions. `a` exists only if the fall-through edge does. */
    a = dssFallsThrough(0);     /* fall-through -> 10 */
    if (a != 10) return 1;
    b = dssFallsThrough(1);     /* label        -> 20 */
    if (b != 20) return 1;

    /* SHAPE 2, both directions. The fall-through arm ALSO reads the asm's
     * output, which is what makes it the pin for the result-piece path. */
    c = dssPositionalLabel(0);  /* fall-through -> the template's 7 */
    if (c != 7) return 1;
    if (dssPositionalLabel(1) != 100) return 1;

    /* SHAPE 3 — the symbolic operand name reaches the template. */
    d = dssSymbolicOperand(seed);
    if (d != 20) return 1;

    /* SHAPE 4 — the loop-latch `asm goto`: `e` lags `i` by one iteration. */
    e = dssLoopLatch(bound);
    if (e != 4) return 1;

    /* SHAPE 5, both directions. `%l2` names the FIRST of three labels; the
     * THIRD (33) is what a labels-only index base would reach. */
    f = dssThreeLabels(1);      /* label `one`  -> 11 */
    if (f != 11) return 1;
    if (dssThreeLabels(0) != 3) return 1;   /* fall-through -> the template's 3 */

#if defined(__x86_64__)
    /* SHAPE 6 — the pinned capture on the label edge. Guarded rather than given a
     * trivial aarch64 twin: that target declares no register-pinning constraint
     * letter, so a stand-in would assert nothing while looking like coverage. */
    if (dssPinnedUnconditional() != 9) return 1;
#endif

    /* 10 + 20 + 7 + 4 + 11 == 52, plus the seed's 20 minus 30 == 42. Every
     * branch above already returned 1 on a mismatch, so this arithmetic is a
     * second, weaker check kept only so the exit code is a FUNCTION of the
     * values rather than a constant a miscompile could still produce. */
    return (a + b + c + e + f + seed - 30 == 42) ? 42 : 1;
}
