// C23 6.7.6 (P58, [[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]], the PARAMETER half):
// an alignment specifier on a FUNCTION PARAMETER must be accepted AND honored — the
// parameter's storage must land on the requested boundary at RUNTIME.
//
// ★★ WHY THIS IS THE RIGHT ANSWER RATHER THAN A DIAGNOSTIC. The row that opened this
// asked for `S_AlignasInvalidContext` here, reading C11 6.7.5p2. Measuring the
// references inverted it. ✔MEASURED 2026-09-03, each probed SEPARATELY:
//   * MSVC 19.51 ACCEPTS `_Alignas(16)` on a parameter at /std:c11, /std:c17 and
//     /std:clatest, rc 0, SILENT at /W4 — and HONORS it. Proven twice: the
//     compile-time pair `int p[__alignof(x)==16?1:-1]` compiles while its `==4` twin
//     fails C2118, and a LINKED, EXECUTED binary reports `((uintptr_t)&x)%16 == 0` at
//     both /Od and /O2.
//   * clang 18.1.3 refuses the ISO spelling but ACCEPTS AND HONORS the GNU one,
//     `void f(__attribute__((aligned(16))) int x)` → `__alignof__(x) == 16`.
//   * gcc 13.3.0 refuses both spellings ("alignment specified for parameter 'x'").
// The references split on ACCEPT-vs-REFUSE, which the disjunction governs, so one
// working accepting reference makes acceptance REQUIRED. C23 also softened the text:
// the alignment specifier is C23 §6.7.6 (not §6.7.5, which is Function specifiers in
// C23), and 6.7.6p2 DROPPED the word `parameter` that C11 6.7.5p2 named explicitly.
//
// ★ THE REFERENCE ORACLE FOR **THIS FILE** IS MSVC, AND IT WAS RUN, NOT ASSUMED —
// AND IT REFUTED THE FIRST DRAFT OF THIS FILE, which is why the draft is not what
// shipped. That draft asserted two extra things in the exit code and MSVC failed both:
//   (a) a NEGATIVE control demanding that an UNDECORATED int parameter's address be
//       NOT 16-aligned. `cl /std:c17 /Od` aligned it anyway (exit 8). "Not aligned" is
//       not a guaranteed property of any ABI — a frame may satisfy it by accident — so
//       it cannot be an assertion. It is now a comment carrying the DSS measurement
//       instead: MEASURED through the shipped CLI at x86_64:pe64-x86_64-windows-exec,
//       an undecorated `int` parameter's address is NOT 16-aligned in debug OR release
//       (the probe returns 7, not 42), so under DSS the 42 below is discrimination and
//       not luck — but that is a fact about DSS's frame layout, not a portable demand.
//   (b) an arm demanding the request on a SECOND parameter. MSVC returned 9 for that
//       at /Od AND /O2, at 32 and at 16 alike. A focused positional probe says why:
//       with `only_p(_Alignas(16) int x)`, `first_p(_Alignas(16) int a, int b)` and
//       `second_p(int a, _Alignas(16) int b)` in one binary, MSVC's mask is 5 — it
//       delivers the alignment in parameter slot 0 and SILENTLY DROPS it in slot 1.
//       ★ THIS DOES NOT WEAKEN THE ACCEPTANCE VERDICT AND DSS DOES NOT COPY THE DROP.
//       MSVC still ACCEPTS every position (that is the disjunction's question), and
//       clang HONORS every position in the GNU spelling — ✔MEASURED, the same three
//       probes built with `__attribute__((aligned(16)))` give clang mask 7 at -O0 AND
//       -O2. "A quality split is not a meaning fork: match the one that WORKS." DSS
//       honors every position (✔MEASURED, a second parameter is 16-aligned in debug
//       and release), and it is pinned at the semantic tier, where no reference oracle
//       is needed, by the test named on the next line (unwrapped so a grep finds it):
//       SemanticAnalyzerC.AlignasOnASecondParameterDoesNotLeakToItsSiblings
//       It is NOT in this file's exit code, because this file's oracle cannot deliver
//       it and an example must run identically on its oracle.
// What ships is compiled by the oracle at `/std:c17 /Od`, `/std:c17 /O2` and
// `/std:clatest /O2` (with /D"alignas=_Alignas", the spelling MSVC implements) and
// EXITS 42 on all three. gcc and clang refuse the file, which is exactly why MSVC is
// the oracle here — an example only DSS accepts is not a witness, it is a second copy
// of the bug.
//
// RED-ON-DISABLE: remove `alignasSpec` from `paramDeclSpecifier`'s alt in
// c.lang.json and this file does not COMPILE at all (P_NoAlternativeMatched) — it
// stops exiting 42. If the specifier were admitted but its value dropped before the
// frame layout, `aligned_probe` returns 0 and the exit code is 7.

typedef unsigned long long uptr;

// A parameter carrying an explicit 16-byte alignment request. `&x` forces the
// parameter into a real frame slot rather than a register, which is the only way its
// alignment is observable at all.
static int aligned_probe(alignas(16) int x) {
    return ((uptr)(void *)&x % 16u) == 0u;
}

int main(void) {
    if (!aligned_probe(3)) return 7;   // the request was admitted and then dropped
    return 42;
}
