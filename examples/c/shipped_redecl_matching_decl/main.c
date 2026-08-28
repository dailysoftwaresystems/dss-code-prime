// D-CSUBSET-SUPPRESSED-SHIPPED-ROW-SIGNATURE-UNCHECKED — THE ACCEPTING HALF.
//
// C23 7.1.4p2 entitles a program to declare a library function itself, and real
// code does it constantly. P44 made DSS compare such a declaration against the
// shipped descriptor row it suppresses (before, the row was suppressed on its NAME
// alone and `int puts(double);` compiled clean and then called the platform's real
// `puts` with a double in xmm0). A check that refuses the declarations below would
// be worse than no check at all, so this example is the runtime proof that the
// ordinary spellings still compile, still bind the platform's own realization, and
// still RUN.
//
// Every declaration here is one gcc, clang and mingw-w64 gcc all accept:
//   * `int puts(const char *);` — the standard signature, spelled with `const`
//     where the descriptor row spells a bare `ptr<char>`. The row makes NO
//     qualification claim (hir-text has no `const`), so C23 6.7.6.1p2 is not
//     enforced against it and this must stay ACCEPTED.
//   * `extern int printf(const char *, ...);` — the most-redeclared identifier in
//     C, and on pe it is a compiler-SYNTHESIZED shim rather than an import, so
//     this also exercises the CST->HIR shim gate through the same oracle.
//   * `int puts(const char s[]);` — C 6.7.6.3p7 adjusts an array parameter to a
//     pointer, so this IS the row's `ptr<char>` written another way. A structural
//     comparison that forgot the adjustment would refuse it.
//
// The exit code is the witness that the calls really reached the platform rather
// than merely type-checking: C 7.21.7.10 has `puts` return a NON-NEGATIVE value on
// success, and 7.21.6.3 has `printf` return the number of characters written — 11
// for the ten digits plus the newline. Exit 42 is reachable only if BOTH
// redeclared names bound the real implementations.
#include <stdio.h>

int        puts(const char *s);
extern int printf(const char *, ...);
int        puts(const char s[]);

int main(void) {
    int a = puts("abcd");
    int b = printf("%s\n", "0123456789");
    return (a >= 0 && b == 11) ? 42 : 1;
}
