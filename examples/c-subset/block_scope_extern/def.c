// The sibling TU: defines what main.c block-extern-declares. The LK11 merge's
// cross-CU winner definitions — main.c's block-scope externs (one FUNCTION, one
// OBJECT) collapse onto these: the function call is rewired direct, the object
// load reaches this global.
int g_base = 40;
int helper(int x) { return x + 2; }
