/* `elf_group_jump_table_pair` with the two objects named to the link in the OTHER order: the link keeps the
   first unit's group, which is now unit B's, and both callers answer 44. */
int jump_table_a(void);
int jump_table_b(void);

int main(void) { return jump_table_a() + jump_table_b(); }   /* 44 + 44 = 88 */
