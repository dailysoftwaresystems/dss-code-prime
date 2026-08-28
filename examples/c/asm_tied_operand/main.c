/* `"+r"` — a GNU extended `__asm__` READ-WRITE operand, end to end.
 * D-LIR-TIED-OPERAND-NOT-EXPRESSIBLE.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. `"+r"` is one source operand naming two machine
 * facts: a value the template READS and a location it WRITES. Until this cycle
 * the program below did not compile at all — `mir_to_lir.cpp`'s
 * `bindAsmOperand` refused every read-write constraint outright, because when
 * that refusal was written no MIR operand carried the read half. It does now
 * (`hir_to_mir.cpp` appends a matching-constraint input per `+` output — GNU's
 * own 6.47.2.4 transformation), and what LIR owes is to bind BOTH halves to ONE
 * register. This file is the runtime witness that it does.
 *
 * ✔MEASURED 2026-08-17 before the change, through the real CLI on
 * `x86_64:pe64-x86_64-windows-exec`, debug AND release:
 *   `int main(void){ int x; x = 40; __asm__("addl $2, %0" : "+r"(x)); return x; }`
 *   → rc=1, `error[L_UnsupportedLoweringForOpcode] … output 0 uses the
 *   read-write constraint "+r"`.
 * `"+r"` is ordinary in real systems C — ✔MEASURED, gcc 13.3.0 builds that
 * exact program and it exits 42 — so the refusal was a conformance gap, not an
 * exotic corner. ⚠ clang is NOT installed on this machine, so every reference
 * figure in this file names gcc 13.3.0, the compiler it was run against.
 *
 * ★★★ WHY THE DISCRIMINATING SHAPE HAS **TWO** TIED OPERANDS, AND WHY THAT IS A
 * MEASUREMENT RATHER THAN A STYLE CHOICE.
 * The mutant this example exists to catch deletes the tie (the read half is
 * materialised into a FRESH vreg, so the template reads a register nothing
 * wrote). ✔MEASURED 2026-08-17, mutant applied and the binary PROVEN rebuilt on
 * every transition:
 *
 *   ONE tied operand (SHAPE C below, alone)     → baseline 42, release 42
 *                                                 ⇒ THE MUTANT SURVIVES BOTH ARMS
 *   TWO tied operands in one template           → baseline 11, release 11  (RED)
 *   TWO tied + a source input, behind a call    → baseline 11, release 11  (RED)
 *   THREE tied operands                         → baseline 11, release 11  (RED)
 *
 * ★★ THE MECHANISM, because it generalises. Under the mutant the read half's
 * `mov` targets a DEAD vreg emitted immediately before the template, and the
 * template's own result vreg starts its live range right after it — so the two
 * do not overlap and the linear scan hands the result the register the dead
 * `mov` just filled with exactly the right value. With TWO tied operands the
 * two result vregs overlap EACH OTHER, cannot both inherit, and the luck breaks.
 * ⇒ ★★★ A SINGLE-`"+r"` EXAMPLE IS A VACUOUS PIN NO MATTER WHICH ARM IT RUNS ON,
 * AND ADDING A `release` ARM DOES NOT FIX IT. ⛔ Do not "simplify" this file down
 * to the one-operand spelling; the four rows above are why the others are here.
 *
 * ★ SHAPE C IS KEPT ANYWAY, deliberately. It is the ORDINARY spelling and the
 * exact program the anchor names, so it is the coverage a reader comes looking
 * for — it simply is not the arm that does the discriminating, and saying so is
 * the honest version of shipping it.
 *
 * ★ THE ARITHMETIC IS CHAINED AND EVERY CHECK HAS ITS OWN EXIT CODE, so a
 * failure names WHICH shape broke instead of collapsing to a single "not 42".
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING. Without it the release pipeline folds
 * every operand to a constant before lowering. That does not make the pin
 * vacuous — a constant still has to be materialised into the bound register —
 * but it changes WHICH mechanism each arm tests, and an example whose subject
 * silently differs between arms is not a pin.
 */

volatile int dss_seed = 20;

#if defined(__x86_64__)

