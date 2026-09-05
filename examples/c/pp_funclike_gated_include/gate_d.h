// Reached ONLY through a COMPUTED `#include` (C23 6.10.2p4) whose operand is a
// function-like macro CALL: `#define HDR(x) #x` then `#include HDR(gate_d.h)`.
// The same weakness as the guards above, through a different arm — and one the
// row that owns this example did not name.
// [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]]
#define GATE_D_VALUE 12

int gate_d_value(void) { return GATE_D_VALUE; }
