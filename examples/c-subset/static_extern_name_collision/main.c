// D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0 — the runnable witness.
//
// os_unix.c's `static vfsSyscall[]` and testvfs.c's extern `vfsSyscall[]` share a
// NAME but are DISTINCT objects (internal vs external linkage, different struct
// shapes). `osOpen()` reads os_unix's OWN static table → localOpen → 42;
// `testDispatch()` reads testvfs's OWN extern table → testOpen → 999.
//
// If the cross-CU merge ALIASES os_unix's static table onto testvfs's extern table
// (the bug), osOpen reads testvfs's row through os_unix's smaller struct and
// dispatches to testOpen → osOpen() returns 999 (or crashes on the layout mismatch),
// so `main` returns 999 → exit 231 (999 & 0xFF), clearly != 42.
//
// Exit 42 IFF the file-local static table stayed distinct from the same-named extern.
extern int osOpen(void);
extern int testDispatch(void);

int main(void){
  int a = osOpen();        // must be 42  (os_unix's own static table)
  int b = testDispatch();  // must be 999 (testvfs's own extern table)
  if (a == 42 && b == 999) return 42;
  return a;                // on the bug a == 999 → exit 231, not 42
}
