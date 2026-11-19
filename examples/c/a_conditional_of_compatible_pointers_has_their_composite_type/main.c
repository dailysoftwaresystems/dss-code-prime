/* A CONDITIONAL OF POINTERS TO COMPATIBLE TYPES HAS A POINTER TO THEIR COMPOSITE (C 6.5.15p6, 6.2.7p3; P69 round 1,
 * lane cs, fold f26c). `int (*)[]` and `int (*)[3]` are compatible and their composite is `int (*)[3]`, so the
 * conditional of the two points to an array of 3 -- its size is 12 -- and the same holds through two function
 * pointers and two function designators. Every return before 42 names the first fact that failed. */
static int store[3] = { 40, 2, 0 };
static int (*g3(void))[3] { return &store; }
static int (*gu(void))[] { return (int (*)[])store; }
volatile int one = 1;

int main(void) {
    int (*pu)[] = (int (*)[])store;
    int (*p3)[3] = &store;
    if ((int)sizeof *(one ? pu : p3) + 30 != 42) return 1;      /* the composite's size, 12 */
    int (*(*fu)(void))[] = gu;
    int (*(*f3)(void))[3] = g3;
    if ((int)sizeof *(one ? fu : f3)() + 30 != 42) return 2;    /* through two function pointers */
    int (*(*fp)(void))[3] = one ? g3 : gu;                       /* two function designators */
    if ((*fp())[0] + (*fp())[1] != 42) return 3;
    if ((*(one ? pu : p3))[1] != 2) return 4;                    /* the composite indexes as int[3] */
    return 42;
}
