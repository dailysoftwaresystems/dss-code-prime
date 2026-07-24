// TF-C68 (D-FFI-OS-UNIX-LFS-ISNAN-SYMBOLS): <math.h> `isnan`, shipped as a
// function-like MACRO `((x) != (x))` in math.json. On every real platform
// isnan is a macro (glibc `#define isnan(x) __builtin_isnan(x)`), so a portable
// C fallback -- NaN is the sole IEEE-754 value not equal to itself -- models it
// with no symbol/library binding, working on all object formats. sqlite's
// util.c `sqlite3IsNaN` is the real consumer. The NaN is produced at RUNTIME
// (a mutable-global 0.0/0.0) so no earlier pass folds it away before isnan runs.
#include <math.h>

double g_zero = 0.0;
double g_val  = 42.0;

int main(void) {
    double n = g_zero / g_zero;   // NaN, computed at runtime
    if (!isnan(n))    return 1;   // a NaN MUST be detected
    if ( isnan(g_val)) return 2;  // a finite value must NOT be
    return (int)g_val;            // 42
}
