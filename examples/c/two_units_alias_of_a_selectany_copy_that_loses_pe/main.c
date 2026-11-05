/* AN ALIAS OF A SELECT-ANY COPY THAT LOSES (PE): the link discards the losing copy WHOLE, and the alias with it.
   a.c is the first unit, so its copy of each select-any definition is the one the link keeps; b.c's copies lose.
   one_m   a.c's weak definition (5) beside b.c's alias of its losing copy of one_w. The alias goes with that copy,
           so the name is a.c's weak definition, for main and for a.c alike; b.c's own use of its alias reads what
           one_w is there now, the kept copy (7).
   both_m  an alias of both_w in each unit. a.c's copy is kept, so a.c's alias is the name's: every use reads 7.
   own_m   only b.c aliases own_w. Its copy is discarded, so no unit defines own_m; b.c's own use reads the kept
           copy of own_w (7). Every return before 42 names the first fact that failed. */
extern int one_w, one_m, both_w, both_m, own_w;
int one_read_in_a(void), one_read_in_b(void);
int both_read_in_a(void), both_read_in_b(void);
int own_read_in_a(void), own_read_in_b(void);

int main(void) {
    if (one_w != 7) return 1;            /* a.c's copy of one_w is the kept one */
    if (one_m != 5) return 2;            /* the name is a.c's weak definition: b.c's alias went with its copy */
    if (one_read_in_a() != 5) return 3;
    if (one_read_in_b() != 7) return 4;  /* in b.c the alias IS one_w, and one_w is the kept copy */
    if (both_w != 7 || both_m != 7) return 5;
    if (both_read_in_a() != 7) return 6;
    if (both_read_in_b() != 7) return 7;
    if (own_w != 7) return 8;
    if (own_read_in_a() != 7) return 9;
    if (own_read_in_b() != 7) return 10;
    return 42;
}
