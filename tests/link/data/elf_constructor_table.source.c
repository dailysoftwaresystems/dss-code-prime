/* P69 fold 3d: the source of `elf_constructor_table.o` -- a constructor, so gcc writes a `.init_array` slot and the
 * relocation that fills it. REBUILD (WSL Ubuntu 24.04, gcc 13.3.0):
 *   gcc -O2 -c elf_constructor_table.source.c -o elf_constructor_table.o */
static int value;
__attribute__((constructor)) static void initialise(void) { value = 42; }
int get(void) { return value; }
