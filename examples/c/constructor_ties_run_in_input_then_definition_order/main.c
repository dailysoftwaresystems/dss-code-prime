// CONSTRUCTORS OF ONE PRIORITY RUN IN THE UNITS' INPUT ORDER, THEN EACH UNIT'S
// DEFINITION ORDER -- one rule for every format (the decision, its measurement
// and what it refuses are in `src/link/static_init_tables.hpp`).
//
// The link's input is main.c u1.c u2.c u3.c w.c: one constructor in each of u1,
// u2 and u3, and three in w, whose `wc` is PROTOTYPED before `wa` and `wb` are
// defined -- so the unit numbers `wc` first, and an order taken from that
// numbering runs it first. Each constructor notes its number; `main` prints
// what was noted and exits with how many. The twin
// `constructor_ties_run_in_input_then_definition_order_reversed` links the same
// units in the opposite order.
//
// `note` makes no library call: it runs before `main`, from the entry DSS
// synthesizes.

#include <stdio.h>

static char order[64];
static int  used;
static int  notes;

void note(int k) {
    char digits[12];
    int  n = 0;
    if (used + 13 > (int)sizeof order) return;
    do {
        digits[n++] = (char)('0' + k % 10);
        k /= 10;
    } while (k > 0);
    while (n > 0) order[used++] = digits[--n];
    order[used++] = ' ';
    ++notes;
}

int main(void) {
    fputs(order, stdout);
    fflush(stdout);
    return notes;
}
