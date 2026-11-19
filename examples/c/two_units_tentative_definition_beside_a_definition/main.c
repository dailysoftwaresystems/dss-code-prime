/* A TENTATIVE DEFINITION IN ONE UNIT, A DEFINITION WITH A VALUE IN THE OTHER — IN BOTH ORDERS.
   `seeded_there` is tentative here and defined with 20 in other.c; `seeded_here` is defined with 21 here and
   tentative there. Each name is ONE object and holds the value its definition gave it, whichever unit the link
   reads first. */
int seeded_there;
int seeded_here = 21;

int read_there(void);    /* other.c: seeded_there */
int read_here(void);     /* other.c: seeded_here */

int main(void) {
    if (seeded_there != read_there()) return 1;   /* one object: both units read 20 */
    if (seeded_here != read_here()) return 2;     /* one object: both units read 21 */
    return seeded_there + seeded_here + 1;        /* 20 + 21 + 1 = 42 */
}
