// D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0 — the test_syscall.c analogue.
//
// A NON-static (external linkage → Global binding) syscall table of a DIFFERENT
// struct shape (extra fields → a larger row stride), sharing the NAME `vfsSyscall`
// with os_unix.c's `static` table. Its presence in the link is exactly what exposed
// the cross-CU merge's Local/Global name-collapse: the merge folded os_unix's Local
// table onto THIS external one. This table + its accessor must keep returning 999,
// independent of os_unix's table.
typedef int (*syscall_ptr)(void);

static int testOpen(void){ return 999; }   // the "ts_open" analogue

struct test_syscall {
  const char *zName;
  syscall_ptr xTest;
  syscall_ptr xOrig;
  int e0;
  int e1;
} vfsSyscall[] = {                          // NON-static: external linkage (Global)
  { "open", (syscall_ptr)testOpen, 0, 0, 0 },
};

// Reads testvfs's OWN external table — must dispatch to testOpen → 999.
int testDispatch(void){
  return ((int (*)(void))vfsSyscall[0].xTest)();
}
