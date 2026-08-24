/* THE WIDTH-VIEW OPERAND MODIFIER — `%w0` / `%x0` on aarch64 and `%k0` / `%q0`
 * on x86-64 — END TO END AND BY EXECUTION (D-CSUBSET-INLINE-ASM-OPERANDS,
 * tranche 2 of D-ASM-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE).
 *
 * ★★★ THIS FORM WAS A REFUSAL UNTIL 2026-08-24 AND THE REFUSAL WAS RIGHT AT THE
 * TIME. `S0067` said *"a modifier asks for a narrower VIEW of the register the
 * operand was bound to, and no shipped target declares its width-view
 * vocabulary. Refusing: falling back to the full register would run the
 * operation at the WRONG WIDTH with a clean build log."* Both halves of that
 * were true: the vocabulary was undeclared, and the plausible wrong answer
 * always assembles. What changed is that the vocabulary is now DECLARED — per
 * DIALECT, in `assembly.templateModifiers` — so the refusal became the
 * divergence and this program is what proves it closed.
 *
 * ★★★ WHY THE LETTERS ARE PER DIALECT AND NOT ONE SHARED TABLE, WHICH IS THE
 * WHOLE REASON THE TWO ARMS BELOW SPELL DIFFERENT LETTERS FOR THE SAME IDEA.
 * ✔MEASURED 2026-08-24 by execution on gcc 13.3.0, both ports, `-O0` and `-O2`,
 * each port fed the same source: `%w0` renders `w0` — THIRTY-TWO bits — under
 * `aarch64-linux-gnu-gcc`, and `%ax` — SIXTEEN bits — under x86-64 gcc. One
 * letter, two widths. The 32-bit view is spelled `%w` on aarch64 and `%k` on
 * x86-64, and the 64-bit view `%x` and `%q`. A single table anywhere in shared
 * substrate would be wrong for one of its two readers by construction.
 *
 * ★★★ EVERY ASSERTION READS **ABOVE BIT 31**, AND THAT IS THE ONE THING THIS
 * FILE MUST NOT GET WRONG. ✔MEASURED that the obvious version of this program
 * asserts nothing: the same shapes written as `return (int)r;` exit 42 whether
 * the move was 32-bit or 64-bit, because the truncation destroys exactly the
 * evidence. So the seed's two halves are DIFFERENT (`0x11223344` above,
 * `0x55667788` below) and every check compares a full `long long`:
 *
 *     narrow view  -> 0x0000000055667788   (a 32-bit write zeroes bits 63:32
 *                                           on both CPUs)
 *     wide view    -> 0x1122334455667788
 *
 * ⇒ a modifier that was IGNORED (the pre-fix fallback: the full register) turns
 * shape 1 into `0x1122334455667788` and shape 1 fails. Shape 6 is the matched
 * NEGATIVE CONTROL: it compares the same two values through `(int)` and finds
 * them EQUAL, which is the observation that would have let a wrong width ship.
 *
 * ★★ THE WIDE VIEW IS NOT A SPARE — IT IS THE SECOND HALF OF THE DISCRIMINATION.
 * A build that honoured no letter at all and simply emitted the operand's own
 * register would pass every `%x`/`%q` shape and fail every `%w`/`%k` one; a
 * build that hard-coded "a modifier means 32" would pass the narrow shapes and
 * fail the wide ones. Only asserting BOTH says the declared width is the one
 * being read.
 *
 * ⚠ `long long` RATHER THAN `long` FOR EVERY 64-BIT SHAPE, AND IT IS A
 * DATA-MODEL FACT: one target here is LLP64, where `long` is 32 bits, so a
 * `long` operand would bind a 32-bit register and the wide-view shapes would be
 * refused on Windows while compiling on Linux — the divergence the shared width
 * check exists to make loud (the sibling `c_inline_asm_width_and_direction`
 * records the same trap, measured).
 *
 * ⚠ THE `volatile` SEED IS LOAD-BEARING: without it the release pipeline folds
 * the value to a constant before lowering and the `release` arm stops
 * exercising the template at all.
 *
 * ⚠ NO ARM HERE USES A MODIFIER ON AN `"m"` OR `"i"` OPERAND, and that is
 * deliberate rather than an omission. ✔MEASURED 2026-08-24 on gcc 13.3.0, both
 * ports, each shape compiled with and without the letter and the two outputs
 * compared byte for byte: on a memory or immediate binding the letter changes
 * NOTHING (`str w1, [x0]` either way; `movl $7, %eax` either way), because a
 * memory operand carries an address and an immediate IS its value — neither
 * states an operation width. DSS matches that, and an example asserting on it
 * would assert on an absence.
 */

