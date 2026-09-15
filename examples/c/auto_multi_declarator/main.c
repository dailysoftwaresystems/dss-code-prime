// P66 (C23 6.7.10 + Annex J.5.12 + footnote 164): ONE initializer-inferred
// declaration, MANY declarators, ONE deduced type — end to end, with a RUNTIME
// consequence for every claim rather than "it compiles".
//
// DSS used to REFUSE this whole file — `error[S_AutoRequiresSingleDeclarator]:
// … 2 declarators — exactly one is required` — on programs clang 18.1.3
// (`-std=c23`) compiles and RUNS. C23 carries no such constraint: type
// inference is §6.7.10 and constrains only the presence of `auto`; the
// declarator COUNT is Annex J.2(78) UNDEFINED BEHAVIOUR (so refusing and
// accepting are both conforming); Annex J.5.12 names the multi-declarator form
// a sanctioned COMMON EXTENSION; and footnote 164 tells an implementation that
// accepts a wider form to follow ISO/IEC 14882 — ONE type for the whole
// declaration, shared by every declarator.
//
// ★ THE REFERENCE ORACLE FOR THIS FILE IS clang AND IT WAS RUN, NOT ASSUMED.
// ✔MEASURED 2026-09-09: `clang -std=c23 -Wall -Wextra -pedantic` compiles this
// shape SILENTLY and the linked binary reports both objects `int` via
// `_Generic`, `sizeof` 4 for each, binds `int *` to each, and returns the
// values written. gcc 13.3.0 refuses ("'auto' may only be used with a single
// declarator") and MSVC ABSTAINS (it reads `auto` as the C89 storage class over
// an implicit `int`, which is a different feature answering a different
// question). An example only DSS accepts is not a witness; this one has one.
//
// WHAT EACH LINE COSTS IF THE SHARED TYPE IS WRONG:
//   • file scope        `auto fa = 10, fb = 20;`   — 30
//   • block scope       `auto a = 1, b = 2;`       —  3
//   • WIDTH is shared   `auto w = 1LL, v = 2LL;`   — sizeof 8 each, so 16
//   • DECAY is shared   `auto p = arr, q = arr;`   — both int*, p[1]+q[2] = 4+9
//   • static storage    `auto s1 = 5, s2 = 6;` in a static run — 11
//   • three declarators `auto x = 1, y = 2, z = 3;` — 6
// argc-seeded so the release optimizer cannot fold the program away.
// exit = 30 + 3 + 16 + 13 + 11 + 6 - 37 - (argc - 1) = 42 at argc == 1.

auto fa = 10, fb = 20;          /* file scope: both int, 10 and 20            */

static int statics(void) {
    static auto s1 = 5, s2 = 6; /* static storage duration, shared int        */
    return s1 + s2;             /* 11                                        */
}

int main(int argc, char **argv) {
    (void)argv;
    auto a = 1, b = 2;                      /* 3                              */
    auto w = 1LL, v = 2LL;                  /* long long: sizeof 8 each       */
    int arr[3];
    arr[0] = 2; arr[1] = 4; arr[2] = 9;
    auto p = arr, q = arr;                  /* both decay to int*             */
    auto x = 1, y = 2, z = 3;               /* three declarators, one type    */

    /* Both declarators must be POINTERS, not ints: indexing proves it. */
    int decayed = p[1] + q[2];              /* 4 + 9 = 13                     */

    /* Both must be 8 bytes wide. A per-declarator deduction that gave `v`
       something narrower changes this sum and the exit code. */
    int widths = (int)(sizeof w) + (int)(sizeof v);   /* 8 + 8 = 16           */

    /* Both must bind to `int *` — a compile-time claim with a runtime read. */
    int *pa = &a;
    int *pb = &b;
    int addressable = *pa + *pb;            /* 3                              */

    int total = fa + fb                     /* 30                             */
              + addressable                 /*  3                             */
              + widths                      /* 16                             */
              + decayed                     /* 13                             */
              + statics()                   /* 11                             */
              + x + y + z;                  /*  6                             */
    return total - 37 - (argc - 1);         /* 79 - 37 = 42                   */
}
