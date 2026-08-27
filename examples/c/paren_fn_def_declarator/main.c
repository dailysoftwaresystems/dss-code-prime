// D-CSUBSET-PARENTHESIZED-FUNCTION-DEFINITION-DECLARATOR-REFUSED witness.
//
// C 6.7.6p1 lets a declarator be wrapped in redundant parentheses:
//     direct-declarator := identifier | '(' declarator ')' | direct-declarator '(' … ')'
// so `int (add)(int a, int b) { … }` defines the FUNCTION `add` exactly as
// `int add(int a, int b) { … }` does. That spelling is not a curiosity — it is the
// standard glibc/musl idiom for defining a name that is ALSO a function-like macro,
// because `(name)` suppresses the macro at the definition site while leaving the
// macro live for ordinary calls. `scale` below is written that way on purpose.
//
// ✔MEASURED against gcc 13.3.0 AND clang 18.1.3 (`-std=c17 -Wall -Wextra`,
// compiled AND RUN, with a deliberately-broken negative control in the same probe
// run so a silent instrument failure could not read as agreement): every shape in
// this file compiles and runs on BOTH. DSS refused all of them, in THREE different
// ways from ONE cause — the declarator-shape walk looked only at the name's own
// direct declarator and could not step out through a parenthesis:
//   * the definition        -> S0018 "a function definition's declarator must be a
//                             function declarator";
//   * the PROTOTYPE         -> `int (scale)(int v);` never became a prototype at
//                             all, so it bound as an object and the definition then
//                             collided (S0002 + S0018 "function prototype
//                             declarations are not supported here");
//   * the definition's OWN
//     PARAMETERS            -> scoped as if they belonged to a function POINTER, so
//                             the body could not see them (S0001 on the parameter).
//
// Every derived form is exercised, not just the headline one: nested parentheses, a
// POINTER return, a prototype/definition pair, the macro idiom, and — the sharp one
// — a function RETURNING A FUNCTION POINTER whose name is parenthesized, where TWO
// function suffixes sit over ONE name. Only the INNER one declares `chooser`; the
// outer belongs to the returned type, and its parameter names must stay invisible to
// the body. ✔MEASURED: gcc and clang both REJECT a body that reads the outer
// parameter ("'w' undeclared" / "use of undeclared identifier 'w'") and both ACCEPT
// a body that reads the inner one — so the pair is a real distinction, not a
// preference. The unit tests own that refusing half; this file owns the accepting
// half plus the runtime proof.
//
// Value-divergent so any miscompile misses 42 rather than coincidentally hitting it:
//   pick(&lo,&hi) -> &hi = 11 ; (scale)(3) = 6 ; scale(3) = 300 (the MACRO) ;
//   chooser(1)(1) = helper(1) = 21 ; neg(-9) = 9 ; add(11,6) = 17
//   => 17 + 21 + 9 - 300/60 = 42
//
// RED-ON-DISABLE: remove the parenthesis-ascent arm from `innermostNameDerivation`
// (semantic_analyzer.cpp) — i.e. put the walk back to a single-level look at the
// name's own direct declarator — and this program no longer COMPILES (the runner
// reports a compile failure instead of exit 42).
//
// Front-end (semantic) feature, fully source/target/format-agnostic: the declarator
// vocabulary it walks is read from `c.lang.json`, and the emitted code is ordinary
// functions and calls. Runs on x86_64 (PE + ELF) and arm64 (ELF qemu, Mach-O macOS
// leg); the release arm re-witnesses it under the shipped optimizer.

#define scale(v) ((v) * 100)   // a function-like MACRO named `scale` …

int (scale)(int v);            // … and a real FUNCTION of the same name. `(scale)`
                               // is not a macro invocation (no `(` follows the
                               // name), so this declares the function — a PROTOTYPE
                               // reached through a redundant parenthesis.

static int helper(int y) { return y + 20; }

// (1) the headline shape: a parenthesized name, parameters USED in the body.
int (add)(int a, int b) { return a + b; }

// (2) NESTED redundant parentheses — the walk must step out more than once.
int ((neg))(int v) { return -v; }

// (3) a POINTER return with a parenthesized name (`*` binds to the RESULT, not to
//     the name, so `pick` is still a function and not a function pointer).
int *(pick)(int *p, int *q) { return *p > *q ? p : q; }

// (4) TWO function suffixes over ONE name: `chooser` is a function taking `int k`
//     and returning `int (*)(int)`. The INNER `(int k)` declares it — `k` is a real
//     parameter, visible below. The OUTER `(int)` belongs to the RETURNED type.
int (*(chooser)(int k))(int) { return k ? helper : helper; }

// (5) the macro idiom's definition half — satisfies the prototype at the top.
int (scale)(int v) { return v * 2; }

int main(void) {
    int lo = 4, hi = 11;
    int m = *pick(&lo, &hi);   // 11  — parenthesized name, pointer return
    int s = (scale)(3);        // 6   — the FUNCTION (parens suppress the macro)
    int t = scale(3);          // 300 — the MACRO (untouched by the definition)
    int c = chooser(1)(1);     // 21  — helper(1), through the returned fn pointer
    int n = neg(-9);           // 9   — nested parentheses
    int a = add(m, s);         // 17  — 11 + 6

    int total = a + c + n - (t / 60);   // 17 + 21 + 9 - 5 = 42

    return (m == 11 && s == 6 && t == 300 && c == 21 && n == 9 && a == 17
            && total == 42)
               ? 42
               : 7;
}
