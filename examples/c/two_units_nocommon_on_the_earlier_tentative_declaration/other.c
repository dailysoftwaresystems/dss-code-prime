/* The other unit's two tentative declarations of the same object, in the same order (see main.c). */
int t __attribute__((nocommon));
int t;

int other(void) { return t == 40 ? 2 : 0; }
