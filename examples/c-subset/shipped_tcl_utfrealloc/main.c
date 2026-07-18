#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C22 2026-07-18): the RUN witness for
   the 3 Tcl functions added to tcl.json to clear test6 + test_vfs (their P9006 blocker
   was fixed in C21; the residual was S0001 on these):
     - Tcl_AttemptRealloc -- `char *Tcl_AttemptRealloc(char *ptr, unsigned int size)`
       (grows a buffer, contents preserved) [test6];
     - Tcl_UtfToLower     -- `int Tcl_UtfToLower(char *src)` (in-place lowercase,
       returns the new byte length) [test6];
     - Tcl_AppendObjToObj -- `void Tcl_AppendObjToObj(Tcl_Obj*, Tcl_Obj*)` [test_vfs].
   All three are EXPORTED libtcl8.6 functions (nm -D: T). Adding them takes test6 +
   test_vfs fully CLEAN (34->36).

   Cmd is a C command libtcl CALLS BACK on the eval, value-divergent over values libtcl
   round-trips through its own machinery:
     (1) Tcl_AppendObjToObj appends obj "0" onto obj "4" -> the obj holds "40", parsed
         back to int 40 (Tcl_GetIntFromObj) -- a broken append yields the wrong base;
     (2) Tcl_AttemptRealloc grows a 4-byte buffer holding "AB" to 64 bytes (contents
         preserved), then Tcl_UtfToLower lowercases it in place to "ab" returning byte
         length 2 -- a broken realloc loses "AB" and a broken UtfToLower leaves 'A'.
   Result = 40 + (2 + (s[0]=='a' ? 0 : 100)) = 42.

   RED-ON-DISABLE: remove any of the 3 from tcl.json -> S0001 "undeclared identifier" ->
   this example stops compiling. elf-x86_64 only (tcl.json availableObjectFormats:[elf]);
   the CI ubuntu x86_64 legs run it (libtcl8.6). */

static int Cmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *const objv[]) {
    /* (1) Tcl_AppendObjToObj: "4" + "0" -> "40" -> 40. */
    Tcl_Obj *o = Tcl_NewStringObj("4", 1);
    Tcl_AppendObjToObj(o, Tcl_NewStringObj("0", 1));
    int base = 0;
    if (Tcl_GetIntFromObj(ip, o, &base) != 0) return 1;   /* base = 40 */

    /* (2) Tcl_AttemptRealloc grows "AB"; (3) Tcl_UtfToLower -> "ab", returns 2. */
    char *s = Tcl_AttemptAlloc(4);
    if (s == 0) return 2;
    s[0] = 'A'; s[1] = 'B'; s[2] = 0;
    s = Tcl_AttemptRealloc(s, 64);
    if (s == 0) return 3;
    int n = Tcl_UtfToLower(s);                            /* s -> "ab", n = 2 */
    int add = n + (s[0] == 'a' ? 0 : 100);                /* 2 + 0 */

    Tcl_SetObjResult(ip, Tcl_NewIntObj(base + add));      /* 40 + 2 = 42 */
    return 0;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "c", Cmd, 0, 0);
    if (Tcl_Eval(ip, "c") != 0) return 2;
    int r = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &r) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-utfrealloc-ok");
    return r;   /* 42, computed by DSS code called back from libtcl */
}
