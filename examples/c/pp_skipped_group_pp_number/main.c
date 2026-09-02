// [[D-PP-SKIPPED-CONDITIONAL-GROUP-VALIDATED-AS-A-PHASE-7-NUMBER]] — a SKIPPED
// conditional group's text is divided into preprocessing tokens and NOT
// OTHERWISE PROCESSED (C 6.10.1p6). The observable behaviour this example pins
// is the coarsest one there is: the program COMPILES AND RUNS AT ALL.
//
// ★ THIS IS THE REAL SHAPE, NOT A REDUCTION. Upstream sqlite's `src/printf.c`
// keeps a Tcl script inside `#if 0 … #endif` as documentation of how `fmtinfo[]`
// was ordered, and one of its lines contains `%2d`. `2d` is a perfectly valid
// PREPROCESSING NUMBER (C 6.4.8) that converts to no numeric literal — so it is
// an error in phase 7 and nothing at all in a group that is skipped. DSS ran the
// phase-7 grammar over the skipped text and refused the whole translation unit,
// which is why every build cell of the sqlite corpus was red on every host.
//
// ✔MEASURED, gcc 13.3.0 and clang 18.1.3 probed SEPARATELY: both accept every
// skipped form below, and both REFUSE the same spellings when they are live
// (`int x = 2d;` → "invalid suffix \"d\" on integer constant" / "invalid digit
// 'd' in decimal constant"). The retiming must therefore keep the live refusal —
// the unit pins assert both directions; this example asserts the accept.
//
// Runtime witness: prints "skipped" and exits 42. If any group below were
// processed, there is no artifact to run at all and the harness fails on the
// build rather than on the comparison.

extern int puts(const char* s);

#if 0
// The sqlite idiom, verbatim in shape: a script kept as documentation.
//   puts -nonewline [format %2d: $r]
int broken = 2d;
double e = 1e;
int hexexp = 0x1e+2;
#endif

#ifdef PP_SKIPPED_GROUP_NOT_DEFINED_ANYWHERE
2d
#endif

#if 1
#else
0x1e+2
#endif

// A dead OUTER group whose inner arm is live-looking: the skip must cover the
// nested group's text too, and the `#if 1` inside it must not reactivate it.
#if 0
#if 1
2d
#endif
#endif

// The live half. `0x1e` is the same spelling whose pp-number tail would swallow
// a following `+2` — here nothing follows, so the literal grammar accepts it and
// it must still mean 30. `12` keeps the sum honest: 30 + 12 == 42.
static int total(int a, int b) {
    return a + b;
}

int main(void) {
    puts("skipped");
    return total(0x1e, 12);
}
