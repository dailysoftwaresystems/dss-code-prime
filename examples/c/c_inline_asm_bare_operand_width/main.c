/* THE WIDTH A **BARE** `%N` SUBSTITUTES AT — END TO END, BY EXECUTION, AND
 * WITH THE TWO PORTS GIVING TWO DIFFERENT CORRECT ANSWERS TO ONE QUESTION.
 *
 *     D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE
 *
 * ★★★ WHAT WAS WRONG, AND IT WAS A SILENT MISCOMPILE RATHER THAN A REFUSAL.
 * A modifier-less `%1` used to substitute at the OPERAND'S OWN C-TYPE width on
 * every target — `char`→8 … `long`→64. That is exactly right on x86-64 and
 * exactly wrong on AArch64, where both references name the 64-bit `x` register
 * for every integer type and expect `%w` when 32 bits is meant. The old build
 * emitted `str w28, [x29]` where gcc emits `str x1, [x0]`: a 4-byte store
 * against an 8-byte one, from the same source text, with no diagnostic at
 * either end. Shape 1 below is that program, and it returned the wrong answer.
 *
 * ★★★ WHY THE ANSWER COULD NOT BE HARD-CODED, AND WHY IT COULD NOT BE DERIVED
 * EITHER. ✔MEASURED 2026-08-27 out of `-S`, gcc 13.3.0 AND clang 19.1.1, both
 * ports, `-O0` and `-O2`, on `__asm__("BARE %0 END" : : "r"(v))`:
 *
 *     aarch64  ->  x0  for _Bool, char, unsigned char, short, int,
 *                      unsigned int, long, long long, void * AND __int128
 *     x86-64   ->  %al / %ax / %eax / %rax, tracking the type exactly
 *
 * The two references AGREE WITH EACH OTHER and DISAGREE ACROSS PORTS, so a
 * single rule fixes one machine by breaking the other. And no arithmetic over
 * the declared views reproduces both: *the narrowest view at least as wide as
 * the type* yields `w0` for an `int` on aarch64, which renders `x0`; *the
 * widest view* yields `%rax` for an `int` on x86-64, which renders `%eax`.
 * ⇒ the derivation is a DECLARED per-target, per-register-class policy
 * (`asmBareOperandWidths` in `.target.json`), not a branch and not a formula.
 *
 * ★★★ THE ONE SEED AND THE ONE OPERAND ARE THE WHOLE DESIGN OF THIS FILE.
 * Every shape below feeds the SAME `int` value into the SAME 64-bit object,
 * and the two ports are asserted to land on DIFFERENT values — each port's
 * correct answer is the other port's BUG. A build that applied one rule
 * everywhere fails on exactly one of the two, whichever rule it picked, so
 * neither arm can be satisfied by a coincidence.
 *
 * ⚠ EVERY ASSERTION READS **ABOVE BIT 31**, AND THAT IS THE ONE THING THIS FILE
 * MUST NOT GET WRONG — the same trap the sibling
 * `c_inline_asm_width_view_modifier` records. A 4-byte and an 8-byte store of
 * the same small value are INDISTINGUISHABLE through `(int)`, which is the
 * observation under which this defect shipped for two cycles. So the seed's
 * high half is non-zero (`0x11223344`), every check compares a full
 * `long long`, and shape 5 is the matched NEGATIVE CONTROL that asserts the
 * blindness directly rather than leaving a reader to assume the wide reads
 * were merely thorough.
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING: without it the release pipeline folds
 * the value before lowering and the `release` arm stops exercising the
 * template at all.
 */

/* 0x1122334455667788 — the high half is non-zero so that a 4-byte store and an
 * 8-byte store of the same value are TELLABLE APART. */
#define DSS_SEED 0x1122334455667788LL

/* What a 4-byte store of 42 leaves behind: the low word replaced, the high
 * word of the seed still standing. Written as an expression over DSS_SEED so
 * the two constants cannot drift apart. */
#define DSS_NARROW_RESULT ((DSS_SEED & ~0xFFFFFFFFLL) | 42LL)

volatile long long dss_cell = DSS_SEED;
volatile int       dss_v    = 42;

/* Reset the cell between shapes. `volatile` on both sides keeps the release
 * pipeline from carrying a known value across a template it cannot see into. */
static void dssReset(void) { dss_cell = DSS_SEED; }

