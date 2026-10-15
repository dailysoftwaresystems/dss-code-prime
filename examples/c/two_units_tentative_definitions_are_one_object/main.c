/* TWO UNITS, ONE NAME, NO INITIALIZER IN EITHER: ONE OBJECT.
   `int shared_counter;` here and in other.c are two TENTATIVE definitions of one object (C 6.9.2p2), and the link
   makes them one. `pair` is declared `int pair[1];` here and `int pair[10];` there: the one object is as large as
   the larger of them, so the other unit's `pair[1]` .. `pair[9]` are storage of the program and `pair[0]` is the
   same word in both units. `guard_main` is this unit's own zero-filled neighbour: other.c checks that no object
   either unit can name lies inside pair's ten words, and that none of them changed after it filled them. */
int shared_counter;
int pair[1];
static int guard_main[4];

int bump(void);                                            /* other.c: ++shared_counter */
int last(int const *guard, unsigned long long guardSize);  /* other.c: 21 when pair is ten words of its own */

int main(void) {
    shared_counter = 20;
    pair[0] = 1;
    int const bumped = bump();                         /* 21; called first, whatever the order of a sum's operands */
    return bumped + last(guard_main, sizeof guard_main); /* 21 + (20 + 1) = 42 */
}
