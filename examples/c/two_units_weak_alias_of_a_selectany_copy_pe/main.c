/* A WEAK ALIAS OF A SELECT-ANY COPY (PE): the link discards a losing copy whole, but a weak alias of it is a weak
   definition of its OWN name and stays one. a.c is the second unit and b.c the third, so a.c's copy of each
   select-any definition is the one the link keeps; b.c's copies lose.
   yield_m  b.c's weak alias beside main's strong yield_m (1): the strong one wins, and every read of the name,
            b.c's own included, reads 1.
   hold_m   b.c's weak alias is the only definition of the name: it names the KEPT copy of hold_w, so main and b.c
            read 7.
   quiet_m  the same, and only b.c names it: b.c reads 7.
   pair_m   a weak alias of pair_w in each unit: both name the kept copy, ONE definition, and every read gives 7.
   first_m  a.c's weak definition (5) beside b.c's weak alias, and main does not name it: the first weak definition
            wins, and b.c reads 5 through the name.
   Every return before 42 names the first fact that failed. */
extern int yield_w, hold_w, hold_m, pair_w, pair_m;
int yield_m = 1;
int yield_read_w_in_a(void), yield_read_in_b(void);
int hold_read_w_in_a(void), hold_read_in_b(void);
int quiet_read_w_in_a(void), quiet_read_in_b(void);
int pair_read_in_a(void), pair_read_in_b(void);
int first_read_in_a(void), first_read_in_b(void);

int main(void) {
    if (yield_w != 7 || yield_read_w_in_a() != 7) return 1;  /* a.c's copy of yield_w is the kept one */
    if (yield_read_in_b() != 1) return 2;                    /* b.c's weak alias yields to main's strong yield_m */
    if (hold_w != 7 || hold_read_w_in_a() != 7) return 3;
    if (hold_m != 7) return 4;                               /* the weak alias names the kept copy of hold_w */
    if (hold_read_in_b() != 7) return 5;
    if (quiet_read_w_in_a() != 7) return 6;
    if (quiet_read_in_b() != 7) return 7;
    if (pair_w != 7 || pair_m != 7) return 8;                /* two weak aliases of one copy are one definition */
    if (pair_read_in_a() != 7) return 9;
    if (pair_read_in_b() != 7) return 10;
    if (first_read_in_a() != 5) return 11;                   /* a.c's weak first_m is the first weak definition */
    if (first_read_in_b() != 5) return 12;                   /* b.c reads its weak alias's NAME, which a.c holds */
    return 42;
}
