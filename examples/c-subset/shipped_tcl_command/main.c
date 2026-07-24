#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C2 2026-07-17): the RUN witness
   for the TCL command-registration (FFI reverse-callback) pattern — the core
   shape of EVERY SQLite `src/test*.c` test command (test1.c alone registers
   ~200 of them this way).

   `SumCmd` is a C function that libtcl CALLS BACK when the interpreted script
   invokes the `mysum` command: libtcl parses the script string "mysum 40 2",
   hands the C function its arguments as an array of Tcl_Obj* (objv[]), and the
   C function parses them (Tcl_GetIntFromObj), computes, and sets a Tcl_Obj
   result. DSS must (1) model Tcl_CreateObjCommand's function-pointer parameter
   (`ptr<fn(...)>` in tcl.json — the signal.json handler precedent), (2) emit
   SumCmd as an address-taken function with the SysV ABI libtcl expects, and
   (3) let the DSS-linked libtcl call it back across the FFI boundary. The
   `40 + 2` is computed by DSS-compiled code invoked BY libtcl on arguments
   libtcl parsed at runtime — nothing an optimizer pass can fold away.

   RED-ON-DISABLE: revert tcl.json's elf `library` to libc.so.6 -> ld.so refuses
   to load (undefined symbol: Tcl_CreateInterp), exit 127. */

static int SumCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj **objv) {
    int a = 0, b = 0;
    if (objc != 3) return 1;                            /* TCL_ERROR: wrong # args */
    if (Tcl_GetIntFromObj(ip, objv[1], &a) != 0) return 1;
    if (Tcl_GetIntFromObj(ip, objv[2], &b) != 0) return 1;
    Tcl_SetObjResult(ip, Tcl_NewIntObj(a + b));
    return 0;                                           /* TCL_OK */
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "mysum", SumCmd, 0, 0);
    if (Tcl_Eval(ip, "mysum 40 2") != 0) return 2;
    int n = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &n) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-cmd-ok");
    return n;   /* 42, computed by DSS code called back from libtcl */
}
