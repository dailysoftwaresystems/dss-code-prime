/* The second unit: each weak copy here loses its name, and the link keeps its bytes, with the alias they carry. */
__attribute__((weak)) int one_w = 9;
extern int one_m __attribute__((alias("one_w")));
int one_read_in_b(void) { return one_m; }

__attribute__((weak)) int own_w = 9;
extern int own_m __attribute__((alias("own_w")));
int own_read_in_b(void) { return own_m; }
