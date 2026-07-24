#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C28 2026-07-20): the RUN witness
   for the 6 need-driven, PURE-descriptor items added to tcl.json to grow the
   test1.c (keystone command-registration TU) surface —
     - Tcl_GetDouble(Tcl_Interp*, const char*, double*) -> int   (tclDecls.h:146,
       the Tcl_GetInt double-out sibling; exported `T` nm -D libtcl8.6.so),
     - Tcl_DStringAppend(Tcl_DString*, const char*, int) -> char* (tclDecls.h:382,
       the Tcl_DStringAppendElement sibling; exported `T`),
     - Tcl_DStringLength(dsPtr) -> ((dsPtr)->length)             (tcl.h:1014, the
       LENGTH twin of the shipped Tcl_DStringValue macro; reads the Tcl_DString
       `length` field already modeled in C13 — NO new struct layout),
     - TCL_EVAL_DIRECT=0x040000, TCL_LEAVE_ERR_MSG=0x200, TCL_LINK_STRING=4
       (tcl.h:1079/1108/1153) — object-macro constants.

   Value-divergent END-TO-END: Tcl_DStringAppend runs INSIDE libtcl (an opaque
   extern DSS cannot see into), so the 42-byte count it computes and stores in
   Tcl_DString.length — read back out through the new Tcl_DStringLength macro and
   returned as the exit code — is NOT foldable by any pass (a broken append or a
   macro that reads the wrong field never yields 42). Tcl_GetDouble's parse is a
   real runtime gate (a broken parse never lands d in [42.0, 43.0]); the three
   constants are value-pinned (a wrong descriptor value returns 4/5/6, not 42).

   elf-x86_64 only (tcl.json availableObjectFormats:[elf]); the CI ubuntu x86_64
   legs run it (libtcl8.6 installed), native-arm64 arch-gates the run,
   windows/macos compile-only. The `release` shippedPipeline arm runs the
   optimizer over the libtcl calls. RED-ON-DISABLE: remove any of the 6 items from
   tcl.json -> S0001 "undeclared identifier" -> this example stops compiling. */

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;

    /* (1) Tcl_GetDouble parses a decimal string INSIDE libtcl (opaque strtod). */
    double d = 0.0;
    if (Tcl_GetDouble(ip, "42.5", &d) != TCL_OK) return 2;
    if (d < 42.0 || d > 43.0) return 7;

    /* (2) Tcl_DStringAppend builds a 42-byte string inside a DString; its byte
       count flows OUT of libtcl (via the new Tcl_DStringLength macro over the
       Tcl_DString `length` field) straight into the exit code. */
    Tcl_DString ds;
    Tcl_DStringInit(&ds);
    Tcl_DStringAppend(&ds, "hello, world, ", -1);   /* 14 */
    Tcl_DStringAppend(&ds, "hello, world, ", -1);   /* 28 */
    Tcl_DStringAppend(&ds, "hello, world, ", -1);   /* 42 */
    int len = Tcl_DStringLength(&ds);                /* 42, straight from libtcl */
    Tcl_DStringFree(&ds);

    /* (3) the three new object-macro constants, value-pinned. */
    if (TCL_EVAL_DIRECT != 0x040000) return 4;
    if (TCL_LEAVE_ERR_MSG != 0x200) return 5;
    if (TCL_LINK_STRING != 4) return 6;

    Tcl_DeleteInterp(ip);
    puts("tcl-dstring-ok");
    return len;   /* 42 = the DString byte count libtcl computed */
}
