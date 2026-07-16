/* FC17.9(i) (D-CSUBSET-INLINE-ASM-OPERANDS): an inline-asm operand/clobber list
 * (`: outputs : inputs : clobbers`) is NOT admitted in cycle-1 — the asmStmt grammar
 * ends at `)`, so the `:` fails loud at PARSE (P_UnexpectedToken), never silently
 * accepted. The positioned diagnostic is pinned in the golden .diag. */
int main(void) { __asm__("" : : ); return 0; }
