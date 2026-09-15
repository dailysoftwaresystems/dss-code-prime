// P53 (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS, the half that closed):
// `_Noreturn` beside `extern` in a declaration.
//
// ✔MEASURED at P53's base through the shipped CLI: `extern _Noreturn void
// die(int);` was `error[P_NoAlternativeMatched]: expected 'Identifier',
// 'VoidKeyword', … — got '_Noreturn'`. The keyword was simply absent from the
// specifier run `externSpecifiers` already carried `inline` and both
// thread-local spellings in — an omission, not a decision. gcc 13.3.0
// `-std=c2x`, clang 18.1.3 `-std=c23` and MSVC 19.51 `/std:clatest`, each
// probed SEPARATELY 2026-09-02, all accept it.
//
// ★★ THE PIN IS THAT THE SPECIFIER IS HONORED, NOT THAT THE LINE PARSES. It is
// the `noreturn_function` shape routed through the OTHER declaration rule:
// `compute` is a NON-void function whose only non-return path calls `die`, so
// without the noreturn attribute reaching `SymbolRecord.isNoreturn` the
// fall-through would let `compute` reach its closing `}` without returning and
// the build would fail H_VerifierFailure with NO binary. A parse-only fix —
// admit the token to the grammar and let the linkage scan swallow it — produces
// exactly that failure, which is why the example runs rather than compiles.
//
// ⓘ `externSpecifiers` is the `externDecl` row's `specifierPrefix` and is always
// its first child, so `specifierPrefixNamesNoreturn` reads this subtree; the
// row's `linkageSpecifierIgnoredKinds` entry keeps `linkageFrom` from reporting
// the same token as an unknown LINKAGE specifier. Ignored by one scan, read by
// the other — the topLevelDecl row's own arrangement.
//
// ⚠ THE MIRROR ORDER IS STILL REFUSED AND THAT IS DELIBERATE, NOT AN OVERSIGHT:
// `_Noreturn extern void die(int);` is valid C (6.7.1 makes the specifiers an
// unordered SET) and all three references accept it, but admitting a specifier
// BEFORE `extern` puts those tokens into FIRST(externDecl), which costs the
// predictive prune and cliffs every `inline` function definition longer than
// four statements on the 128-token speculative-probe budget — ✔MEASURED in P53
// by bisecting body size. That half stays OPEN under this row.
//
// RED-ON-DISABLE (REMOVE direction), ✔EXERCISED P53: delete `NoreturnKeyword`
// from the `externSpecifiers` run in `src/dss-config/sources/c.lang.json` and
// this example FAILS IN BOTH RUNNERS (back to the P_NoAlternativeMatched above)
// while `noreturn_function` — whose prototype has no `extern` — stays green:
// the control that says the mutant is targeted at the extern route.
#include <stdlib.h>

extern _Noreturn void die(int code);

void die(int code) {
    exit(code);
}

int compute(int x) {
    if (x > 100) {
        return x;
    }
    die(42);
}

int main(void) {
    return compute(0);
}
