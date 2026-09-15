// [[D-CSUBSET-AUTO-FILE-SCOPE]] (P65) — C23 6.7.9 initializer type inference at
// FILE scope. Before this cycle the top-level grammar had no head-less
// inference form at all, so every declaration below was
// `error[P_NoAlternativeMatched]`. ✔MEASURED 2026-09-08, probed SEPARATELY:
// gcc 13.3.0 (-std=c2x) and clang 18.1.3 (-std=c23) each ACCEPT and RUN all of
// them. MSVC 19.51.36231 ABSTAINS rather than voting — it accepts the SPELLING
// as the C89 storage class over an implicit `int` (it also accepts `auto g;`,
// and refuses `static auto g = 42;` with C2159), so it is answering a different
// question.
//
// ★★★ THE EXAMPLE IS NOT A PARSE TEST. Parsing `auto` at file scope is the easy
// half; what can go wrong in SILENCE is what the declaration then MEANS, and
// file scope adds two meanings block scope does not have — LINKAGE and the
// tentative-definition interaction. So every arm gates on a RUNTIME
// consequence:
//
//   *  `auto g_ext = 11;` in def.c must be a DEFINITION with EXTERNAL linkage:
//      it is read here through an ordinary `extern int g_ext;`, so an internal
//      or absent definition fails the LINK rather than the comparison.
//   *  `auto static s_int = 20;` must keep INTERNAL linkage and still hold its
//      value — gcc emits `d s_int`, lower case.
//   *  `auto const k_const = 7;` must bind the QUALIFIER, so it can be read but
//      the release pipeline must still produce 7 rather than folding a
//      differently-typed object. gcc emits `R k_const` (read-only).
//
//   *  `auto d_dbl = 2.0;` must infer DOUBLE, not int: the sum `d_dbl + d_dbl`
//      is taken in floating point and truncated to 4. An int inference would
//      still give 4 here, so the type is ALSO pinned exactly at the semantic
//      tier (`FileScopeDeclarationDefinedness.AutoFileScopeInfersTheInitializersType`)
//      — this arm exists to prove the double SURVIVES to codegen.
//   *  `auto p_str = "ok";` must DECAY to a pointer (C23 6.7.9): it is indexed,
//      which an array-typed inference would also allow, so the arm that matters
//      is that it compiles and reads 'o' back at runtime.
//
// exit = 11 + 20 + 7 + 4 + 0 = 42.
//
// ⚠ THE SPECIFIERS ARE WRITTEN **AFTER** `auto`, AND THAT IS THE FEATURE'S ONE
// RESTRICTION RATHER THAN a stylistic choice. `auto static` / `auto const` are
// legal C23 6.7.2p2 and gcc 13.3.0 and clang 18.1.3 accept them with exactly the
// linkage the specifier-led spellings get (✔MEASURED, same `nm` letters on both
// orders). DSS refuses the SPECIFIER-LED order at file scope because admitting
// it would give `static`/`const`/… a second top-level candidate and put
// `topLevelDecl` — which contains file-scope FUNCTION DEFINITIONS, an unbounded
// token class — inside a speculation probe; that shape was built and MEASURED
// breaking a 3000-element file-scope initializer and a 1200-statement `static`
// function with `error[P_SpeculationBudgetExhausted]`.
//
// RED-ON-DISABLE (REMOVE direction, CONFIG mutant — the mutant is the DOCUMENT,
// so NO object md5 is involved; what moves is c.lang.json's own md5, which must
// move and RETURN): delete "autoInferredTopLevelDecl" from the `alt` list of
// `topLevel` in src/dss-config/sources/c.lang.json. Every declaration below
// becomes a loud parse error and the example never links.

extern int g_ext;                 // defined by def.c's `auto g_ext = 11;`

auto static s_int = 20;           // INTERNAL linkage, inferred int
auto const k_const = 7;           // the qualifier binds through the prefix
auto d_dbl = 2.0;                 // inferred DOUBLE, not int
auto p_str = "ok";                // C23 6.7.9 array-to-pointer DECAY

int main(void) {
    int sum = g_ext + s_int + k_const;
    sum = sum + (int)(d_dbl + d_dbl);
    // The decay arm: `p_str` is a pointer, so indexing it reads 'o'. A wrong
    // inference here changes the exit code rather than merely the type.
    if (p_str[0] != 'o') { return 1; }
    if (p_str[1] != 'k') { return 2; }
    return sum;
}
