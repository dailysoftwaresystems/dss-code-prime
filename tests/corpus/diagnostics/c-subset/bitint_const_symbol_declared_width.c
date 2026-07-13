const _BitInt(4) k = 20wb;  /* C4b I1: (_BitInt(4))20 == 4, so `k == 20` is FALSE */
_Static_assert(k == 20, "k truncates to 4");
int main(void) { return 0; }
