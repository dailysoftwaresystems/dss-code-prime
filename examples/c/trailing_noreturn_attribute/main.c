// P56 (D-CSUBSET-TRAILING-NORETURN-NOT-HONORED): an `__attribute__((noreturn))`
// written anywhere but the LEADING specifier position was parsed, folded for
// linkage and for attribute semantics, and its noreturn MEANING was dropped.
//
// ✔RE-MEASURED 2026-09-03 at `6482a71b` through the shipped CLI, on programs
// shaped exactly like this one. The row named ONE dropped position; the tree had
// TWO, and both are exercised below:
//
//   void die(int) __attribute__((__noreturn__));          AFTER-DECLARATOR  rc=1
//   extern void die(int) __attribute__((__noreturn__));   ditto, extern     rc=1
//   void __attribute__((__noreturn__)) die(int);          the SLOT position rc=1
//   __attribute__((__noreturn__)) void die(int);          LEADING           rc=0
//   extern __attribute__((__noreturn__)) void die(int);   LEADING           rc=0
//
// The `rc=1` is `error[H_VerifierFailure]: non-void function may fall through
// without returning a value` — a SPURIOUS refusal of a correct program, with NO
// binary. That is why this example RUNS rather than merely compiles: an unhonored
// noreturn is not a missing optimisation here, it is a build that does not happen.
//
// ★★ THE THREE REFERENCES, EACH PROBED SEPARATELY 2026-09-03, by -Wreturn-type
// differential against an undecorated control:
//   • gcc 13.3.0 and clang 18.1.3 HONOR every GNU spelling above, at -std=c11,
//     c17 and c2x alike — silent where the control warns.
//   • MSVC 19.51 implements no `__attribute__` in any position — every spelling
//     above is a hard syntax refusal at /std:c11, /std:c17 AND /std:clatest
//     (`error C2061: syntax error: identifier '__attribute__'` for the trailing
//     forms, `error C2143` for the leading ones) — so it casts NO vote on the
//     GNU spelling. Two references that WORK make it REQUIRED.
//   • ✔AND THE PROGRAM ITSELF IS REFERENCE-CLEAN, which is what makes exit 42
//     the CORRECT answer rather than merely DSS's self-consistent one: gcc and
//     clang each compile THIS FILE with `-Wall -Wextra` at c11, c17 and c2x with
//     ZERO diagnostics, and the binary exits 42 under both.
//
// ★ GRANULARITY IS THE OTHER HALF, AND IT IS THE HALF WITH AN UNSAFE DIRECTION.
// A trailing run belongs to the ONE declarator it follows; a declaration-level
// spelling belongs to all of them. ✔MEASURED in gcc AND clang: for `void a(int)
// __attribute__((__noreturn__)), b(int);` a caller of `a` is silent and a caller
// of `b` still draws -Wreturn-type, while `void __attribute__((__noreturn__))
// a(int), b(int);` silences both. Dropping the flag can only cost a spurious
// diagnostic; LEAKING it onto a sibling would elide a return path that must be
// kept. `keep` below is that sibling, and its return value is observable.
//
// ⚠ WHAT THIS EXAMPLE DELIBERATELY DOES NOT CONTAIN: a trailing C23
// `void die(int) [[noreturn]];`. NOT ONE reference honors that — C23 6.7.13.1
// puts an attribute written after a declarator on the TYPE, and gcc says
// `'noreturn' attribute ignored`, clang `error: 'noreturn' attribute cannot be
// applied to types`, MSVC `C4649`. DSS must not honor it either, which is why
// `declarators.afterDeclaratorAttrRules` gives `attrSpec` the grain
// `declarator` and `stdAttr` the grain `declaratorUnlessTypeDerived` (P56 lane
// `at`; the `afterDeclaratorEntityAttrRules` subset key this comment used to
// name is retired -- a subset can only say entity-or-not, and the measured axis
// has three answers). The negative is pinned in the unit suite
// (`SemanticAnalyzerC.C23TrailingStdAttrNoreturnAppertainsToTheTypeNotTheEntity`)
// rather than here, because a corpus example can only witness a program that
// BUILDS.
//
// RED-ON-DISABLE (REMOVE direction): set `attrSpec`'s `appertainsTo` in
// `declarators.afterDeclaratorAttrRules` to `type` in
// `src/dss-config/sources/c.lang.json` and this example FAILS IN BOTH
// RUNNERS (back to H_VerifierFailure on `viaTrailing`), while
// `extern_noreturn_specifier` and `noreturn_function` — whose spellings are both
// in the LEADING position — stay green: the control that says the mutant is
// targeted at the non-leading positions and not at the noreturn sink itself.
#include <stdlib.h>

// AFTER-DECLARATOR position, on a multi-declarator declaration: `stop` is
// noreturn and `keep` — its sibling — is NOT.
void stop(int code) __attribute__((__noreturn__)), keep(int code);

// The declaration-level SLOT position (`declAttrRun`), the one the row does not
// mention. It reaches every declarator of its declaration.
void __attribute__((__noreturn__)) bail(int code);

// AFTER-DECLARATOR position on an `extern` declaration — the glibc/Tcl idiom,
// and the exact spelling the row was opened on.
extern void quit(int code) __attribute__((__noreturn__));

int accumulated = 0;

void stop(int code) {
    exit(code);
}

void keep(int code) {
    accumulated += code;
}

void bail(int code) {
    exit(code);
}

void quit(int code) {
    exit(code);
}

// Non-void; its only non-return path ends in an AFTER-DECLARATOR-noreturn call.
int viaTrailing(int x) {
    if (x > 100) {
        return x;
    }
    stop(42);
}

// Non-void; its only non-return path ends in a SLOT-position-noreturn call.
int viaSlot(int x) {
    if (x > 100) {
        return x;
    }
    bail(43);
}

// Non-void; its only non-return path ends in an extern trailing-noreturn call.
int viaExternTrailing(int x) {
    if (x > 100) {
        return x;
    }
    quit(44);
}

// `keep` is the SIBLING declarator: NOT noreturn, so this function must supply
// its own return, and the value it returns is observable at runtime.
int viaSibling(int x) {
    keep(x);
    return accumulated;
}

int main(void) {
    if (viaSibling(7) != 7) {
        return 1;
    }
    if (viaSlot(200) != 200) {
        return 2;
    }
    if (viaExternTrailing(300) != 300) {
        return 3;
    }
    return viaTrailing(0);
}
