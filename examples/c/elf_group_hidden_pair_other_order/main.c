/* `elf_group_hidden_pair` with the two objects named to the link in the OTHER order: the link keeps the first
   unit's group, which is now unit B's, and both callers answer 9. */
int group_hidden_a(void);
int group_hidden_b(void);

int main(void) { return group_hidden_a() * 10 + group_hidden_b(); }   /* 9 * 10 + 9 = 99 */
