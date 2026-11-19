/* The second unit: each select-any copy here loses, and each WEAK alias of it stays a weak definition of its name. */
__declspec(selectany) int yield_w = 9;
extern int yield_m __attribute__((weak, alias("yield_w")));
int yield_read_in_b(void) { return yield_m; }

__declspec(selectany) int hold_w = 9;
extern int hold_m __attribute__((weak, alias("hold_w")));
int hold_read_in_b(void) { return hold_m; }

__declspec(selectany) int quiet_w = 9;
extern int quiet_m __attribute__((weak, alias("quiet_w")));
int quiet_read_in_b(void) { return quiet_m; }

__declspec(selectany) int pair_w = 9;
extern int pair_m __attribute__((weak, alias("pair_w")));
int pair_read_in_b(void) { return pair_m; }

__declspec(selectany) int first_w = 9;
extern int first_m __attribute__((weak, alias("first_w")));
int first_read_in_b(void) { return first_m; }
