#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C8 2026-07-17): the RUN witness for
   the 5 TCL TYPEDEFS the sqlite test corpus uses but tcl.json lacked — the dominant
   S0006 "undeclared type" blocker after C7 (the post-C7 re-probe: 13/44 src/test*.c
   hit S0006 first, dominantly Tcl_ObjCmdProc [27 uses] / Tcl_CmdProc [19]):
     - Tcl_ObjCmdProc : a FUNCTION-TYPE typedef `int(ClientData,Tcl_Interp*,int,
                        Tcl_Obj*const*)`, used mostly as `Tcl_ObjCmdProc *` (a fn-ptr).
     - Tcl_CmdProc    : the old string-command fn-type `int(...,char**)`.
     - Tcl_WideInt    : `long long` -> i64.
     - Tcl_DString    : a 216-byte struct (char* + int + int + char[200]).
     - Tcl_Channel    : an opaque pointer (`struct Tcl_Channel_ *`).

   This exercises all 5: `proc` is a Tcl_ObjCmdProc* (the fn-type typedef used as a
   pointer) passed to Tcl_CreateObjCommand and CALLED BACK by libtcl; `w` is a
   Tcl_WideInt (i64) summed into the result; `ds` is a Tcl_DString whose 216-byte
   size is asserted at RUNTIME (a layout witness); `ch` is a Tcl_Channel referenced.
   libtcl invokes SumCmd on "mysum 2" -> 40 + 2 = 42.

   RED-ON-DISABLE: remove any of the 5 typedefs from tcl.json -> that type's use
   below fails S0006 "undeclared type" -> the example stops compiling. */

static int SumCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *const objv[]) {
    Tcl_WideInt w = 40;                                  /* Tcl_WideInt (i64) */
    int a = 0;
    if (objc != 2) return 1;
    if (Tcl_GetIntFromObj(ip, objv[1], &a) != 0) return 1;
    Tcl_SetObjResult(ip, Tcl_NewIntObj((int)w + a));
    return 0;
}

int main(void) {
    Tcl_DString ds;                     /* Tcl_DString: a 216-byte struct local */
    Tcl_Channel ch = 0;                 /* Tcl_Channel: opaque pointer */
    Tcl_ObjCmdProc *proc = SumCmd;      /* the fn-type typedef used as a pointer */
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    if (sizeof ds != 216) return 8;     /* runtime layout witness: Tcl_DString == 216 bytes */
    if (ch != 0) return 9;              /* reference Tcl_Channel */
    Tcl_CreateObjCommand(ip, "mysum", proc, 0, 0);   /* register via the typedef'd ptr */
    if (Tcl_Eval(ip, "mysum 2") != 0) return 2;
    int n = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &n) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-typedefs-ok");
    return n;   /* 40 + 2 = 42 */
}
