// CONSTRUCTORS OF ONE PRIORITY IN ONE UNIT RUN IN THE ORDER THE UNIT DEFINES
// THEM (the decision, its measurement and what it refuses are in
// `src/link/static_init_tables.hpp`).
//
// `wc` is PROTOTYPED before `wa` and `wb` are defined, so the unit numbers it
// first: an order taken from that numbering runs 23 21 22 -- gcc 13.3 on ELF's
// order at an EXPLICIT equal priority, and no reference's at the default one.
// One unit, so nothing renumbers it at the link. Each
// constructor notes its number; `main` prints what was noted and exits with how
// many. `note` makes no library call: it runs before `main`, from the entry DSS
// synthesizes.

#include <stdio.h>

static char order[64];
static int  used;
static int  notes;

static void note(int k) {
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

/* PROTOTYPED FIRST, defined last. */
__attribute__((constructor)) static void wc(void);
__attribute__((constructor)) static void wa(void) { note(21); }
__attribute__((constructor)) static void wb(void) { note(22); }
__attribute__((constructor)) static void wc(void) { note(23); }

int main(void) {
    fputs(order, stdout);
    fflush(stdout);
    return notes;
}
