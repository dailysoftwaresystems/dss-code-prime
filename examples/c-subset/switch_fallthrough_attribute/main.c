// FC17 (D-CSUBSET-ATTRIBUTE-STATEMENT, C23 6.8.1 / 6.7.13.5): the
// `[[fallthrough]];` attribute-declaration STATEMENT — pre-FC17 a parse error
// (P0009), now a first-class switch-body item that lowers to nothing.
//
// The exit arithmetic is correct ONLY if the marked fall-through actually
// happens: x==1 selects `case 1` (acc += 1), control FALLS THROUGH the
// `[[fallthrough]];` marker into `case 2` (acc += 10), then breaks.
//   acc = 1 + 10 = 11  — the runtime witness that the marker statement
// neither blocks the fall-through nor emits any code of its own. The GNU
// spelling (`__attribute__((fallthrough));`) rides the same grammar rule and
// is pinned at the unit tier.
//
// The release pipeline arm proves the Skip-lowered marker survives the full
// optimizer stack (an empty block is DCE-food, never a barrier).
int main(void) {
    int x = 1;
    int acc = 0;
    switch (x) {
        case 1:
            acc += 1;
            [[fallthrough]];
        case 2:
            acc += 10;
            break;
        default:
            acc = 99;
            break;
    }
    return acc;
}
