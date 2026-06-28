// D-CSUBSET-LABEL-BUDGET-CLIFF corpus (p19 Cluster G c31): a goto-label
// IMMEDIATELY BEFORE a multi-case `switch` (`spin: switch(sel){…}`), with a
// `goto spin` that jumps back INTO that label — the exact shape sqlite3.c uses
// (`json_parse_restart:` before a ~900-case JSON switch). The `commitAfterPrefix`
// PEG cut lets `labelStmt`'s probe commit after its fixed leading prefix so the
// switch body parses NON-speculatively (off the 4096-token probe budget) AND
// `labelStmt` keeps its `Identifier Colon statement` shape, so the label stays a
// real LabelStmt whose labeled statement IS the switch — `goto spin` resolves to
// it and re-runs the switch.
//
//   sel = feed(1) (opaque -> the switch is not constant-folded away on baseline)
//   pass 1: switch(1) -> case 1: acc += 20 (=20), sel = 0; hops=1; sel==0 && hops<3 -> sel=2, goto spin
//   pass 2: switch(2) -> case 2: acc += 17 (=37), sel = 5; hops=2; sel==5 && hops<3 -> sel=3, goto spin
//   pass 3: switch(3) -> case 3: acc +=  5 (=42), sel = 9; hops=3; (hops<3 false) -> return 42
//
// Fold-resistant: the discriminant comes from feed() and changes every pass, so
// the loop survives the release optimizer (it cannot be const-propagated away).
//
// This corpus is the RUNTIME witness: a goto INTO a label that precedes a switch
// resolves + re-runs the switch to exit 42 (baseline AND release, 4 targets). It
// is intentionally small, so the budget-cliff PARSE red-on-disable lives in the
// HIR pin LabelBeforeOversizeStatementParsesPastProbeBudget (>4096 tokens), which
// fails error[P0001] "got ':'" when `commitAfterPrefix` / the parser cut is removed.
int feed(int x) { return x; }

int main() {
    int sel = feed(1);
    int acc = 0;
    int hops = 0;
spin:
    switch (sel) {
        case 0: { acc = acc + 3;  sel = 8; } break;
        case 1: { acc = acc + 20; sel = 0; } break;
        case 2: { acc = acc + 17; sel = 5; } break;
        case 3: { acc = acc + 5;  sel = 9; } break;
        case 4: { acc = acc + 11; sel = 6; } break;
        default: { acc = acc + 100; sel = 1; } break;
    }
    hops = hops + 1;
    if ((sel == 0 || sel == 5) && hops < 3) {
        sel = (sel == 0) ? 2 : 3;
        goto spin;
    }
    return acc;
}
