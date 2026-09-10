// Reached ONLY through `#if ENABLED(1)` — a FUNCTION-LIKE macro in the guard.
// [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]]
#define GATE_A_VALUE 10

int gate_a_value(void) { return GATE_A_VALUE; }
