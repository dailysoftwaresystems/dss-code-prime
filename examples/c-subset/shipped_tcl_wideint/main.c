#include <tcl.h>
#include <stdio.h>

/* D-LANG-FFI-DESCRIPTOR-INT-POINTEE-COMPAT (SQLite testfixture arc, TF-C41): the
   RUN witness for the shipped-FFI-descriptor integer-pointee call-arg relaxation.

   The real Tcl 8.6 API `int Tcl_GetWideIntFromObj(Tcl_Interp*, Tcl_Obj*,
   Tcl_WideInt*)` is modeled in tcl.json as `fn(..., ptr<i64>) -> i32` — an
   abstract width-based pointee. The SQLite testfixture calls it as
   `Tcl_GetWideIntFromObj(interp, obj, &iVal)` where `iVal` is a `Tcl_WideInt` /
   `sqlite3_int64` (a `typedef long long`). Before this change DSS rejected the
   `long long*` argument S0003 (a distinct-IDENTITY pointee — `long long` is the
   named I64 vocabulary entry, `ptr<i64>` the anonymous one — even though the
   REPRESENTATION is byte-identical on LP64; gcc accepts). The relaxation admits
   the same-representation integer pointer AT THE SHIPPED-DESCRIPTOR CALL-ARG
   BOUNDARY ONLY, and libtcl fills the caller's wide int through it.

   Here `Tcl_Eval("expr 20 + 22")` produces a Tcl wide-int result, and
   `Tcl_GetWideIntFromObj(ip, res, &wide)` — the `&wide` (`sqlite3_int64*` ==
   `long long*`) into the `ptr<i64>` parameter — has libtcl write 42 into `wide`.
   The return value IS that libtcl-filled wide int, so a broken binding never
   yields 42 (value-divergent).

   RED-ON-DISABLE: set `pointerConversions.ffiDescriptorIntPointeeCompat` to false
   in src/dss-config/sources/c-subset.lang.json (or revert the relaxation) -> the
   `Tcl_GetWideIntFromObj(ip, res, &wide)` argument raises S0003 -> this example
   stops compiling. elf-x86_64 only (tcl.json availableObjectFormats:[elf]); the CI
   ubuntu x86_64 legs run it (libtcl8.6 installed). */

typedef long long sqlite3_int64;   /* the sqlite spelling (a typedef long long) */

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    if (Tcl_Eval(ip, "expr 20 + 22") != 0) return 2;
    Tcl_Obj *res = Tcl_GetObjResult(ip);
    sqlite3_int64 wide = 0;
    /* THE witness: &wide is `long long*`, the parameter is `ptr<i64>` — a
       same-representation integer pointer admitted at the shipped-descriptor
       call-arg boundary, then filled by libtcl. */
    if (Tcl_GetWideIntFromObj(ip, res, &wide) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-wideint-ok");
    return (int)wide;   /* 42, written by libtcl through the ptr<i64> parameter */
}
