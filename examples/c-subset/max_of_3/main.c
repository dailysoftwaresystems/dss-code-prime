// D-CSUBSET-WHILE-LOOP-SUBSTRATE (step 13.5 cycle 1, 2026-06-03):
// max-of-three via two sequential if/else statements. Exercises:
//   * The ICmp+CondBr fusion at MIR→LIR lowering (cmp lhs, rhs;
//     jcc-cond directly — no naive cmp-against-zero pre-step that
//     would read setcc's garbage upper-byte and invert the branch).
//   * Multiple sequential if/else regions in one function body
//     (each forms its own diamond CFG with phi at the join).
//   * Sequential block-rel32 patches with both forward (jcc/jmp
//     into a later if-body) and backward branches structurally
//     possible (none in max-of-three, but the patch resolver is
//     exercised across multiple branches per function).
//
// Inputs: a=7, b=13, c=5. Max = 13 (b).

int main() {
    int a;
    int b;
    int c;
    int m;
    a = 7;
    b = 13;
    c = 5;
    if (a > b) {
        m = a;
    } else {
        m = b;
    }
    if (c > m) {
        m = c;
    }
    return m;
}
