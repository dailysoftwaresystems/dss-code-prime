// D-CSUBSET-NODISCARD-INDIRECT-DISCARD-CONTEXT (P32 lane A) witness: a
// `warn_unused_result` result discarded through an INTERPOSED wrapper.
//
// The discard test was TWO-HOP-EXACT — parent(call)==`expression` and
// grandparent(call)==`exprStmt` — which is right for the dominant `f();` idiom
// and blind to every other way C discards a value. ✔MEASURED at the pre-change
// HEAD through the shipped CLI, one TU per shape with a bare `g();` positive
// control: `(g());`, `g(), calls;`, `for(g();;)` and `for(;;g())` ALL compiled
// with no diagnostic, while gcc 13.3.0 AND clang 19.1.7 warn on all four.
//
// ★★ WHY THIS FILE EXISTS AT ALL, GIVEN THE EFFECT IS A WARNING. A warning does
// not change an exit code, so this example cannot assert the diagnostic — that
// is `tests/analysis/semantic/test_nodiscard_discard_context.cpp`'s job, and it
// asserts the count in BOTH directions (four shapes that must warn, four that
// must stay silent). What this file owns is the OTHER half, and it is not
// nothing: the fix walks the CST parent chain during semantic analysis, and a
// walk that mis-steps could just as easily change which node gets typed as which
// node gets warned about. The program below RUNS every wrapped shape and gates
// its exit code on each call having actually happened, so a walk that broke
// evaluation — or a `for` clause that stopped running — fails the run rather
// than the build.
//
// ★ `(void)g();` IS DELIBERATELY ABSENT from this file and present in the unit
// test as a MUST-NOT-WARN pin. The references disagree there (gcc warns, clang
// does not) and the cast is the universal idiom for "I meant to discard this",
// so DSS follows clang — the same disjunction reasoning that made the keyword-
// attribute class take clang's superset elsewhere in this cycle.
//
// ★ VERIFIED against BOTH references, `-std=c2x -Wall -Wextra`: gcc 13.3.0 and
// clang 19.1.7 compile this file — with the four expected `-Wunused-result`
// warnings, which is the point — and both binaries exit 42.
//
// RED-ON-DISABLE: drop `discardTransparentRules` + `discardClauseHosts` from
// `semantics.nodiscard` in `src/dss-config/sources/c.lang.json` → the four
// wrapped discards go silent again; the unit test's four warn-pins go red. This
// FILE stays green under that mutation by construction (it asserts evaluation,
// not diagnostics), which is exactly why the two are a pair and neither is
// sufficient alone.
//
// Front-end feature (semantic attribute checking), target/format-agnostic:
// x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O macos leg).

static int calls = 0;

int g(void) __attribute__((warn_unused_result));
int g(void) { return ++calls; }

int main(void) {
    (g());                            // paren-wrapped discard   -> calls == 1
    g(), calls;                       // comma-operand discard   -> calls == 2
    for (g(); 0; ) { }                // `for` INIT clause       -> calls == 3
    // The STEP clause discards too, and the loop is terminated BY that call:
    // `calls` is 3 on entry, the step raises it to 4, and the condition then
    // fails. A step clause that discarded without advancing anything would hang
    // the example rather than fail it, which is a worse test than no test.
    for (; calls < 4; g()) { }        // `for` STEP clause       -> calls == 4
    int used = g();                   // NOT discarded           -> calls == 5
    return calls * 8 + (used > 0 ? 2 : 0);   // 5*8 + 2 = 42
}
