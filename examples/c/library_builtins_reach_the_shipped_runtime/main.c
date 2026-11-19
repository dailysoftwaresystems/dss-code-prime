/* gcc's LIBRARY builtins ARE the library functions: `__builtin_printf` is printf, called here
 * with no header and no declaration of any of them. On pe and Mach-O the printf family's body
 * is DSS's own platform runtime (runtime/platform/src/stdio.c, which owns C23's %b and %B
 * there), so each bracket holds binary digits only if the builtin reached that runtime — a
 * refusal of the builtin, or a binding to the platform's own printf (a literal `b`), fails. */
int main(void) {
    char a[32], b[32];
    int n = __builtin_snprintf(a, sizeof a, "[%b]", 5u);
    int m = __builtin_sprintf(b, "[%#B]", 3u);
    int k = __builtin_printf("%s %d %s %d\n", a, n, b, m);
    return k == 17 ? 42 : 1;
}
