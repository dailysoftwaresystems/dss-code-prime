// SOURCE of the committed fixtures `tests/link/data/elf_unique_counter_b_x86_64_elf_gxx.o` and
// `elf_unique_counter_b_pic_x86_64_elf_gxx.o` (P69 fold 3c, lane p69/xa;
// D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE, part (3)): UNIT B of a pair.
//
// `counter` is a C++ INLINE VARIABLE, which g++ writes as `OBJECT UNIQUE` (STB_GNU_UNIQUE, binding 10) in a COMDAT
// group named `counter` (`.data.counter`), under OS/ABI GNU: the one instance of its name in the PROCESS. Both units
// take its address; ONE object makes the two addresses equal (`elf_unique_counter_a.source.cpp` is the other
// unit).
//
// REBUILD (g++ 13.3.0, Ubuntu 24.04, x86_64; the two code lines below were compiled saved as `u2a.cpp`, which is the
// object's FILE symbol, so a build from this file's name differs in that symbol alone):
//   g++ -std=c++17 -O1 -c u2a.cpp -o elf_unique_counter_b_x86_64_elf_gxx.o           md5 bb03cc7e347a4ff0262a20ff5350cba8
//   g++ -std=c++17 -O1 -fPIC -c u2a.cpp -o elf_unique_counter_b_pic_x86_64_elf_gxx.o  md5 8d68c5c9068c408b81df8ffe2b1a2ed7
inline int counter = 5;
extern "C" int *addr2(void) { return &counter; }
