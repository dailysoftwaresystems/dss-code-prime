// The sibling TU: defines what main.c bare-declares. The LK11 merge's
// cross-CU winner definition — main.c's synthesized no-library extern
// collapses onto this symbol and the call becomes direct.
int addRemote(int base) {
    return base + 12;
}
