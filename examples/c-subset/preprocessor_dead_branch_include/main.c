// c17 (D-PP-CONDITIONAL-INCLUDE-ORDERING) -- the dead-branch elision runtime
// witness. Proves, end to end through every target leg, that the preprocessor
// FULLY skips a dead `#if` branch BEFORE the include splice + the global
// tokenize, closing BOTH symptoms of the anchor:
//
//   * P0016 -- a quote-`#include` of a NONEXISTENT header inside a dead branch
//     (`#if 0`, and `#ifdef SQLITE_OS_WIN` -- the real SQLite cross-compile
//     pattern, undefined when targeting linux/macos) is NOT resolved. If the
//     SynthBuilder still resolved it, the build would FAIL LOUD with a
//     missing-include error and never produce a binary -- so a green exit here
//     is the proof the dead-branch include was elided.
//   * P000E -- lexically ILLEGAL characters (`$ @ `` -- not in the C basic
//     source character set) inside the dead branch are suppressed. If the
//     tokenizer still reported them, the build would fail with P_IllegalChar.
//
// The LIVE arm is the one taken: `#if __STDC__` (a PREDEFINED-macro guard, = 1)
// defines BASE = 40 and `#ifndef SQLITE_OS_WIN` (true -- never defined) defines
// MARGIN = 2. The `#if __STDC__` guard is the c17 regression witness: the
// SynthBuilder pre-scan cannot see predefined macros, so it must NOT be the
// authority on this branch's liveness -- the real macro pass (which materializes
// __STDC__ = 1) is. A wrong elision (dead branch kept, or the predefined guard
// mis-evaluated to 0 -> BASE = 0 -> exit 2) would either leave `return 99;` live
// (exit 99), pull in a missing include (link/preprocess failure), or trip an
// illegal-char error.
//
// Fold-resistance (mirrors the FC14 conditional witness): BASE and MARGIN reach
// `add` as FUNCTION ARGUMENTS, so the baseline (unoptimized) arm keeps a live
// runtime add -- the result is not const-folded to a single immediate at exit.
// The optimizedPipelines `release` arm runs the shipped pipeline over the SAME
// source.
//
//   live branches -> BASE = 40, MARGIN = 2; add(40, 2) = 42 -> exit 42

#if 0
// DEAD branch: every line here must be elided. It contains lexically illegal
// characters AND a quote-#include of a header that does not exist. If
// conditional elision regressed at the SynthBuilder/tokenize stage, ANY of
// these would break the build (illegal-char error, missing-include error, or a
// live `return 99`).
$ @ `
#include "this_header_does_not_exist.h"
int dead_should_not_compile(void) { return 99; }
#endif

#if __STDC__
#define BASE 40
#else
#define BASE 0
#endif

// The SQLite cross-compile pattern: a Windows-only header guarded by
// SQLITE_OS_WIN, which is UNDEFINED on the linux/macos targets -> the include
// must be skipped (the header intentionally does not exist).
#ifdef SQLITE_OS_WIN
#include "os_win_does_not_exist.h"
#define MARGIN 99
#endif

#ifndef SQLITE_OS_WIN
#define MARGIN 2
#endif

int add(int a, int b) {
    return a + b;
}

int main(void) {
    return add(BASE, MARGIN);
}
