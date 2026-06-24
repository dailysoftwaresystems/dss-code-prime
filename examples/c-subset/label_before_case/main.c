// D-CSUBSET-LABEL-BEFORE-CASE corpus: a goto-label IMMEDIATELY BEFORE a `case`
// (`hop: case 0:`) — legal ISO C (6.8.1, a case is a labeled statement, so a
// label may precede it) that FC5 could not parse (P0001) until cycle 10. The
// label lowers to a real LabelStmt at the case-0 arm's entry, so `goto hop`
// jumps INTO the switch and lands exactly there.
//
//   sel = feed(1) (opaque -> the switch is not constant-folded away on baseline)
//   switch(1) -> case 1: acc += 20, sel = 0, goto hop
//   goto hop  -> `hop: case 0:`  -> acc += 5 -> goto out
//   return acc = 25
//
// RED-ON-DISABLE: remove `caseStmt` from the `statement` alt in c-subset.lang.json
// and this fails to parse (error[P0001] "expected EndStatement" at the ':').
int feed(int x) { return x; }

int main() {
    int sel = feed(1);
    int acc = 0;
    switch (sel) {
hop:    case 0:
            acc = acc + 5;
            goto out;
        case 1:
            acc = acc + 20;
            sel = 0;
            goto hop;
        default:
            return 88;
    }
out:
    return acc;
}
