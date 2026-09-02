// D-CSUBSET-LINKAGE-INHERITED-INTERNAL-EMITS-GLOBAL witness (C 6.2.2p4/p5).
//
// THE EXAMPLE IS TWO TUs BECAUSE THE DEFECT ONLY EXISTS IN TWO TUs. In ONE
// translation unit `static int f(void); int f(void) { … }` already compiled and
// ran correctly, and a single-TU image carries no symbol table that could
// contradict the binding — so a one-file pin could not have failed no matter how
// wrong the emission was. left.c and right.c each use the C 6.2.2p4/p5
// INHERITANCE shape for BOTH a function and an object: a plain definition whose
// linkage comes from a VISIBLE PRIOR `static` declaration rather than from any
// token of its own. Each TU's copy is therefore a DISTINCT object, and the
// program links only if DSS agrees.
//
// VALUE-DIVERGENT, so a silent re-merge cannot pass as green: leftTotal() is
// 20 + 1 = 21 and rightTotal() is 30 + 4 = 34, for 55. Had both TUs bound to
// left.c's `tuValue`/`tuSlot` the answer would be 21 + 21 = 42; the pre-fix
// compiler did not even get that far — it refused the program outright with
// K_SymbolRedefinedAcrossUnits ("symbol 'tuValue' has multiple strong (global)
// definitions across compilation units"), which is a legal C program rejected.
//
// REFERENCES, each probed SEPARATELY on this two-file shape (✔MEASURED
// 2026-09-01): gcc 13.3.0, clang 18.1.3 and MSVC 19.51 all BUILD AND RUN the
// FUNCTION half to the same answer — MSVC through a real two-TU link, not only
// a front-end probe. The OBJECT half (`static int g; int g = 4;`) splits — gcc and
// clang reject it ("non-static declaration of 'g' follows static declaration",
// reading 6.2.2p5 as giving a file-scope object with no storage class EXTERNAL
// linkage), while MSVC accepts it and classes the symbol `Static` in its own
// .obj. One accepting reference makes the behaviour REQUIRED, and MSVC's
// dumpbin output states the meaning it must be given: internal.
//
// RED-ON-DISABLE: in `recordLinkage` (src/hir/lowering/cst_to_hir.cpp) drop the
// `rec->isInternalLinkage` arm so the binding comes from the definition's own
// specifier tokens again -> this program fails to compile with
// K_SymbolRedefinedAcrossUnits on `tuValue`.

int leftTotal(void);
int rightTotal(void);

int main(void) {
    return leftTotal() + rightTotal();   // 21 + 34 = 55
}
