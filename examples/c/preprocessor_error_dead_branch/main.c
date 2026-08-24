// D-CPP-ERROR-WARNING (C23 6.10.5 `#error` / 6.10.6 `#warning`) -- the
// AUTHORED-DIAGNOSTIC runtime witness. Proves, end to end through every target
// leg, the ONE property no unit test can reach: a Warning-severity PREPROCESSOR
// diagnostic lets the build PROCEED -- preprocess -> parse -> lower -> codegen
// -> LINK -> spawn -> exit code. The analyzer-tier tests stop at the
// DiagnosticReporter; this is the only pin that carries a live `#warning` all
// the way through the real driver and out the other side as a running binary.
//
//   * ★ P001F -- the LIVE `#warning` (the headline). `#if !defined(__GNUC__)`
//     is TRUE here (DSS predefines __STDC__/__STDC_VERSION__/... but NOT
//     __GNUC__), so the directive is REACHED and reported at Warning severity.
//     This is verbatim the `sys/cdefs.h:81` shape -- the Apple SDK header
//     nearly every macOS translation unit pulls in, which greets a non-gcc
//     compiler with exactly this advisory. Translation must CONTINUE: the
//     warning must not bump `errorCount()`, must not poison the link, and must
//     not cost the program its exit status. A green 42 here is the proof.
//   * P001E -- an `#error` in a NOT-TAKEN group is entirely SILENT (C 6.10p1:
//     reachability, not recognition). Three shapes ride along as secondary
//     coverage, one per conditional form: `#if 0`, `#ifdef` on a
//     never-defined macro, and a not-taken `#else`. Each is the SDK's standard
//     unsupported-configuration guard. `#error` is Error severity AND
//     unsuppressable, so any one of them firing is a hard compile failure --
//     no binary, no spawn, no exit code.
//
// The LIVE arm is the one taken: `#if __STDC__` (a PREDEFINED-macro guard, = 1)
// defines BASE = 40 and its `#else` -- never entered -- parks both the third
// `#error` and BASE = 0. `#ifndef DSS_NEVER_DEFINED` (true) defines MARGIN = 2.
// A wrong evaluation of either guard would take the dead arm instead (BASE = 0
// -> exit 2), and a regressed dead-branch gate turns any of the three `#error`s
// into a build that never produces a binary at all.
//
// Fold-resistance (mirrors the sibling dead-branch witness): BASE and MARGIN
// reach `add` as FUNCTION ARGUMENTS, so the baseline (unoptimized) arm keeps a
// live runtime add -- the result is not const-folded to a single immediate at
// exit. The optimizedPipelines `release` arm runs the SHIPPED pipeline over the
// same source, so the optimizer sees the post-preprocess program too.
//
//   live branches -> BASE = 40, MARGIN = 2; add(40, 2) = 42 -> exit 42

// ★ THE HEADLINE. `sys/cdefs.h:81` verbatim: a REACHED `#warning` whose
// operand is a string literal. It fires (warning[P001F]) and the build carries
// on. If this were Error severity, the compile would stop here and the example
// would never reach a binary.
#if !defined(__GNUC__)
#warning "Unsupported compiler detected"
#endif

// DEAD `#error` (1/3) -- the `#if 0` form. Must be silent.
#if 0
#error this configuration is not supported
#endif

// DEAD `#error` (2/3) -- the `#ifdef` form on a macro nothing ever defines.
// This is the SDK's cross-compile guard shape (`#ifdef _MSC_VER` on a non-MSVC
// build), where the guarded body exists only to abort the wrong target.
#ifdef DSS_NEVER_DEFINED
#error DSS_NEVER_DEFINED must never be set
#endif

// DEAD `#error` (3/3) -- the not-taken `#else` form. `__STDC__` is predefined
// to 1, so the `#else` group is never entered and neither its `#error` nor its
// BASE = 0 exists.
#if __STDC__
#define BASE 40
#else
#error a conforming C implementation must predefine __STDC__
#define BASE 0
#endif

#ifndef DSS_NEVER_DEFINED
#define MARGIN 2
#endif

int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(BASE, MARGIN);
}
