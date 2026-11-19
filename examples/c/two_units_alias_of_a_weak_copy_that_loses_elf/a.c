/* The first unit: each weak copy here is its name's. */
__attribute__((weak)) int one_w = 7;
__attribute__((weak)) int one_m = 5;
int one_read_in_a(void) { return one_m; }

__attribute__((weak)) int own_w = 7;
int own_read_in_a(void) { return own_w; }
