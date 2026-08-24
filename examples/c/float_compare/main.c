// FC3.5 sweep-c2: the FIRST float-comparison runtime witness — every
// C-reachable predicate drives a BRANCH. The single-cc predicates
// take the fused fcmp+jcc path (x86 UCOMISD + ja/jae; arm64 FCMP +
// b.gt/b.ge — Olt/Ole arrive swap-canonicalized as Fogt/Foge); the
// equality pair exercises BOTH realization families: on x86 `==`/`!=`
// have no single NaN-correct condition, so they run the COMPOSED
// materialization (sete AND setnp / setne OR setp folded with the
// new AND/OR encodings) followed by the branch-on-Bool path; on
// arm64 they fuse as single CSET-free b.eq/b.ne. (A semantic note,
// honestly: a non-branch materialized SUM like `(a==b)+(a!=b)` is
// rejected S_ReturnTypeMismatch today — the semantic tier types
// Bool arithmetic by the left operand, a pre-existing Bool-ALU gap;
// the materialized setcc shapes are pinned at the LIR tier instead.)
//
// Fold-resistance: every comparison's operands arrive as FUNCTION
// PARAMETERS (runtime XMM/d-register values loaded from .rodata at
// the call sites in main) — an intraprocedural pass never sees a
// comparable constant pair, and MIR ConstFold is int-only today.
//
// The sum's observations:
//   lt(1.5,2.5)=1  lt(2.5,1.5)=0  gt(2.5,1.5)=1  ge(2.5,2.5)=1
//   le(1.5,2.5)=1  eq(2.5,2.5)=1  ne(1.5,2.5)=1  eq(1.5,2.5)=0
// total = 6 → exit 42 (the +100 tail separates "wrong count" from
// "happened to be 6"). HONEST REACH (audit-residue sweep c2,
// D-AUDIT-WITNESS-STRENGTHENING): the sum catches SOME misroutes,
// not all — a POLARITY INVERSION of a predicate observed with both
// outcomes here (lt, eq) flips both of its calls and is SUM-NEUTRAL,
// and an adjacent-predicate slip (e.g. lt→le) is invisible without
// an equal-operand probe on that slot. The EXHAUSTIVE per-predicate
// guarantee is the 56-cell truth-table pin (7 predicates × 4 operand
// outcomes × 2 targets vs an independent SDM/ARM-ARM flag simulator,
// tests/lir/test_fcmp_lowering.cpp); what THIS corpus witnesses
// end-to-end is the branch plumbing of both realization families on
// live hardware.
int lt(double a, double b) { if (a < b)  return 1; return 0; }
int gt(double a, double b) { if (a > b)  return 1; return 0; }
int ge(double a, double b) { if (a >= b) return 1; return 0; }
int le(double a, double b) { if (a <= b) return 1; return 0; }
int eq(double a, double b) { if (a == b) return 1; return 0; }
int ne(double a, double b) { if (a != b) return 1; return 0; }

int main() {
    int t = 0;
    t = t + lt(1.5, 2.5);
    t = t + lt(2.5, 1.5);
    t = t + gt(2.5, 1.5);
    t = t + ge(2.5, 2.5);
    t = t + le(1.5, 2.5);
    t = t + eq(2.5, 2.5);
    t = t + ne(1.5, 2.5);
    t = t + eq(1.5, 2.5);
    if (t == 6) return 42;
    return t + 100;
}
