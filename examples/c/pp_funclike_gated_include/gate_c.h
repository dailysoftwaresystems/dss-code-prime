// Reached ONLY through `#if CAT(ON,E)` — a TOKEN PASTE in the guard, which a
// rescan-only expander cannot perform at all (it needs the argument machinery
// and the mint path). [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]]
#define GATE_C_VALUE 9

int gate_c_value(void) { return GATE_C_VALUE; }
