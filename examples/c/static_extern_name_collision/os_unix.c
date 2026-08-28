// D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0 — the os_unix.c analogue.
//
// A FILE-LOCAL (`static`) syscall-dispatch table whose row points at a file-local
// override function `localOpen`. Its name `vfsSyscall` DELIBERATELY COLLIDES with a
// NON-static `vfsSyscall` of a DIFFERENT struct shape in testvfs.c (a sibling TU).
// C internal linkage (6.2.2p3) makes the two objects DISTINCT — the compiler must
// NOT alias this static table onto testvfs.c's extern one. If it does, `osOpen()`
// reads the WRONG table (a 32-byte-row struct read as a 24-byte one) and dispatches
// through the wrong slot — exactly the sqlite testfixture's NULL-function-pointer
// SIGSEGV at the first FILE-database open (os_unix's aSyscall vs test_syscall's).
typedef int (*syscall_ptr)(void);

static int localOpen(void);   // forward decl (static → internal linkage)

static struct local_syscall {
  const char *zName;
  syscall_ptr pCurrent;
  syscall_ptr pDefault;
} vfsSyscall[] = {
  { "open", (syscall_ptr)localOpen, 0 },
};

static int localOpen(void){ return 42; }

// Reads os_unix's OWN static table — must dispatch to localOpen → 42.
int osOpen(void){
  return ((int (*)(void))vfsSyscall[0].pCurrent)();
}
