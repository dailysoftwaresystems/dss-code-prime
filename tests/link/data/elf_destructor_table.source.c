/* P69 fold 3d: the source of `elf_destructor_table.o` -- a destructor, so gcc writes a `.fini_array` slot and the
 * relocation that fills it. REBUILD (WSL Ubuntu 24.04, gcc 13.3.0):
 *   gcc -O2 -c elf_destructor_table.source.c -o elf_destructor_table.o */
int finished;
__attribute__((destructor)) static void finish(void) { finished = 1; }
int get(void) { return finished; }
