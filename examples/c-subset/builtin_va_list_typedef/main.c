/* D-CSUBSET-BUILTIN-VA-LIST-TYPE-NAME (sqlite S0006 "got __builtin_va_list" in
 * os_unix.c/mem1.c/test1.c, always via the Darwin SDK's <sys/_types/_va_list.h>
 * `typedef __builtin_va_list __darwin_va_list;`): `__builtin_va_list` is the
 * COMPILER-PROVIDED spelling of the va_list type — no shipped descriptor can supply
 * the name — so the semantic analyzer injects it beside `va_list` in the builtin
 * scope at every VaListStrategy branch, bound to the IDENTICAL per-calling-convention
 * TypeId (SysV `__va_list_tag[1]` 24B, Win64/Apple-arm64 `char*` 8B, AAPCS64
 * `__va_list` 32B).
 *
 * PROPERTY: the builtin alias and plain `va_list` share ONE TypeId per calling
 * convention. `vsum` receives the list as `my_va_list` (typedef'd from
 * `__builtin_va_list`) while `sumv` builds it as plain `va_list` — the CROSS-NAME
 * forward only compiles/runs because the c63 va_list-param forwarding fix
 * (D-CSUBSET-VA-LIST-PARAM-SLOT) is type-EXACT: the param rides the same c82
 * adjustment exclusion and the va_arg isVaList check by TypeId identity, never by
 * spelling.
 *
 * RED-ON-DISABLE: revert the three `__builtin_va_list` injections in
 * semantic_analyzer.cpp -> S0006/S_UnknownType on the typedef line below (does not
 * compile).
 *
 * VALUE-CORRECT + SENSITIVE across targets (each has a different va_list ABI): a
 * broken cross-name forward (stale/garbage list, or a copy that fails to advance the
 * caller's cursor) would not sum the caller's actual varargs. 10+11+12+9 = 42. */
#include <stdarg.h>

/* The Darwin-SDK shape: a user typedef reaching the type through the builtin name. */
typedef __builtin_va_list my_va_list;

/* The va_list-PARAMETER callee (the sqlite3_str_vappendf shape), alias-typed. */
int vsum(int n, my_va_list ap) {
    int total = 0;
    int i;
    for (i = 0; i < n; i = i + 1) {
        total = total + va_arg(ap, int);   /* lvalue use of ap: read + advance in place */
    }
    return total;
}

/* The variadic forwarder: va_start a plain `va_list`, then forward it CROSS-NAME. */
int sumv(int n, ...) {
    va_list ap;
    int r;
    va_start(ap, n);
    r = vsum(n, ap);                        /* one TypeId under both names -> exact forward */
    va_end(ap);
    return r;
}

int main(void) {
    return sumv(4, 10, 11, 12, 9);          /* 10 + 11 + 12 + 9 = 42 */
}
