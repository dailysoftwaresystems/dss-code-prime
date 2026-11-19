// CONSTRUCTORS OF ONE PRIORITY RUN IN THE UNITS' INPUT ORDER, THEN EACH UNIT'S
// DEFINITION ORDER -- the twin of
// `constructor_ties_run_in_input_then_definition_order`, the same units linked
// in the OPPOSITE order: main.c w.c u3.c u2.c u1.c. The units run in that order,
// and `w`'s three still run in the order `w` defines them (the decision, its
// measurement and what it refuses are in `src/link/static_init_tables.hpp`).
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
