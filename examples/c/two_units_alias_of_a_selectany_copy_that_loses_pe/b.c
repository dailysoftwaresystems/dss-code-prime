/* The second unit: each select-any copy here loses, and the link discards it whole, with the alias it carries. */
__declspec(selectany) int one_w = 9;
extern int one_m __attribute__((alias("one_w")));
int one_read_in_b(void) { return one_m; }

__declspec(selectany) int both_w = 9;
extern int both_m __attribute__((alias("both_w")));
int both_read_in_b(void) { return both_m; }

__declspec(selectany) int own_w = 9;
extern int own_m __attribute__((alias("own_w")));
int own_read_in_b(void) { return own_m; }
