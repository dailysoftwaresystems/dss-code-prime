#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C4 2026-07-17): the RUN witness
   for the TCL scalar/string/result surface. A registered C command reads a
   STRING argument (Tcl_GetString), validates its arg count (Tcl_WrongNumArgs on
   error), and returns a Tcl_Obj result — the shape sqlite's src/test*.c commands
   use pervasively. Exercises the C4 additions non-speculatively: the ClientData
   parameter type, the TCL_OK / TCL_ERROR PP-macros, NULL (the CreateObjCommand
   clientData/deleteProc args), Tcl_GetString, and Tcl_WrongNumArgs.

   libtcl parses `expr [len abc] + 39`, CALLS BACK into LenCmd for `len abc`
   (which returns the length 3), and the expr yields 3 + 39 = 42 — computed
   across the FFI boundary in DSS-compiled code invoked by libtcl on arguments
   libtcl parsed at runtime.

   RED-ON-DISABLE: revert tcl.json's elf `library` to libc.so.6 -> ld.so refuses
   to load (undefined symbol: Tcl_CreateInterp), exit 127. */

static int LenCmd(ClientData cd, Tcl_Interp *ip, int objc, Tcl_Obj **objv) {
    if (objc != 2) { Tcl_WrongNumArgs(ip, 1, objv, "string"); return TCL_ERROR; }
    char *s = Tcl_GetString(objv[1]);
    int n = 0;
    while (s[n]) n++;                 /* strlen, without <string.h> */
    Tcl_SetObjResult(ip, Tcl_NewIntObj(n));
    return TCL_OK;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "len", LenCmd, NULL, NULL);
    if (Tcl_Eval(ip, "expr [len abc] + 39") != TCL_OK) return 2;
    int n = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &n) != TCL_OK) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-strcmd-ok");
    return n;   /* [len abc] = 3, + 39 = 42 */
}
