/* FC17.9(i) (D-CSUBSET-INLINE-ASM / -TEXT): a non-empty `__asm__` template carries
 * real per-target instructions cycle-1 cannot emit, so it fails loud
 * S_InlineAsmNonEmptyTemplate (S0057) at the template node — never silently lowered
 * to a no-op barrier (which would drop the instructions, a miscompile). The positioned
 * diagnostic is pinned in the golden .diag. */
int main(void) { __asm__("nop"); return 0; }
