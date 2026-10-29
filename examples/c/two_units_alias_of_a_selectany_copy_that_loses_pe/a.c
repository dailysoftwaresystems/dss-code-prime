/* The first unit: each select-any copy here is the one the link keeps. */
__declspec(selectany) int one_w = 7;
__attribute__((weak)) int one_m = 5;
int one_read_in_a(void) { return one_m; }

__declspec(selectany) int both_w = 7;
extern int both_m __attribute__((alias("both_w")));
int both_read_in_a(void) { return both_m; }

__declspec(selectany) int own_w = 7;
int own_read_in_a(void) { return own_w; }
