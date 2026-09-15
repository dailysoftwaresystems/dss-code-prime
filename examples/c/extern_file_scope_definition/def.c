// the FILE-SCOPE half of [[D-FF2-3]] (P65) — THE
// DEFINING translation unit. Every object here is declared `extern` AND
// initialized, which C 6.9.2p1 makes a DEFINITION: "a declaration of an
// identifier for an object that has file scope with an initializer is a
// definition". The `extern` is REDUNDANT, not contradictory.
//
// ✔MEASURED 2026-09-08, each reference probed SEPARATELY on its own
// translation unit: gcc 13.3.0 (-std=c2x) rc=0 warning "'x' initialized and
// declared 'extern'", clang 18.1.3 (-std=c23) rc=0 warning
// -Wextern-initializer, MSVC 19.51.36231 (/std:c17 AND /std:clatest) rc=0
// SILENTLY. `nm` on gcc's and clang's objects shows a DEFINED symbol.
//
// Before this cycle DSS refused every line below with
//   error[H_ExternHasInitializer]: extern declarations cannot carry an
//   initializer — storage lives in another translation unit
// a sentence the standard does not support at file scope.

extern int g_defined = 23;

extern const int g_const = 7;

extern int g_arr[3] = {1, 2, 2};

// ★ THE PER-DECLARATOR ARM, and it is what makes this example prove more than
// "it compiles". One declaration, two declarators, TWO DIFFERENT ANSWERS:
// `g_pair` carries an initializer and is DEFINED HERE; `g_other` does not and
// stays an ordinary extern DECLARATION, defined in main.c instead. If DSS gave
// a declaration-level answer it would either define `g_other` here too — a
// duplicate definition the linker refuses, so this example would not LINK — or
// refuse `g_pair`. ✔MEASURED: gcc and clang each emit `D g_pair` and NO symbol
// at all for `g_other`.
extern int g_pair = 4, g_other;
