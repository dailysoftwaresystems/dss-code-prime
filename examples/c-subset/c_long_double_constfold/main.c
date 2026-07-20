/* LD-3 (D-CSUBSET-LONG-DOUBLE-CONSTFOLD-PRECISION): TARGET-PRECISION constant
 * folding for `long double` — the const-evaluator now folds F80 (x87 80-bit) /
 * F128 (IEEE binary128) arithmetic at TRUE 64/113-bit significand precision via
 * a minimal portable soft-float, instead of REFUSING the fold (which it did
 * because it carried values as a host `double`).
 *
 * ★ NO anti-fold trick — the POINT is that it FOLDS (contrast the sibling
 * `c_long_double`, which routes operands through MUTABLE GLOBALS precisely to
 * PREVENT folding and witness the runtime arithmetic). Here the fold is
 * LOAD-BEARING two ways:
 *   - `constexpr long double k = 20.0L + 22.0L;` REQUIRES a constant initializer:
 *     if the const-eval refused the F80/F128 fold, this is a COMPILE ERROR
 *     (S_ConstexprNonConstantInitializer). It compiles ONLY because LD-3 folds.
 *   - `const long double g_answer = 20.0L + 22.0L;` at file scope folds its
 *     initializer to the wide-precision 42.0L whose 16 rodata bytes are emitted
 *     via `appendWideFloatBits` (the folded-arm producer) on the walled axes.
 *
 * Axes: f64 (pe64 x86_64 MSVC + Apple arm64) collapse long double to binary64,
 * so the fold runs on the host `double` path; x87-80 (elf64 x86_64) folds via
 * the F80 kernel; ieee128 (elf64 aarch64) folds via the F128 kernel. On EVERY
 * axis `(int)k` const-folds to 42 (Float→Int at the operand's own precision) and
 * `(int)g_answer` reads the folded rodata global + converts at runtime — both 42.
 *
 * exit = (int)(20.0L + 22.0L) = 42.
 */

const long double g_answer = 20.0L + 22.0L;   /* folded to 42.0L in rodata */

/* LD-3 Finding-2 (body-literal promotion of a folded `WideFloatValue`): a
 * constexpr long double RETURNED BY VALUE materializes the folded arm into a
 * memory home (the body-literal rodata promotion, now WideFloatValue-arm-aware)
 * + the LD-4 return boundary. Before the promoter learned the WideFloatValue arm
 * this was a COMPILE ERROR (fell through to addConst → the mir_to_lir FPR wall);
 * a green build + a correct value proves the promotion fires and emits the right
 * 16 bytes (the same appendWideFloatBits path the file-scope global uses). */
static long double folded_body_value(void) {
    constexpr long double k = 20.0L + 22.0L;
    return k;
}

int main(void) {
    /* Compile-time fold — REQUIRED to be a constant (constexpr); a fold refusal
     * would be a compile error, so a green build proves the fold happened. */
    constexpr long double k = 20.0L + 22.0L;
    if ((int)k != 42) {
        return 1;
    }
    /* Finding-2: a folded long double crossing a return boundary must materialize
     * (not fail loud, not miscompile) — the debug arm exercises the real body-value
     * path (no whole-call fold), agreeing with the compile-time value. */
    if ((int)folded_body_value() != 42) {
        return 3;
    }
    /* Runtime read of the folded rodata global + long-double→int conversion:
     * must agree with the compile-time fold. */
    return (int)g_answer;   /* = 42 */
}