int main(void) {
    long long seen;

#if defined(__aarch64__)

    /* 1 — THE SUBJECT, and it is the row's own witness program. `str` writes
     * the width its REGISTER names, and on this CPU a bare `%1` names the full
     * `x` register, so an `int` operand still produces an EIGHT-byte store.
     * Under the old type-driven rule this emitted `str w…` and left the seed's
     * high half standing — the miscompile, observable right here. */
    dssReset();
    __asm__ ("str %1, %0" : "=m"(dss_cell) : "r"(dss_v));
    seen = dss_cell;
    if (seen != 42LL) return 1;

    /* 2 — THE MODIFIER STILL WINS, AND IT WINS IN THE DIRECTION THAT PROVES IT
     * IS READ. `%w` asks for the 32-bit view of the very same operand, so the
     * store narrows to four bytes and the seed's high half survives. A build
     * that had "fixed" shape 1 by ignoring the letters would return the shape-1
     * answer here and fail. */
    dssReset();
    __asm__ ("str %w1, %0" : "=m"(dss_cell) : "r"(dss_v));
    seen = dss_cell;
    if (seen != DSS_NARROW_RESULT) return 2;

    /* 3 — THE COINCIDENCE, STATED AS A CONTROL. At `long long` the two
     * derivations AGREE — both say 64 — which is precisely why this divergence
     * survived so long: every inline-asm example that shipped before it used
     * `long`/`long long` on this port, so nothing ever disagreed. */
    dssReset();
    {
        volatile long long wide = 42;
        __asm__ ("str %1, %0" : "=m"(dss_cell) : "r"(wide));
    }
    seen = dss_cell;
    if (seen != 42LL) return 3;

    /* 4 — A `char` OPERAND, WHICH THIS CPU COULD NOT BIND AT ALL BEFORE.
     * AArch64 has no 8-bit GPR view, so a type-driven width had nothing to
     * name; under the reference rule a `char` rides the full register like
     * every other integer and the shape simply works. */
    dssReset();
    {
        volatile char narrow = 42;
        __asm__ ("str %1, %0" : "=m"(dss_cell) : "r"(narrow));
    }
    seen = dss_cell;
    if (seen != 42LL) return 4;

#elif defined(__x86_64__)

    /* 1 — THE MATCHED OPPOSITE, AND IT IS WHAT STOPS THE FIX FROM BEING A
     * HARD-CODED 64. On this CPU a bare `%1` names the view that matches the
     * operand's own type, so the same `int` through the same one-instruction
     * template writes only FOUR bytes and the seed's high half stands. A build
     * that had adopted AArch64's rule globally would either widen this store
     * (failing here) or fail to build at all, because `movl` declares width 32
     * and the width-honesty gate refuses a 64-bit register under it. */
    dssReset();
    __asm__ ("movl %1, %0" : "=m"(dss_cell) : "r"(dss_v));
    seen = dss_cell;
    if (seen != DSS_NARROW_RESULT) return 1;

    /* 2 — THE MODIFIER STILL WINS, mirroring the AArch64 arm: `%k` asks for the
     * 32-bit view of a `long long` operand, narrowing a store that would
     * otherwise have been eight bytes wide. Same assertion, other direction. */
    dssReset();
    {
        volatile long long wide = 42;
        __asm__ ("movl %k1, %0" : "=m"(dss_cell) : "r"(wide));
    }
    seen = dss_cell;
    if (seen != DSS_NARROW_RESULT) return 2;

    /* 3 — THE COINCIDENCE, the same control as the other arm: at `long long`
     * both derivations say 64 and the store is eight bytes wide. */
    dssReset();
    {
        volatile long long wide = 42;
        __asm__ ("movq %1, %0" : "=m"(dss_cell) : "r"(wide));
    }
    seen = dss_cell;
    if (seen != 42LL) return 3;

    /* 4 — A `char` OPERAND, which on THIS port names the 1-byte view and has
     * worked all along. Kept so both arms exercise the same five shapes and a
     * reader can diff them: the sub-native operand is the case the sibling row
     *
     *     D-ASM-SUB-NATIVE-OPERAND-UNUSABLE-IN-INLINE-ASM
     *
     * closed here first, and the AArch64 arm above is what the reference rule
     * gives it on the other port. */
    dssReset();
    {
        volatile char narrow = 42;
        __asm__ ("movb %1, %0" : "=m"(dss_cell) : "r"(narrow));
    }
    seen = dss_cell;
    if (seen != ((DSS_SEED & ~0xFFLL) | 42LL)) return 4;

#else
#error "c_inline_asm_bare_operand_width: no arm for this architecture — add \
one rather than letting the example pass without exercising a bare operand"
#endif

    /* 5 — ⚠ THE NEGATIVE CONTROL, AND THE REASON EVERY CHECK ABOVE IS WIDE.
     * Through `(int)` a 4-byte store and an 8-byte store of 42 are EQUAL. This
     * is the exact observation under which the wrong width shipped, so the
     * example asserts that the narrow view is blind rather than leaving the
     * reader to infer it. Both ports run this one. */
    dssReset();
    {
        long long const narrowStore = DSS_NARROW_RESULT;
        long long const wideStore   = 42LL;
        if (narrowStore == wideStore) return 5;            /* wide: they differ */
        if ((int)narrowStore != (int)wideStore) return 6;  /* narrow: blind */
    }

    return 42;
}
