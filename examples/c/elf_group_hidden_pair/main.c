/* Two assembler-written ELF objects each carry COMDAT group `h`, which DEFINES the strong, hidden global `h`,
   and each calls `h` from a function of its own. The link keeps ONE group, the first unit's: with unit A first
   both callers answer 7. */
int group_hidden_a(void);
int group_hidden_b(void);

int main(void) { return group_hidden_a() * 10 + group_hidden_b(); }   /* 7 * 10 + 7 = 77 */
