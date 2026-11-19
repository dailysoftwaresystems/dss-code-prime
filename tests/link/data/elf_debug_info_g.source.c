/* P69 fold 3d: the source of `elf_debug_info_g.o` -- two functions compiled with debug information, so the object
 * carries `.debug_*` sections and the relocations that point them at the code. REBUILD (WSL Ubuntu 24.04, gcc 13.3.0,
 * in the directory holding this file):
 *   gcc -g -O2 -fdebug-prefix-map=$PWD=. -c elf_debug_info_g.source.c -o elf_debug_info_g.o */
int add(int a, int b) { return a + b; }
int twice(int a) { return add(a, a); }
