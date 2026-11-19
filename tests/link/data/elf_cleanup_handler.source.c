/* P69 fold 3d: the source of the checked-in ELF objects `elf_cleanup_handler_*.o` -- a C `cleanup` handler compiled
 * with -fexceptions, so each object's `.eh_frame` names a personality routine (`__gcc_personality_v0`) and points
 * `run`'s FDE at a language-specific data area in `.gcc_except_table`. When an unwind passes through `run` (a
 * forced unwind, as `pthread_exit` raises, out of `sink`), the personality reads that area and runs `done`, so
 * `cleaned` holds the value `run` was given.
 *
 * REBUILD (WSL Ubuntu 24.04; gcc 13.3.0, clang 18.1.3, aarch64-linux-gnu-gcc 13.3.0), each byte-identical twice:
 *   gcc -O2 -fexceptions -fPIC   -c elf_cleanup_handler.source.c -o elf_cleanup_handler_gcc_x86_64_pic.o
 *   gcc -O2 -fexceptions -fno-pic -c elf_cleanup_handler.source.c -o elf_cleanup_handler_gcc_x86_64_nopic.o
 *   clang -O2 -fexceptions -fPIC   -c elf_cleanup_handler.source.c -o elf_cleanup_handler_clang_x86_64_pic.o
 *   clang -O2 -fexceptions -fno-pic -c elf_cleanup_handler.source.c -o elf_cleanup_handler_clang_x86_64_nopic.o
 *   aarch64-linux-gnu-gcc -O2 -fexceptions -fPIC -c elf_cleanup_handler.source.c -o elf_cleanup_handler_gcc_aarch64_pic.o
 */
extern void sink(int *);
extern int cleaned;
static void done(int *p) { cleaned += *p; }
int run(int x) {
    int v __attribute__((cleanup(done))) = x;
    sink(&v);
    return v + 1;
}
