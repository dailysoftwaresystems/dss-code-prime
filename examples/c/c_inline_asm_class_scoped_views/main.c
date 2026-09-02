/* CLASS-SCOPED WIDTH-VIEW MODIFIER LETTERS, END TO END AND BY EXECUTION —
 * the runnable half of D-ASM-AARCH64-FP-BARE-OPERAND-WIDTH-DIVERGES-FROM-REFERENCE
 * (R5 + R8 of the operator's design A′, P50).
 *
 * ★★★ WHAT THE aarch64 ARM PROVES: the class check is PER OPERAND, not per
 * template. One statement binds BOTH register files — GPR operands written
 * through their `%w`/`%x` views inside a REAL executed `mov`, and an FP-class
 * `"w"`-bound double live in the same template — and the letters resolve
 * against each operand's own class. Before P50 the five FP letters did not
 * lex at all and `%x0` on a `"w"` operand decoded silently at 64 bits; now
 * the letters are declared class-scoped in `asm-arm64-gas.lang.json`, a
 * wrong-class letter is refused by name (✔MEASURED, gcc 13.3.0 and clang
 * 18.1.3 separately: an FP letter on an `"r"` operand is a hard error under
 * BOTH; `%x0` on a `"w"` operand is rendered as a DIFFERENT register by each
 * — gcc `v0`, clang `d0` — and the operator ruled that construct dissolved),
 * and a bare FP reference derives `registerNatural` = 128 bits, matching the
 * `v0` both references render.
 *
 * ⚠ WHY NO FP LETTER APPEARS INSIDE AN EXECUTED INSTRUCTION HERE: the aarch64
 * gas DIALECT spells no floating-point mnemonic (✔MEASURED, 51 instructions,
 * none floating — the gap `c_inline_asm_fp_class_constraint` records, a
 * cross-referenced row's subject, not this one's). The FP letters' decode,
 * width statement and refusal directions are pinned at the asm tier in
 * `tests/asm/test_asm_class_scoped_modifiers.cpp`; what execution CAN witness
 * on this port today — and could not before R1 — is an FP-class operand
 * BOUND and ALLOCATED in the same template whose GPR operands run through
 * their views.
 *
 * ★★★ WHAT THE x86-64 ARM PROVES, AND IT IS THE OTHER HALF OF THE DESIGN: the
 * x86 letters are deliberately WIDTH-ONLY. ✔MEASURED, x86-64 gcc ACCEPTS a
 * GPR letter on an `"x"`-bound xmm operand and renders the bare `%xmm0`
 * (clang refuses; the disjunction decides acceptance), so `movsd %q1, %q0`
 * over `"x"` operands must keep compiling AND executing — class-scoping those
 * letters would have refused a program a reference accepts. This file runs
 * that exact construct and checks the copied value.
 *
 * ⚠ EVERY SEED IS `volatile` SO THE RELEASE ARM STILL REACHES THE TEMPLATES —
 * a folded constant would let the file pass without binding anything.
 *
 * ★ ASSERTIONS READ ABOVE BIT 31 (the `c_inline_asm_width_view_modifier`
 * lesson): the 32-bit view writes must zero bits 63:32, and a letter that
 * silently decoded at the wrong width changes exactly those bits.
 */

volatile unsigned long long dss_seed_hi = 0x1122334455667788ull;
volatile double             dss_seed_d  = 3.5;

#if defined(__aarch64__)

/* SHAPE A1 — both register files in ONE template: `mov %w0, %w1` runs the
 * 32-bit GPR view (bits 63:32 of the output must come back zero), while a
 * `"w"`-bound double is live in the same statement and must come back intact.
 * The FP operand is deliberately unreferenced by the template text: binding
 * and allocation are what execution can witness without an FP mnemonic. */
static unsigned long long dssMixedClasses(unsigned long long seed, double *d) {
    unsigned long long out;
    double fp = *d;
    __asm__("mov %w0, %w1" : "=r"(out) : "r"(seed), "w"(fp));
    *d = fp;
    return out;
}

/* SHAPE A2 — the 64-bit view on the same shape: `%x` must move the WHOLE
 * register, so the high half survives. A1 and A2 differing in exactly bits
 * 63:32 is the discrimination that a letter is being read at all. */
static unsigned long long dssWideView(unsigned long long seed, double *d) {
    unsigned long long out;
    double fp = *d;
    __asm__("mov %x0, %x1" : "=r"(out) : "r"(seed), "w"(fp));
    *d = fp;
    return out;
}

int main(void) {
    unsigned long long const seed = dss_seed_hi;   /* 0x1122334455667788 */
    double d1 = dss_seed_d;                        /* 3.5 */
    double d2 = dss_seed_d;

    if (dssMixedClasses(seed, &d1) != 0x0000000055667788ull) return 1;
    if (d1 != 3.5) return 2;
    if (dssWideView(seed, &d2) != 0x1122334455667788ull) return 3;
    if (d2 != 3.5) return 4;
    return 42;
}

#elif defined(__x86_64__)

/* SHAPE B1 — the measured gcc-accepted construct: a GPR view letter on an
 * `"x"`-bound xmm operand. gcc renders the bare `%xmm1`/`%xmm0`, so the
 * instruction is an ordinary `movsd` copy and the letter dies — which is
 * exactly what the width-only posture preserves. The copy is checked by
 * value. */
static double dssQLetterOnXmm(double a) {
    double b = 0.0;
    __asm__("movsd %q1, %q0" : "=x"(b) : "x"(a));
    return b;
}

/* SHAPE B2 — the control with no letter: the bare form must produce the same
 * copy, because on this operand the letter degrades to it. */
static double dssBareOnXmm(double a) {
    double b = 0.0;
    __asm__("movsd %1, %0" : "=x"(b) : "x"(a));
    return b;
}

int main(void) {
    double const a = dss_seed_d;                   /* 3.5 */
    if (dssQLetterOnXmm(a) != 3.5) return 1;
    if (dssBareOnXmm(a) != 3.5) return 2;
    (void)dss_seed_hi;
    return 42;
}

#else
#  error "c_inline_asm_class_scoped_views: no arm for this architecture — add \
one rather than letting the example pass without exercising a class-scoped \
view letter"
#endif
