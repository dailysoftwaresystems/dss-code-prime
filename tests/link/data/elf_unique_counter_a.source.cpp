// SOURCE of the committed fixtures `tests/link/data/elf_unique_counter_a_x86_64_elf_gxx.o` and
// `elf_unique_counter_a_pic_x86_64_elf_gxx.o` (P69 fold 3c, lane p69/xa;
// D-LK-ELF-READER-KEEPS-EVERY-SECTION-GROUP-AND-DISCARDS-NONE, part (3)): UNIT A of a pair.
//
// `counter` is a C++ INLINE VARIABLE, which g++ writes as `OBJECT UNIQUE` (STB_GNU_UNIQUE, binding 10) in a COMDAT
// group named `counter` (`.data.counter`), under OS/ABI GNU: the one instance of its name in the PROCESS. Both units
// take its address; ONE object makes the two addresses equal (`elf_unique_counter_b.source.cpp` is the other
// unit).
//
// REBUILD (g++ 13.3.0, Ubuntu 24.04, x86_64; the two code lines below were compiled saved as `u1a.cpp`, which is the
// object's FILE symbol, so a build from this file's name differs in that symbol alone):
//   g++ -std=c++17 -O1 -c u1a.cpp -o elf_unique_counter_a_x86_64_elf_gxx.o           md5 9778a674f692012126d43b011e02bbfb
//   g++ -std=c++17 -O1 -fPIC -c u1a.cpp -o elf_unique_counter_a_pic_x86_64_elf_gxx.o  md5 d58b2d3466dbd41beceab45c74e12015
inline int counter = 5;
extern "C" int *addr1(void) { return &counter; }
