/* TWO UNITS, `nocommon` ON THE EARLIER OF TWO TENTATIVE DEFINITIONS: ONE OBJECT.
   Each unit declares `int t __attribute__((nocommon));` and then `int t;`. The LATER declaration is the one the
   unit's definition of `t` is read from, and it says nothing of commons: so each unit's `t` is a common, and the
   link makes the two one object. This unit stores 40 in it; other.c reads 40 through its own `t`. */
int t __attribute__((nocommon));
int t;

int other(void);   /* other.c: 2 when it reads the 40 stored here */

int main(void) {
    t = 40;
    return t + other();   /* 40 + 2 = 42 */
}
