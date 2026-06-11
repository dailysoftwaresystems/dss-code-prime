// FC3.5 sweep-c1 — logical operators AS CONDITIONS + the conventional
// C for-loop (chips task_bd58aa3d + task_20b1224d).
//
// Two pre-existing lowering bugs, one witness:
//
//   * `if (a < 2 && a < 3)` tripped the MIR verifier
//     (I_StructCfMismatch: IfThen-count 1 != IfJoin-count 2) — the
//     LogicalAnd/Or short-circuit diamond minted an IfJoin with a
//     `Linear` rhs arm, leaving the join unpaired. The fix marks the
//     rhs arm IfThen (the one-armed-if shape IfStmt already uses for
//     `if`-without-`else`).
//
//   * `for (i = 9; i; i = i - 1)` failed "HIR expression kind ordinal
//     19 [AssignStmt] not yet supported" — ForStmt routed the UPDATE
//     clause through the expression dispatch while the assignment
//     update is statement-shaped. The fix routes statement-shaped
//     for-clauses through the statement path (compound assigns and
//     postfix ++/-- desugar to the same AssignStmt shape).
//
// Exit discipline: gate(19, 85) must walk every branch shape —
//   &&-true, &&-false, ||-short-circuit, mixed nesting — then the
//   loops accumulate the exit code. Wrong short-circuit semantics,
//   a mis-paired join phi, or a broken for-update each derail the
//   accumulator away from 42.
//
// Fold resistance: the seeds arrive as FUNCTION ARGS, so the baseline
// (unoptimized) arm keeps live runtime CondBr diamonds and a live
// loop; every literal fits the narrowest shipped immediate slot
// (arm64 movz imm16).

int gate(int a, int b) {
    int acc = 0;
    if (a < 20 && b > 80) {        // both true -> +10
        acc = acc + 10;
    }
    if (a > 100 && b > 0) {        // lhs false -> short-circuits, no add
        acc = acc + 100;
    }
    if (a > 100 || b > 80) {       // rhs rescues -> +10
        acc = acc + 10;
    }
    if (a < 0 || b < 0) {          // both false -> no add
        acc = acc + 100;
    }
    if (a > 10 && (b < 50 || b > 70)) {  // mixed nesting, true -> +4
        acc = acc + 4;
    }
    // Conventional countdown: i = 9..1 -> +9 iterations of +1.
    int i;
    for (i = 9; i; i = i - 1) {
        acc = acc + 1;
    }
    // Compound-assign update (desugars to the same AssignStmt shape).
    int j;
    for (j = 9; j > 0; j -= 1) {
        acc = acc + 1;
    }
    return acc;                    // 10 + 10 + 4 + 9 + 9 = 42
}

int main() {
    return gate(19, 85);
}
