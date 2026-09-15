// C23 6.7.6 (P58, [[D-CSUBSET-ALIGNAS-REGISTER-CONTEXT]]): an alignment specifier on
// an object declared `register` is ACCEPTED and the program RUNS. This example exists
// to keep a REFUTATION from being re-broken.
//
// ★★ THE ROW ASKED FOR A DIAGNOSTIC AND THE REFERENCES SAY NO. The row wanted
// `S_AlignasInvalidContext` for `register alignas(16) int x;`, reading C11 6.7.5p2
// (C23 renumbers it to 6.7.6p2), which does name `register` explicitly. But the bar
// here is `DSS = (gcc u clang u MSVC) u ISO C`, a DISJUNCTION, and ✔MEASURED
// 2026-09-03, each reference probed SEPARATELY:
//   * MSVC 19.51 ACCEPTS `register _Alignas(16) int x = 0;` at /std:c11, /std:c17 and
//     /std:clatest, rc 0, SILENT at /W4 — in BOTH specifier orders — and APPLIES the
//     alignment: `int p[__alignof(x)==16?1:-1]` compiles while its `==4` twin fails
//     C2118, against a control pair that discriminates.
//   * gcc 13.3.0 AND clang 18.1.3 both ACCEPT `register __attribute__((aligned(16)))
//     int x = 0;` clean at -Wall -Wextra — the GNU spelling of the same request on
//     the same object.
// So all three references accept an alignment request on a register object in a
// spelling they implement; only the ISO spelling is refused by gcc and clang. A split
// on ACCEPT-vs-REFUSE is settled by the disjunction (operator, 2026-08-28), so DSS
// must accept — which it already did, because `register` is an ignored storage kind in
// c.lang.json and the object gets an ordinary, honored frame slot. The row asked for a
// REGRESSION. This file is what turns that regression red.
//
// ⚠ WHAT THIS FILE DOES **NOT** WITNESS, STATED RATHER THAN IMPLIED: it does not check
// the alignment at run time, and no example can. Observing an object's alignment
// requires its ADDRESS, and `&x` on a register-qualified object is a constraint
// violation every reference enforces — ✔MEASURED, MSVC refuses it with C2103 and
// gcc/clang refuse it too (C 6.5.3.2p1). A file that took the address would be a
// program NO reference compiles, i.e. exactly the non-witness P57 paid for. The
// alignment IS pinned, one tier down and without needing an oracle, by
// `SemanticAnalyzerC.RegisterAlignasIsAcceptedAndStored`, which asserts the validated
// override reaches `SymbolRecord.explicitAlignment` as 16. This file pins the half
// that only a running program can: DSS still COMPILES it, and it still RUNS.
//
// ★ THE REFERENCE ORACLE IS MSVC AND IT WAS RUN: `cl /std:c17 /Od`, `cl /std:c17 /O2`
// and `cl /std:clatest /O2` (with /D"alignas=_Alignas") all build this file and the
// binary EXITS 42.
//
// RED-ON-DISABLE: add the `register` arm the row asked for to the alignas context
// ladder in semantic_analyzer.cpp and this file stops COMPILING
// (error[S_AlignasInvalidContext]) — it stops exiting 42.

// Both specifier ORDERS: C declaration specifiers are unordered, and both were probed
// on every reference.
static int before(void) {
    register alignas(16) int x = 20;
    return x;
}

static int after(void) {
    alignas(16) register int y = 22;
    return y;
}

int main(void) {
    return before() + after();   // 20 + 22 == 42
}
