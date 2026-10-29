/* A STATIC LIBRARY whose code addresses its own interior: a dense switch (a jump table — one 8-byte slot per case,
 * each naming a block inside the function) and a computed goto (block addresses taken by the code itself). The
 * program links it by --resolve-library, so each reference reaches the link through the object reader of the pair's
 * format (D-LINK-OBJECT-READERS-DROP-INTERIOR-SYMBOL-OFFSET): DSS's COFF and Mach-O writers name every such block
 * with an interior label, and those labels must bind to the function that holds them. */
int lib_switch(int k) {
    switch (k) {
        case 0: return 100;
        case 1: return 101;
        case 2: return 102;
        case 3: return 103;
        case 4: return 104;
        case 5: return 105;
        case 6: return 106;
        case 7: return 107;
        case 8: return 108;
        case 9: return 109;
        case 10: return 110;
        case 11: return 111;
        default: return -1;
    }
}

int lib_computed_goto(int k) {
    void *const targets[] = {&&first, &&second, &&third};
    goto *targets[k % 3];
first:
    return 7;
second:
    return 8;
third:
    return 9;
}
