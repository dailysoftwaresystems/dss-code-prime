/* The first unit: each select-any copy here is the one the link keeps. */
__declspec(selectany) int yield_w = 7;
int yield_read_w_in_a(void) { return yield_w; }

__declspec(selectany) int hold_w = 7;
int hold_read_w_in_a(void) { return hold_w; }

__declspec(selectany) int quiet_w = 7;
int quiet_read_w_in_a(void) { return quiet_w; }

__declspec(selectany) int pair_w = 7;
extern int pair_m __attribute__((weak, alias("pair_w")));
int pair_read_in_a(void) { return pair_m; }

__declspec(selectany) int first_w = 7;
__attribute__((weak)) int first_m = 5;
int first_read_in_a(void) { return first_m; }
