// D-CSUBSET-SWITCH-NESTED-CASE (c60, Design I-A) corpus: a `case`/`default` label
// nested INSIDE a preceding case's brace `{ }` block (braces forced by a local
// declaration) — the SQLite VDBE / json-parser idiom that DSS's flat-switch model
// could not lower (error[S0023] S_CaseLabelNotInSwitch). The fix flattens the switch
// body to one CFG: every case/default at ANY depth becomes a synthetic label that
// the `addSwitch` jump-table targets, so a nested case is a real jump target and
// fall-through is straight-line.
//
// Two exercised shapes, both runtime-value-sensitive:
//   (A) VDBE shared-tail: `case 1: { int t; t=...; goto shared; } case 2: ...;
//       shared: ...` — case 2 AND the `shared:` label live inside case 1's block.
//       The dispatch must jump INTO case 1's block for x==1 (landing past the decl),
//       run the goto to `shared` (which is also inside the block), and for x==2 jump
//       to case 2 (also inside the block) then FALL THROUGH to `shared`.
//   (B) a case inside a deeper inner block: `case 3: { if (...) { case 4: ... } }`.
//
// The exit value depends on landing at the correct nested label AND correct
// fall-through: a wrong landing or a dropped fall-through changes acc.
//
//   classify(1): case 1 -> acc += 10, goto shared -> shared: acc += 2  => 12
//   classify(2): case 2 -> acc += 30 -> FALL THROUGH to shared: acc += 2 => 32
//   classify(3): case 3 -> enter inner block, k>0 so reach `case 4:` body via
//                fall-through -> acc += 6 -> break                       =>  6
//   main: classify(2) + classify(3) + classify(1) - 8 = 32 + 6 + 12 - 8 = 42
//
// RED-ON-DISABLE: revert the c60 fix (restore the fail-loud CaseStmt branch in
// cst_to_hir.cpp's statement dispatch) and this fails to lower with
// error[S0023] S_CaseLabelNotInSwitch on the nested `case 2:` / `case 4:`.
int opaque(int x) { return x; }   // opaque so the switch is not folded away

int classify(int x) {
    int acc = 0;
    switch (opaque(x)) {
        case 1: {
            int t = 10;          // a local decl forces the brace block
            acc = acc + t;
            goto shared;         // jump to a label INSIDE this block
        }
        case 2:                  // a case nested inside case 1's block
            acc = acc + 30;
            /* fall through */
        shared:                  // a shared-tail label inside the block
            acc = acc + 2;
            break;
        case 3: {
            int k = opaque(1);
            if (k > 0) {
        case 4:                  // a case nested inside a deeper inner block
                acc = acc + 6;
            }
            break;
        }
        default:
            acc = acc + 100;
            break;
    }
    return acc;
}

int main(void) {
    return classify(2) + classify(3) + classify(1) - 8;
}
