// Reached ONLY through `#if GATE`, where GATE is an OBJECT-like macro whose
// replacement is a function-like INVOCATION. The old pre-scan had a second,
// separate bail for this shape because it could not see the call until after it
// had expanded the operand. [[D-PP-SINGLE-PASS-INCLUDE-RESOLUTION]]
#define GATE_B_VALUE 11

int gate_b_value(void) { return GATE_B_VALUE; }
