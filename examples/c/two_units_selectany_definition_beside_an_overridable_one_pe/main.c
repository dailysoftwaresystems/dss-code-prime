/* A SELECT-ANY DEFINITION IN ONE UNIT, AN OVERRIDABLE WEAK ONE IN THE OTHER — IN BOTH ORDERS (PE).
   `picked_there` is `__attribute__((weak))` (overridable, 4) here and `__declspec(selectany)` (10) in other.c;
   `picked_here` is select-any (11) here and overridable (5) there. PE ranks the two kinds: the select-any
   definition is the name's in either order, and both units read it. */
__attribute__((weak)) int picked_there = 4;
__declspec(selectany) int picked_here = 11;

int read_picked_there(void);    /* other.c: picked_there */
int read_picked_here(void);     /* other.c: picked_here */

int main(void) {
    if (picked_there != read_picked_there()) return 1;   /* one object: both units read 10 */
    if (picked_here != read_picked_here()) return 2;     /* one object: both units read 11 */
    return picked_there + picked_here + 21;              /* 10 + 11 + 21 = 42 */
}
