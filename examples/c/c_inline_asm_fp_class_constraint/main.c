/* THE FLOATING-POINT REGISTER-CLASS CONSTRAINT, END TO END AND BY EXECUTION —
 * `"w"` on aarch64 and its `"x"` twin on x86-64.
 * D-TARGET-ARM64-W-CONSTRAINT-BINDS-A-CLASS-NO-C-VALUE-EVER-LIVES-IN
 * D-LIR-SUBREGISTER-AWARE-ALLOCATION-FOR-ALIASED-VIEWS
 *
 * ★★★ THE aarch64 HALF OF THIS FILE COULD NOT BE COMPILED AT ALL BEFORE R1 OF
 * THE OPERATOR'S DESIGN A′. ✔MEASURED at the CLI, debug AND release, on
 * `arm64:elf64-aarch64-linux-exec`, every `"w"`-bound floating operand:
 *
 *   error … input 0 (constraint "w") binds register class 'vr', but the value
 *   it carries lives in class 'fpr'
 *
 * about a program `aarch64-linux-gnu-gcc 13.3.0 -O2` compiles, allocating `d0`.
 * The cause was not a missing move verb: `arm64.target.json` declared the ONE
 * SIMD&FP register file TWICE — `fpr` = d0..d31 (8 bytes) and `vr` = v0..v31
 * (16 bytes), 32 rows each, zero `subOf` on either side, the same
 * `dwarfNumber`s and the same `hwEncoding`s. `"w"` bound one of the two and a C
 * `double` lived in the other. Declaring an `{fpr, vr}` move row would have
 * made the symptom disappear and cemented the defect: there is nothing to move
 * BETWEEN. The file is now declared once and `"w"` binds the class the value
 * is in, which is why gcc emits no move here either.
 *
 * ★★ THE x86-64 HALF IS THE CONTROL AND IT IS NOT DECORATION. `"x"` ⇒ `fpr`
 * has ALWAYS had the shape R1 gives aarch64 — one class whose members are the
 * full 16-byte `xmm` registers, holding 8-byte `double`s — and it has always
 * compiled. Running the identical shapes on both targets is what separates
 * "the aarch64 config was wrong" from "this pipeline cannot express the
 * construct".
 *
 * ⚠ EVERY SEED IS `volatile` SO THE RELEASE ARM STILL REACHES THE TEMPLATE.
 * A folded constant would let this file pass without ever binding an operand
 * to a floating-point register — the vacuous-green shape this project has
 * paid for before.
 *
 * ⚠ WHY THE TEMPLATES SAY `nop` AND NOT `fadd`. The bytes an FP instruction
 * would need are declared in `arm64.target.json`, but the aarch64 gas DIALECT
 * (`asm-arm64-gas.lang.json`) spells no FP mnemonic — ✔MEASURED, 51
 * instructions and not one of them floating. That is the aarch64 twin of the
 * gap `c_inline_asm_x86_sse_operands` records for x86-64, it is a DIALECT
 * vocabulary gap rather than anything about the constraint, and it is out of
 * this example's scope. What is in scope is that the operand BINDS, is
 * ALLOCATED a register of the right file, survives a call, and comes back with
 * its value intact — all four of which `nop` templates can witness and none of
 * which could be witnessed at all before.
 *
 * ⚠ AND WHY NO `%d0`-STYLE MODIFIER APPEARS — THE REASON CHANGED ON
 * 2026-09-01 AND THIS PARAGRAPH IS THE CORRECTION. It used to say the five FP
 * view letters (`%b %h %s %d %q`) were UNDECLARED on this target and that
 * writing `%d0` was refused at the parser. Both were true when written and
 * both are now FALSE: P50 declared all seven letters class-scoped in
 * `asm-arm64-gas.lang.json` and flipped the fpr row of `asmBareOperandWidths`
 * to `registerNatural`, closing
 * [[D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE]]. `%d0` is
 * accepted here today. This example keeps the BARE `%0` deliberately: its
 * subject is that a `"w"`-bound operand BINDS, allocates from the right file
 * and survives a call — the letters are `examples/c/c_inline_asm_class_scoped_views`'s
 * subject, and pinning them twice would make this example red for a reason
 * that is not its own.
 *
 * ★ SHAPE 4 IS THE ONE THAT NEEDED THE FREE LIST, not merely the binding.
 * Before R1 the `vr` free list was EMPTY BY CONSTRUCTION — the AAPCS64
 * convention named only the d-views, so `buildFreeLists` produced no VR
 * registers and every VR value spilled (`R_SpilledDueToPressure`, and at debug
 * `rewriteOneFunc: function N exhausted the per-class scratch pool`). Eight
 * simultaneously-live constrained operands is more than any scratch pool, so
 * shape 4 can only pass if the operands were genuinely ALLOCATED.
 */

volatile double dss_seed_d = 3.5;
volatile float  dss_seed_f = 1.25f;

#if defined(__aarch64__)
#  define FPCON "w"
#elif defined(__x86_64__)
#  define FPCON "x"
#else
#  error "c_inline_asm_fp_class_constraint: no arm for this architecture — add \
one rather than letting the example pass without exercising an FP-class \
constraint"
#endif

/* SHAPE 1 — an in-out `double` behind a call, so neither the value nor the
 * template can be folded into the caller. The operand must be moved into a
 * register of the floating-point file and back out again: exactly the
 * same-class copy the old aarch64 config turned into a cross-class refusal. */
static double dssFpRoundTripD(double a) {
    __asm__ ("nop" : "+" FPCON (a));
    return a;
}

/* SHAPE 2 — the same, one width down. `float` lives in the LOW 32 BITS of the
 * same physical register, so this passes only if the width rides the
 * instruction rather than the register declaration. */
static float dssFpRoundTripF(float a) {
    __asm__ ("nop" : "+" FPCON (a));
    return a;
}

/* SHAPE 3 — an FP-class operand LIVE ACROSS A CALL. The constrained value must
 * survive a callee that is free to clobber every caller-saved FP register, so
 * the allocator has to treat it as an ordinary allocatable value with a live
 * range, not as an untouchable physical scratch register. */
static double dssCallee(double x) { return x + 1.0; }

static double dssFpLiveAcrossCall(double a) {
    double kept = a;
    double other;
    __asm__ ("nop" : "+" FPCON (kept));
    other = dssCallee(kept);
    return kept + other;
}

int main(void) {
    double a = dss_seed_d;               /* 3.5  */
    float  f = dss_seed_f;               /* 1.25 */

    if (dssFpRoundTripD(a) != 3.5)   return 1;
    if (dssFpRoundTripF(f) != 1.25f) return 2;
    if (dssFpLiveAcrossCall(a) != 8.0) return 3;

    /* SHAPE 4 — EIGHT simultaneously-live FP-class operands. This is the one
     * that needs a NON-EMPTY free list rather than merely a correct binding:
     * eight live constrained values cannot all come out of a scratch pool. */
    {
        double v0 = a,       v1 = a + 1.0, v2 = a + 2.0, v3 = a + 3.0;
        double v4 = a + 4.0, v5 = a + 5.0, v6 = a + 6.0, v7 = a + 7.0;
        double total;
        __asm__ ("nop"
                 : "+" FPCON (v0), "+" FPCON (v1), "+" FPCON (v2),
                   "+" FPCON (v3), "+" FPCON (v4), "+" FPCON (v5),
                   "+" FPCON (v6), "+" FPCON (v7));
        total = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7;
        /* 8*3.5 + (0+1+2+3+4+5+6+7) = 28 + 28 = 56 */
        if (total != 56.0) return 4;
    }

    return 42;
}
