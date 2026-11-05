/* P69 fold 3d (lane xa) -- the ELF fixtures of tests/link/test_link_order.cpp
   (D-LK-THE-LINK-ORDER-IS-NOT-THE-COMMAND-LINE-ORDER): two units of this one source.
     UNIT_WEAK1    a weak `w = 1`                          -> elf_link_order_weak1_gcc_O2.o
     UNIT_MAINOWN  a weak `w = 5` and a `main` returning w -> elf_link_order_mainown_gcc_O2.o
   Beside the test's C units (`main` returns `w * 10 + through_own()`, the other unit defines a weak `w = 5` and
   `through_own`), the program says whose `w` the link kept: 55 the C unit's, 11 the object's. The two objects alone
   say it as 1 or 5.
   REBUILD (gcc 13.3.0, Ubuntu 24.04, x86_64), each in a directory holding only this file:
     gcc -O2 -fno-asynchronous-unwind-tables -DUNIT_WEAK1 -c elf_link_order.source.c -o elf_link_order_weak1_gcc_O2.o
     gcc -O2 -fno-asynchronous-unwind-tables -DUNIT_MAINOWN -c elf_link_order.source.c -o elf_link_order_mainown_gcc_O2.o */
#if defined(UNIT_WEAK1)
int w __attribute__((weak)) = 1;
#elif defined(UNIT_MAINOWN)
int w __attribute__((weak)) = 5;
int main(void) { return w; }
#else
#error "name a unit: -DUNIT_WEAK1 or -DUNIT_MAINOWN"
#endif
