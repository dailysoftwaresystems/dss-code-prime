#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C27 2026-07-20): the RUN witness
   for Tcl_ObjGetVar2 added to tcl.json — `Tcl_Obj *Tcl_ObjGetVar2(Tcl_Interp*,
   Tcl_Obj *part1Ptr, Tcl_Obj *part2Ptr, int flags)` (tclDecls.h:590), the GET twin
   of the already-shipped Tcl_ObjSetVar2 (one fewer param, no newValuePtr). It is
   exported `T Tcl_ObjGetVar2` (nm -D libtcl8.6.so), so DSS's eager-import-all path
   stays safe. Unblocks sqlite src/test_quota.c:16087
   `Tcl_ObjGetVar2(p->interp, pVarname, 0, 0)` (test_quota.c -> CLEAN, corpus 37->38).

   The witness binds a REAL interpreter variable and reads it back through the new
   symbol: Tcl_Eval("set v 42") stores v=42 inside libtcl; a name object "v" (a
   scalar, part2 = 0, flags 0) is passed to Tcl_ObjGetVar2, which returns the
   Tcl_Obj* libtcl holds for v; Tcl_GetIntFromObj extracts 42 as the exit code. The
   value is stored + retrieved INSIDE libtcl (an opaque call DSS cannot see into),
   so no pass folds it away — a genuine end-to-end binding, value-divergent (a
   broken binding never yields 42).

   elf-x86_64 only (tcl.json availableObjectFormats:[elf]); the CI ubuntu x86_64
   legs run it (libtcl8.6 installed), native-arm64 arch-gates the run,
   windows/macos compile-only. RED-ON-DISABLE: remove Tcl_ObjGetVar2 from tcl.json
   -> S0001 "undeclared identifier" -> this example stops compiling. */

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    /* `set` is a core command registered by Tcl_CreateInterp (cf. `expr`). */
    if (Tcl_Eval(ip, "set v 42") != 0) return 2;
    Tcl_Obj *name = Tcl_NewStringObj("v", -1);   /* the scalar variable name */
    Tcl_IncrRefCount(name);                      /* pin it across the GET call */
    Tcl_Obj *val = Tcl_ObjGetVar2(ip, name, 0, 0);   /* read v back from libtcl */
    if (val == 0) return 3;
    int n = 0;
    if (Tcl_GetIntFromObj(ip, val, &n) != 0) return 4;
    Tcl_DecrRefCount(name);
    Tcl_DeleteInterp(ip);
    puts("tcl-getvar2-ok");
    return n;   /* 42, read from a real interp variable by libtcl */
}
