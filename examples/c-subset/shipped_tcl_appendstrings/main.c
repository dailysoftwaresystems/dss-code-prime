#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C16 2026-07-18): the RUN witness
   for the two Tcl functions added to tcl.json to clear test_func's last blockers:
     - Tcl_AppendStringsToObj -- a VARIADIC `void Tcl_AppendStringsToObj(Tcl_Obj*,
       ...)` (a NULL-terminated list of char* appended to the object);
     - Tcl_NewDoubleObj -- `Tcl_Obj *Tcl_NewDoubleObj(double)`.
   Both are exported libtcl8.6 functions (not macros). Adding them takes test_func
   fully CLEAN (32->33; its only errors were S0001 on these two).

   AppendCmd is a C command libtcl CALLS BACK on the eval:
     - it builds the string "40" by APPENDING "4" then "0" (NULL-terminated) to a
       fresh string obj via Tcl_AppendStringsToObj, then parses it back to the int
       40 (Tcl_GetIntFromObj) -- so a broken variadic append yields the wrong base;
     - it makes a double obj 2.0 via Tcl_NewDoubleObj and reads it back
       (Tcl_GetDoubleFromObj) -- so a broken double obj yields the wrong addend.
   Result = 40 + 2 = 42, computed by DSS code invoked BY libtcl on values libtcl
   round-tripped through its own Tcl_Obj machinery (value-divergent).

   RED-ON-DISABLE: remove Tcl_AppendStringsToObj or Tcl_NewDoubleObj from tcl.json
   -> S0001 "undeclared identifier" -> this example stops compiling. elf-x86_64 only
   (tcl.json availableObjectFormats:[elf]); the CI ubuntu x86_64 legs run it. */

static int AppendCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *const objv[]) {
    /* (1) VARIADIC Tcl_AppendStringsToObj: append "4","0",NULL -> the obj holds "40". */
    Tcl_Obj *o = Tcl_NewStringObj("", 0);
    Tcl_AppendStringsToObj(o, "4", "0", (char *)0);
    int base = 0;
    if (Tcl_GetIntFromObj(ip, o, &base) != 0) return 1;   /* base = 40 */

    /* (2) Tcl_NewDoubleObj: a double obj 2.0, read back as 2. */
    Tcl_Obj *d = Tcl_NewDoubleObj(2.0);
    double dv = 0.0;
    if (Tcl_GetDoubleFromObj(ip, d, &dv) != 0) return 2;
    int add = (int)dv;                                     /* add = 2 */

    Tcl_SetObjResult(ip, Tcl_NewIntObj(base + add));       /* 40 + 2 = 42 */
    return 0;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "app", AppendCmd, 0, 0);
    if (Tcl_Eval(ip, "app") != 0) return 2;
    int r = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &r) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-append-ok");
    return r;   /* 42, computed by DSS code called back from libtcl */
}
