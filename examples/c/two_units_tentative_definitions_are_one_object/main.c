/* TWO UNITS, ONE NAME, NO INITIALIZER IN EITHER: ONE OBJECT.
   `int shared_counter;` here and in other.c are two TENTATIVE definitions of one object (C 6.9.2p2), and the link
   makes them one. `pair` is declared `int pair[1];` here and `int pair[10];` there: the one object is as large as
   the larger of them, so the other unit's `pair[9]` is storage of the program and `pair[0]` is the same word in
   both units. */
int shared_counter;
int pair[1];

int bump(void);          /* other.c: ++shared_counter */
int last(void);          /* other.c: writes pair[9], returns pair[9] + pair[0] */

int main(void) {
    shared_counter = 20;
    pair[0] = 1;
    return bump() + last();   /* 21 + (20 + 1) = 42 */
}
