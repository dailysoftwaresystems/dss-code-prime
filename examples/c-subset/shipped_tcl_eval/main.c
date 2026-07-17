#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C1 2026-07-17): the RUN witness
   for the libtcl link — the first brick toward building SQLite's `testfixture`
   (which links libtcl8.6 and runs the .test suite = "all sqlite units") with DSS.

   DSS compiles this to an ELF whose imports split across TWO libraries: the five
   Tcl_* symbols resolve from libtcl8.6.so (tcl.json `library`), puts/exit from
   libc.so.6. The ELF walker emits one DT_NEEDED per DISTINCT referenced import
   library (D-FFI-MATH-LIBM-DT-NEEDED — the libm.so.6 precedent), so `readelf -d`
   shows NEEDED libc.so.6 AND NEEDED libtcl8.6.so. At load ld.so binds the Tcl_
   symbols from libtcl and a REAL TCL interpreter evaluates "expr 40 + 2"; the
   result object holds 42, extracted as the process exit code. The arithmetic
   runs INSIDE libtcl (an opaque call DSS cannot see into), so no pass folds it
   away — this is a genuine end-to-end binding of DSS-compiled code against a
   real external shared library, not a constant the optimizer could precompute.

   RED-ON-DISABLE: revert tcl.json's elf `library` to libc.so.6 -> the image
   carries only NEEDED libc.so.6 and ld.so refuses to load it (undefined symbol:
   Tcl_CreateInterp, exit 127) — the exact shape a missing DT_NEEDED produces
   (cf. the pre-libm sqlite3 binary dying on `undefined symbol: sqrt`). */

int main(void) {
    Tcl_Interp *interp = Tcl_CreateInterp();
    if (interp == 0) return 1;
    /* Tcl_Eval returns TCL_OK (0) on success; the core `expr` command is
       registered by Tcl_CreateInterp (no Tcl_Init / script library needed). */
    if (Tcl_Eval(interp, "expr 40 + 2") != 0) return 2;
    Tcl_Obj *res = Tcl_GetObjResult(interp);
    int n = 0;
    if (Tcl_GetIntFromObj(interp, res, &n) != 0) return 3;
    Tcl_DeleteInterp(interp);
    puts("tcl-eval-ok");
    return n;   /* 42, computed by libtcl at runtime */
}
