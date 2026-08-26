// CU #2 of the cross-CU jump-table example.
//
// This translation unit owns a DENSE SWITCH. A dense switch is lowered to a JUMP
// TABLE: a DATA item whose slots hold the addresses of basic blocks in `dispatch`,
// so its relocations name SYNTHETIC BLOCK SYMBOLS rather than functions or globals.
// That is the shape the cross-CU merge got wrong.
//
// D-LINK-MERGE-DOES-NOT-REMAP-BLOCK-SYMBOLS — the merge remapped this function's own
// symbol and every relocation target, but copied `blockSymbols` verbatim. The
// table's relocations were retargeted to freshly minted merged ids while the block
// symbols kept their per-CU ids, so nothing declared what the table pointed at and
// the link failed with K_SymbolUndefined.
//
// ⚠ THE SWITCH IS DENSE AND WIDE ON PURPOSE. A sparse or tiny switch lowers to a
// compare chain, which carries no data item at all and would make this example green
// for the wrong reason -- it would stop testing the thing it is named after.
//
// ⚠ AND IT MUST BE A SEPARATE CU FROM `main.c`: in a single-CU build the merge's id
// mint is the identity, the two ids coincide, and the defect cannot be observed.
//
// ⓘ Written as a `switch` rather than as a GNU computed goto (`&&label` in a
// `static void* const tbl[]`) DELIBERATELY: DSS refuses that spelling today with
// error[H0009] "computed `goto *` in a function that takes no label address", so the
// GNU form could not carry this example. The switch reaches the SAME lowering
// through ISO C, which is the better subject anyway -- it is what real code does,
// and it is what sqlite's interpreter loops actually are.

int dispatch(int sel) {
    switch (sel) {
        case 0:  return 10;
        case 1:  return 20;
        case 2:  return 30;
        case 3:  return 40;
        case 4:  return 50;
        case 5:  return 60;
        case 6:  return 70;
        case 7:  return 80;
        case 8:  return 90;
        case 9:  return 100;
        case 10: return 110;
        case 11: return 120;
        default: return -1;
    }
}
