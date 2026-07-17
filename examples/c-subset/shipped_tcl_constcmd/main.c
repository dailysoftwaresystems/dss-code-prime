#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C7 2026-07-17): the RUN witness for
   the `CONST` compatibility macro. Every SQLite src/test*.c object-command uses the
   OLD-Tcl signature `Tcl_Obj *CONST objv[]` (tcl.h's `#define CONST const`, the
   pre-8.4 backward-compat macro) — 30 of the 44 test TUs. Before C7, DSS left `CONST`
   UNEXPANDED (tcl.json lacked the macro), so `Tcl_Obj *CONST` parsed as a pointer +
   a declarator named `CONST`, then `objv` was unexpected -> P0001 "expected ')' got
   'objv'". This was the DOMINANT test-TU blocker after the C6 extern-aggregate fix
   (16 of the 44 TUs hit it FIRST). C7 adds `CONST`->`const` to tcl.json's `macros`
   (the signal.json PP-macro precedent), so `Tcl_Obj *CONST objv[]` ->
   `Tcl_Obj *const objv[]` (an array of const pointers — the real Tcl objProc
   signature) parses, registers via Tcl_CreateObjCommand, and runs.

   `SumCmd`'s parameter list uses `Tcl_Obj *CONST objv[]` (the fixed form): libtcl
   calls it back with the parsed args of the script "mysum 40 2" -> 42, computed by
   DSS-compiled code invoked BY libtcl (nothing an optimizer can fold away). The
   const-pointer array param decays to `Tcl_Obj *const *` and binds to
   Tcl_CreateObjCommand's fn-pointer parameter exactly as the test TUs' commands do.

   RED-ON-DISABLE: remove the `CONST`->`const` macro from tcl.json -> `Tcl_Obj *CONST
   objv[]` P0001s ("expected ')' got 'objv'") -> the example fails to compile. */

static int SumCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *CONST objv[]) {
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
    puts("tcl-constcmd-ok");
    return n;   /* 42, computed by DSS code called back from libtcl */
}
