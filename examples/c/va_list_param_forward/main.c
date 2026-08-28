/* c63 (D-CSUBSET-VA-LIST-PARAM-SLOT, error[H0009]): a function that takes a `va_list ap`
 * PARAMETER and consumes it with va_arg (the vprintf / sqlite3_str_vappendf shape) needs
 * `ap` to be slot-backed, because va_arg READS AND ADVANCES the list in place (an lvalue
 * use). A va_list PARAM was received as a pure-SSA Arg with no storage slot, so the lvalue
 * use failed: error[H0009] "symbol N has no storage slot (non-addressable param or unbound)
 * — required by lvalue use". (A LOCAL va_list built via va_start already gets a slot from
 * its VarDecl, which is why only the PARAM form failed.) SQLite hits it in
 * sqlite3_str_vappendf(..., va_list ap) at `pArgList = va_arg(ap, PrintfArguments*)`.
 *
 * The fix is per-strategy (config-driven, no arch branch): SysV (`__va_list_tag[1]`, an
 * array → the caller passes a POINTER to its own tag, C 6.7.6.3p7) registers that incoming
 * pointer as ap's address so va_arg advances the CALLER's tag (forwarding); Win64/Apple
 * (`char*`) slot-back the incoming pointer (driven address-taken by the va_arg usage);
 * AAPCS64 (`__va_list` struct) already rode the by-value-struct reception.
 *
 * RED-ON-DISABLE: revert the fix -> vsum's `va_arg(ap, int)` -> error[H0009] (does not
 * compile).
 *
 * VALUE-CORRECT + SENSITIVE across targets (each has a different va_list ABI): `sumv`
 * va_start's its list then FORWARDS it to `vsum` (the va_list PARAM); vsum walks it. A
 * broken forward (callee reading a stale/garbage tag, or not advancing the caller's tag)
 * would not yield 42. 10+11+12+9 = 42. */
#include <stdarg.h>

/* The va_list-PARAMETER callee (the sqlite3_str_vappendf shape). */
int vsum(int n, va_list ap) {
    int total = 0;
    int i;
    for (i = 0; i < n; i = i + 1) {
        total = total + va_arg(ap, int);   /* lvalue use of ap: read + advance in place */
    }
    return total;
}

/* The variadic forwarder (the sqlite3_str_appendf shape): va_start, then forward. */
int sumv(int n, ...) {
    va_list ap;
    int r;
    va_start(ap, n);
    r = vsum(n, ap);                        /* FORWARD the va_list to the PARAM-taking callee */
    va_end(ap);
    return r;
}

int main(void) {
    return sumv(4, 10, 11, 12, 9);          /* 10 + 11 + 12 + 9 = 42 */
}
