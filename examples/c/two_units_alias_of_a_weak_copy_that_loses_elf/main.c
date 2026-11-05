/* AN ALIAS OF A WEAK COPY THAT LOSES (ELF): the control of two_units_alias_of_a_selectany_copy_that_loses_pe.
   ELF keeps a losing weak copy's bytes, so an alias that copy carries stays in its name's election, on those bytes.
   a.c is the first unit, so its weak copy of each `_w` is the name's; b.c's copies lose and keep their bytes.
   one_m   a.c's weak definition (5) beside b.c's alias of its losing copy of one_w (9). The alias is a strong
           definition of one_m: it is the name's, for main, a.c and b.c alike, and it reads b.c's kept bytes (9).
   own_m   only b.c aliases own_w: b.c's own use reads b.c's kept copy (9), every other use of own_w a.c's (7).
   Every return before 42 names the first fact that failed. */
extern int one_w, one_m, own_w;
int one_read_in_a(void), one_read_in_b(void);
int own_read_in_a(void), own_read_in_b(void);

int main(void) {
    if (one_w != 7) return 1;            /* a.c's weak copy of one_w is the name's */
    if (one_m != 9) return 2;            /* b.c's strong alias outranks a.c's weak one_m */
    if (one_read_in_a() != 9) return 3;
    if (one_read_in_b() != 9) return 4;
    if (own_w != 7) return 5;
    if (own_read_in_a() != 7) return 6;
    if (own_read_in_b() != 9) return 7;  /* the alias names b.c's kept bytes, not the name own_w */
    return 42;
}