/* 0x1122334455667788 — the two 32-bit halves differ, which is what makes every
 * assertion below discriminate above bit 31. */
volatile long long dss_seed = 0x1122334455667788LL;

#if defined(__x86_64__)

/* The 32-bit view: `%k` here. `movl` declares width 32, so the operand's own
 * 64-bit width would be REFUSED by the width-honesty gate — on this dialect the
 * modifier is what makes the statement compile at all, and its absence is loud
 * rather than silent. */
static long long dssNarrowView(long long v) {
    long long r;
    __asm__ ("movl %k1, %k0" : "=r"(r) : "r"(v));
    return r;
}

/* The 64-bit view: `%q`. */
static long long dssWideView(long long v) {
    long long r;
    __asm__ ("movq %q1, %q0" : "=r"(r) : "r"(v));
    return r;
}

/* The SYMBOLIC selector through a view — one grammar rule serves both selector
 * forms, because the width-view shape takes the SAME `asmTemplateSelector` the
 * plain and `asm goto` label forms take. */
static long long dssNamedNarrowView(long long v) {
    long long r;
    __asm__ ("movl %k[in], %k[out]" : [out] "=r"(r) : [in] "r"(v));
    return r;
}

#elif defined(__aarch64__)

/* The 32-bit view: `%w` here. aarch64 `mov` writes NO width suffix, so the
 * register spelling is the only thing that says which width was meant — which
 * is exactly why a template on this CPU cannot express a 32-bit operation
 * without the modifier. */
static long long dssNarrowView(long long v) {
    long long r;
    __asm__ ("mov %w0, %w1" : "=r"(r) : "r"(v));
    return r;
}

static long long dssWideView(long long v) {
    long long r;
    __asm__ ("mov %x0, %x1" : "=r"(r) : "r"(v));
    return r;
}

static long long dssNamedNarrowView(long long v) {
    long long r;
    __asm__ ("mov %w[out], %w[in]" : [out] "=r"(r) : [in] "r"(v));
    return r;
}

#else
#error "c_inline_asm_width_view_modifier: no arm for this architecture — add \
one rather than letting the example pass without exercising a width view"
#endif

int main(void) {
    long long narrow;
    long long wide;
    long long named;
    long long inline_narrow;

    /* 1 — THE SUBJECT. A 32-bit view of a 64-bit operand: the low half survives
     * and bits 63:32 are zeroed by the CPU, on both targets. A build that
     * ignored the letter would move all 64 bits and leave 0x1122334455667788
     * here. */
    narrow = dssNarrowView(dss_seed);
    if (narrow != 0x0000000055667788LL) return 1;

    /* 2 — the 64-bit view of the same operand, through the OTHER declared
     * letter. A build that treated every modifier as "narrow" fails here. */
    wide = dssWideView(dss_seed);
    if (wide != 0x1122334455667788LL) return 2;

    /* 3 — the symbolic selector through a view. */
    named = dssNamedNarrowView(dss_seed);
    if (named != 0x0000000055667788LL) return 3;

    /* 4 — the two views read the SAME operand, so they must agree below bit 31
     * and differ above it. Stated as two separate reads of the high half so a
     * failure says WHICH view was wrong. */
    if ((wide >> 32) != 0x11223344LL) return 4;
    if ((narrow >> 32) != 0LL) return 5;

    /* 6 — ⚠ THE NEGATIVE CONTROL, AND THE REASON EVERY CHECK ABOVE IS WIDE.
     * Through `(int)` the two views are INDISTINGUISHABLE. This is the exact
     * observation under which a wrong-width store ships green, so the example
     * asserts that it is blind rather than leaving the reader to assume the
     * wide checks were merely thorough. */
    if ((int)narrow != (int)wide) return 6;

    /* 7 — the same form written INLINE rather than behind a call, so the
     * statement is exercised where the operand is a local the allocator placed
     * itself rather than an incoming parameter. */
    inline_narrow = 0;
#if defined(__x86_64__)
    __asm__ ("movl %k1, %k0" : "=r"(inline_narrow) : "r"(dss_seed));
#else
    __asm__ ("mov %w0, %w1" : "=r"(inline_narrow) : "r"(dss_seed));
#endif
    if (inline_narrow != 0x0000000055667788LL) return 7;

    return 42;
}
