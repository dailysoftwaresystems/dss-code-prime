// D-OPT-ALIAS-ARC-CORPUS-CAPSTONE-STRICT (cycle 10i, 2026-06-04):
// The integration-as-a-whole capstone for cycles 10a–10g's alias
// arc. One compiled c-subset artifact that drives the full
// source → JSON → HIR → MIR → CSE → asm → link → run chain and
// asserts BOTH correctness (binary == baseline) AND effectiveness
// (a Load disappears under Mem2Reg+CSE) on the strict-aliasing
// arm — i.e., the path through `mirMayAlias` Rule 6 (distinct
// non-char primitives → No alias under `strictAliasingOnDistinct
// Types: true`).
//
// Why the strict arm and not the char-exception arm: c-subset is
// C, so `charTypesAliasAll: true` is the semantically-correct
// language config. An example under examples/c-subset/ inherits
// that config; an `int*` / `char*` pair would trip Rule 5 (char-
// exception → Maybe) first, leaving no precision win observable
// on the char path. The strict arm is the under-pinned one — no
// compiled-binary differential existed for `strictAliasingOn
// DistinctTypes` before this row. Cycle 10h's `D-CSUBSET-LONG-
// PRIMITIVE` was the substrate pre-req: it added `long`→I64 to
// c-subset so a non-char distinct-primitive pair (`int*` vs
// `long*`) became expressible.
//
// Shape: `compute(int* pI, long* pL)` Loads through pI, Stores
// through pL, Loads through pI again, and returns the sum.
// Under c-subset's default config (`strictAliasingOnDistinctTypes:
// true`, `charTypesAliasAll: true`), `mirMayAlias(pI, pL)` ==
// No (Rule 6: Ptr<I32> vs Ptr<I64>, both non-char, strict-TBAA
// admit). CSE proves the second Load(pI) redundant after Mem2Reg
// promotes the local-int alloca to SSA, and rebuilds the function
// with one Load instead of two. The runtime semantics are
// identical: with i=5, l=0, `compute(&i, &l)` returns 5 + 5 = 10
// regardless of how many MIR Loads are emitted.
//
// Correctness gate (corpus runner): both baseline (no opt) and
// every optimised arm must exit 10. A wrong CSE (e.g., admitting
// the second Load against the first when pI and pL aliased in
// practice — not the case here but the discipline matters)
// would yield a different exit code.
//
// Effectiveness gate (paired MIR-tier test, tests/opt): the
// optimised arm's MIR has strictly fewer Load opcodes than the
// baseline's, AND `passMutationCount[Cse] >= 1`. Without that
// pin, the corpus row would pass even if CSE failed to fire
// (silent ineffectiveness).

int compute(int* pI, long* pL) {
    int a;
    a = *pI;
    *pL = 7;
    return a + *pI;
}

int main() {
    int i;
    long l;
    i = 5;
    l = 0;
    return compute(&i, &l);
}