/* SHAPE B — TWO tied operands PLUS a source input, behind a CALL.
 *
 * ★ IT ALSO EXERCISES THE OPERAND ORDER, which nothing else here does: the
 * synthesized tied entries are APPENDED AFTER every source-written input, so
 * `%2` is the source input and the two tied reads occupy the indices past it.
 * A producer that INSERTED them instead would silently renumber `%2` and this
 * arm would compute with the wrong value.
 *
 * ⚠ THE OPERANDS ARE LOCALS, NOT THE PARAMETERS, AND THAT IS AN ANCHORED DEFECT
 * RATHER THAN A STYLE CHOICE — see
 * D-CSUBSET-ASM-OUTPUT-ON-A-PARAMETER-NOT-ADDRESS-TAKEN.
 * ⚠ AN ANCHOR NAME IS NEVER WRAPPED ACROSS TWO LINES: the registry guard scans
 * for the literal string, so a hyphen-broken name is cited as a TRUNCATED
 * anchor that matches no row — measured, this file did exactly that once.
 * ✔MEASURED 2026-08-17 through the real CLI: an asm output bound to a
 * PARAMETER is refused with
 *   `error[H0009] … symbol 86 has no storage slot (non-addressable param or
 *    unbound) — required by lvalue use`
 * because an asm OUTPUT takes its lvalue's ADDRESS and nothing marks the
 * parameter address-taken. ★ IT IS NOT A `+` DEFECT: a plain `"=r"(v)` on a
 * parameter fails identically, and a `"+r"(v)` on a parameter whose address is
 * ALSO taken explicitly COMPILES AND EXITS 42 — which is what names the cause.
 * ✔MEASURED on gcc 13.3.0: BOTH parameter spellings (`"+r"(v)` and `"=r"(v)`)
 * compile and exit 42. Pre-existing, FAIL-LOUD rather than
 * a silent miscompile, and in the addressability marking (outside this lane's
 * paths) — so it is anchored, not routed around quietly. When it closes, the
 * parameters belong directly in the constraints here. */
static int dssAsmMix(int p, int q, int s) {
    int a; int b;
    a = p;
    b = q;
    __asm__ ("addl %2, %0\n\taddl %2, %1"
             : "+r"(a), "+r"(b)
             : "r"(s)
             : "cc");
    /* BOTH results are folded into the return value, so neither tie can break
     * unnoticed. */
    return (a * 1000) + b;
}

#elif defined(__aarch64__)

/* ★ WHY THE OPERANDS ARE `long`. The natural 32-bit aarch64 spelling is
 * `add %w0, %w0, %w2`, and `%w` is an operand MODIFIER — a narrower VIEW of the
 * bound register. No shipped target declares a width-view vocabulary, so the
 * semantic tier REFUSES `%w` fail-loud rather than silently running the
 * operation at the wrong width (D-CSUBSET-INLINE-ASM-OPERANDS). Plain `%0` on
 * 64-bit operands is the shape that is expressible today — the same reasoning,
 * and the same measurement, as the sibling `c_inline_asm_operands`.
 * ⓘ See the x86_64 arm for why the operands are locals rather than parameters. */
static int dssAsmMix(int p, int q, int s) {
    long a; long b; long t;
    a = p;
    b = q;
    t = s;
    __asm__ ("add %0, %0, %2\n\tadd %1, %1, %2"
             : "+r"(a), "+r"(b)
             : "r"(t));
    return (int)((a * 1000) + b);
}

#else
#error "asm_tied_operand: no arm for this architecture — add one rather than \
letting the example pass without exercising a read-write asm operand"
#endif

int main(void) {
#if defined(__x86_64__)
    int a; int b; int c;
#else
    long a; long b; long c;
#endif

    /* ── SHAPE A — TWO tied operands, lowered HERE rather than behind a call.
     * This is the arm that reddens under the mutant. */
    a = dss_seed;                       /* 20 */
    b = dss_seed + 2;                   /* 22 */
#if defined(__x86_64__)
    __asm__ ("addl %1, %0" : "+r"(a), "+r"(b) : : "cc");
#else
    __asm__ ("add %0, %0, %1" : "+r"(a), "+r"(b));
#endif
    if (a != 42) return 11;             /* a += b */
    if (b != 22) return 12;             /* b is read-write but unmodified */

    /* ── SHAPE C — the ORDINARY single-`"+r"` spelling, and the exact program
     * the anchor names. Coverage, not the discriminator (see the header). */
    c = dss_seed + 20;                  /* 40 */
#if defined(__x86_64__)
    __asm__ ("addl $2, %0" : "+r"(c) : : "cc");
#else
    __asm__ ("add %0, %0, #2" : "+r"(c));
#endif
    if (c != 42) return 13;

    /* ── SHAPE B — two tied operands + a source input, behind a call.
     * p=10, q=1, s=5  ⇒  a=15, b=6  ⇒  15006. */
    if (dssAsmMix(dss_seed - 10, dss_seed - 19, dss_seed - 15) != 15006) {
        return 14;
    }

    return 42;
}
