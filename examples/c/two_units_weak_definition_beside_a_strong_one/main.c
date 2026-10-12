/* A WEAK DEFINITION IN ONE UNIT, A STRONG ONE IN THE OTHER — IN BOTH ORDERS.
   `weak_here` is a weak definition (4) here and a strong one (20) in other.c; `weak_there` is strong (21) here and
   weak (5) there. The strong definition is the name's, whichever unit the link reads first, and BOTH units read
   it: the weak one's own unit too. */
__attribute__((weak)) int weak_here = 4;
int weak_there = 21;

int read_weak_here(void);     /* other.c: weak_here */
int read_weak_there(void);    /* other.c: weak_there */

int main(void) {
    if (weak_here != read_weak_here()) return 1;     /* one object: both units read 20 */
    if (weak_there != read_weak_there()) return 2;   /* one object: both units read 21 */
    return weak_here + weak_there + 1;               /* 20 + 21 + 1 = 42 */
}
