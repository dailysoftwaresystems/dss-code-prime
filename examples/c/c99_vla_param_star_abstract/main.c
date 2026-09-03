// D-CSUBSET-VLA-PARAM-STAR-ABSTRACT (C 6.7.6.2p4): the UNNAMED abstract `[*]`
// array-declarator suffix — `int f(int, int [*]);` — parses, resolves and RUNS on every
// leg, alongside the NAMED `int g(int n, int a[*]);` form that already worked.
//
// `[*]` is the prototype-form marker for "a variable-length bound the prototype does not
// name". C 6.7.6.2p4 permits it only in a function declarator that is not part of a
// definition, so both prototypes below are followed by definitions that spell the real
// bound. Semantically the suffix decays exactly like an absent-length `[]`, so the
// parameter is an `int *` in both functions and the call sites are ordinary.
//
// ★ WHAT ACTUALLY BLOCKED THE ABSTRACT FORM, because the anchor named the wrong rule.
// The row said `abstractDirectDeclarator`'s non-speculative suffix repeat was the
// blocker. ✔MEASURED: an unnamed abstract parameter never reaches that rule at all — the
// shipped `param` is `head + {optional declarator}`, so `int [*]` routes through
// `directDeclarator`, and its BASE alt (where a leading `[` lands when there is no name)
// simply did not list `arrayStarSuffix`. Its own `$comment` claimed it did. The NAMED
// form worked because a name puts the suffix in the REPEAT, which already listed it —
// which is exactly why `g` below is kept as the in-example control.
//
// ✔MEASURED 2026-09-03: gcc 13.3.0 and clang 18.1.3 both compile and RUN both forms at
// `-std=c17` and `-std=c2x` (each emits a bound-mismatch WARNING — `-Wvla-parameter` /
// `-Warray-parameter` — and neither refuses). MSVC ABSTAINS twice over: `C2059: syntax
// error: ']'` on the `[*]` itself, and `C2057` on every VLA bound anywhere.
//
// The arguments are FIXED arrays and main holds no VLA object, so main may CALL — this
// example is isolated from the non-leaf-VLA-frame deferral. Each `return k` is a strict
// in-program pin; only all-pass reaches 42.

int f(int, int [*]);              // ABSTRACT + UNNAMED — the shape this row is about
int g(int n, int a[*]);           // NAMED — the control that already worked
int h(int, int [*], int [*]);     // two abstract star-suffixed params in one prototype

int f(int n, int a[n]) { return a[n - 1]; }
int g(int n, int a[n]) { return a[0]; }
int h(int n, int a[n], int b[n]) { return a[0] + b[n - 1]; }

int main(void) {
    int v[3];
    int w[3];
    v[0] = 7; v[1] = 8; v[2] = 9;
    w[0] = 1; w[1] = 2; w[2] = 3;

    if (f(3, v) != 9) return 1;          // the abstract prototype's definition indexes n-1
    if (g(3, v) != 7) return 2;          // the named control
    if (h(3, v, w) != 7 + 3) return 3;   // two star params, distinct arguments

    // A shorter bound through the SAME prototypes: `[*]` names no bound, so nothing about
    // the declaration may have frozen one. A prototype that had captured 3 would read
    // past the end here, or return the wrong cell.
    if (f(2, v) != 8) return 4;
    if (h(2, v, w) != 7 + 2) return 5;

    return 42;
}
