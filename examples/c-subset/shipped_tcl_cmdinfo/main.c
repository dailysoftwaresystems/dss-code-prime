#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C5 2026-07-17): the RUN witness
   for the TCL command-introspection layer — the Tcl_CmdInfo struct +
   Tcl_GetCommandInfo / Tcl_SetCommandInfo / Tcl_DeleteCommand. This is the FIRST
   field-access shipped struct in tcl.json (C1-C4 used opaque handles): DSS
   models Tcl_CmdInfo's 8 fields (int + 7 pointers) via the `structs` surface so
   `info.isNativeObjectProc` / `info.clientData` resolve AND libtcl fills the
   struct at the ABI-correct offsets. test_backup.c uses exactly this pattern.

   The `info.isNativeObjectProc != 1` check is a RUNTIME LAYOUT WITNESS: libtcl
   sets that field (offset 0) to 1 for a command registered via
   Tcl_CreateObjCommand, so a wrong struct layout would read the wrong bytes and
   fail. RED-ON-DISABLE: revert tcl.json's elf `library` to libc.so.6 -> ld.so
   refuses to load (undefined symbol: Tcl_CreateInterp), exit 127. */

static int AnsCmd(ClientData cd, Tcl_Interp *ip, int objc, Tcl_Obj **objv) {
    Tcl_SetObjResult(ip, Tcl_NewIntObj(42));
    return TCL_OK;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "ans", AnsCmd, NULL, NULL);

    Tcl_CmdInfo info;
    if (Tcl_GetCommandInfo(ip, "ans", &info) == 0) return 4;  /* 0 = command not found */
    if (info.isNativeObjectProc != 1) return 5;               /* wrong Tcl_CmdInfo layout */
    info.clientData = NULL;                                   /* field write */
    Tcl_SetCommandInfo(ip, "ans", &info);

    if (Tcl_Eval(ip, "ans") != TCL_OK) return 2;
    int n = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &n) != TCL_OK) return 3;
    Tcl_DeleteCommand(ip, "ans");
    Tcl_DeleteInterp(ip);
    puts("tcl-cmdinfo-ok");
    return n;   /* 42 */
}
