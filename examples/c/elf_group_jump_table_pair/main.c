/* Two assembler-written ELF objects each carry COMDAT group `f` of TWO sections — the function and its jump
   table, as gcc writes one — and each calls `f(1)` from a function of its own. The link keeps ONE group, the
   first unit's: with unit A first both callers answer 22. */
int jump_table_a(void);
int jump_table_b(void);

int main(void) { return jump_table_a() + jump_table_b(); }   /* 22 + 22 = 44 */
