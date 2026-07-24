#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C9 2026-07-17): the RUN witness for
   the TCL FUNCTION surface added to tcl.json (39 exported Tcl_* functions + the
   TCL_GLOBAL_ONLY/etc. flag macros) — the dominant S0001 "undeclared identifier"
   blocker after C8 (12/44 src/test*.c hit it first: Tcl_NewStringObj [26],
   Tcl_SetVar2 [25], Tcl_ListObjAppendElement [27], Tcl_GetInt, Tcl_NewObj, ...).

   SumCmd is a C command libtcl CALLS BACK: it builds a 2-element Tcl list from
   string objects (Tcl_NewObj + Tcl_ListObjAppendElement + Tcl_NewStringObj), reads
   its length (Tcl_ListObjLength) and each element (Tcl_ListObjIndex + Tcl_GetIntFromObj),
   and sums 40 + 2 = 42 -- computed by DSS code invoked BY libtcl on values libtcl
   parsed, nothing an optimizer can fold. main also exercises Tcl_SetVar (with the
   TCL_GLOBAL_ONLY flag macro) + Tcl_ResetResult.

   NOTE: Tcl_IncrRefCount / Tcl_DecrRefCount are NOT used here -- they are macros in
   real tcl.h (NOT exported by libtcl8.6.so), deferred to D-FFI-TCL-REFCOUNT-MACROS.

   RED-ON-DISABLE: remove any used Tcl_* symbol from tcl.json -> that call fails
   S0001 "undeclared identifier" -> the example stops compiling. */

static int SumCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *const objv[]) {
    Tcl_Obj *list = Tcl_NewObj();                                 /* Tcl_NewObj */
    Tcl_ListObjAppendElement(ip, list, Tcl_NewStringObj("40", 2));/* +NewStringObj */
    Tcl_ListObjAppendElement(ip, list, Tcl_NewStringObj("2", 1)); /* ListObjAppendElement */
    int n = 0;
    if (Tcl_ListObjLength(ip, list, &n) != 0) return 1;           /* Tcl_ListObjLength */
    int total = 0, i;
    for (i = 0; i < n; i++) {
        Tcl_Obj *e;
        if (Tcl_ListObjIndex(ip, list, i, &e) != 0) return 1;     /* Tcl_ListObjIndex */
        int v = 0;
        if (Tcl_GetIntFromObj(ip, e, &v) != 0) return 1;
        total += v;                                               /* 40 + 2 = 42 */
    }
    Tcl_SetObjResult(ip, Tcl_NewIntObj(total));
    return 0;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_SetVar(ip, "marker", "ok", TCL_GLOBAL_ONLY);              /* Tcl_SetVar + TCL_GLOBAL_ONLY */
    Tcl_CreateObjCommand(ip, "mysum", SumCmd, 0, 0);
    if (Tcl_Eval(ip, "mysum") != 0) return 2;
    int r = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &r) != 0) return 3;
    Tcl_ResetResult(ip);                                          /* Tcl_ResetResult */
    Tcl_DeleteInterp(ip);
    puts("tcl-listfuncs-ok");
    return r;   /* 42, computed by DSS code called back from libtcl */
}
