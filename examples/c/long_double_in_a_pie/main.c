/* `long double` in a position-independent executable
 * (D-CONFIG-LONG-DOUBLE-REFUSED-ON-PIE-SHARED-AND-DLL-FORMATS).
 *
 * ★★★ WHY THIS EXAMPLE EXISTS. The ELF `-pie` documents declared no
 * `longDoubleFormat`, so every `long double` in a PIE was refused
 * (S_LongDoubleFormatUndeclared) while the same file built as a fixed-address
 * executable compiled and ran — a property of the platform's ABI, not of the
 * image kind: gcc and clang build it `-pie` and it runs. ✔MEASURED 2026-09-22
 * at the P68 round-8 part-4 base: refused on both ELF `-pie` targets, debug
 * and release.
 *
 * What a PIE adds is RELOCATION: nothing here has a link-time address, so every
 * pointer into `long double` data — the constant table's cursors, the global's
 * address, the function pointer — is written by the dynamic loader at run time.
 *
 * Exit codes:
 *   11  `sizeof(long double)` is not `__SIZEOF_LONG_DOUBLE__`
 *   12  a `long double` member after a `char` is not aligned for its type
 *   13  the relocated arithmetic did not reach 42
 *   42  every check passed
 *
 * ★ The `release` arm: the index and the function pointer are `volatile`, so
 * the optimized pipeline still has to follow the relocated pointers. */

static long double base = 40.0L;
long double step = 1.5L;
static long double const quarter[] = {0.25L, 0.25L};
static long double const *const cursor[] = {&quarter[0], &quarter[1]};
long double *volatile step_at = &step;
long double (*volatile op)(long double, long double);
static volatile int zero = 0;

struct tagged {
    char        tag;
    long double value;
};
static struct tagged result = {'r', 0.0L};

static long double add(long double x, long double y) { return x + y; }

int main(void) {
    long double s;
    op = add;
    s = op(base, *step_at);            /* 41.5  */
    s = op(s, *cursor[zero]);          /* 41.75 */
    s = op(s, *cursor[1 + zero]);      /* 42    */
    result.value = s;
    if (sizeof(long double) != __SIZEOF_LONG_DOUBLE__) return 11;
    if ((unsigned long)&result.value % _Alignof(long double) != 0) return 12;
    if (result.value != 42.0L) return 13;
    return (int)result.value;
}
