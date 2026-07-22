#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-LIST-VAR-DESCRIPTORS (SQLite testfixture arc, TF-C43): the RUN witness
   for the three Tcl 8.6 API functions added to tcl.json so the last two testfixture
   translation units compile:
     - ext/fts5/fts5_tcl.c    calls Tcl_SetVar2Ex + Tcl_SplitList
     - ext/rtree/test_rtreedoc.c calls Tcl_ListObjReplace
   All three were absent from the descriptor -> S0001 (unresolved symbol) at each
   call site. The signatures are modeled faithfully from tcl8.6/tclDecls.h, reusing
   the exact pointer shapes already proven in tcl.json (the triple pointer
   ptr<ptr<ptr<char>>> mirrors Tcl_ListObjGetElements' ptr<ptr<ptr<Tcl_Obj>>>; the
   Tcl_Obj*const objv[] decays to ptr<ptr<Tcl_Obj>> as in Tcl_NewListObj). All three
   symbols are exported by libtcl8.6.so (nm -D verified) so the eager-import law holds.

   This exercises each of the three against a live libtcl interpreter, value-divergent
   so a mis-modeled descriptor can never coincidentally yield 42:
     (1) Tcl_SplitList parses a 4-element list -> libtcl fills argc == 4;
     (2) Tcl_SetVar2Ex stores int-obj 38 into $x, read back via Tcl_GetVar2Ex == 38;
     (3) Tcl_ListObjReplace deletes 3 elements from index 1 of {0 1 2 3 4} -> length 2.
   exit = xi + argc = 38 + 4 = 42.

   elf-x86_64 only (tcl.json availableObjectFormats:[elf]); the CI ubuntu x86_64 legs
   run it (libtcl8.6 installed). RED-ON-DISABLE: remove any of the three tcl.json
   entries (Tcl_SetVar2Ex / Tcl_SplitList / Tcl_ListObjReplace) -> S0001 at its call
   site here -> this example stops compiling. */

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;

    /* (1) Tcl_SplitList: libtcl splits "aa bb cc dd" and fills argc + argv. */
    int argc = 0;
    const char **argv = 0;
    if (Tcl_SplitList(ip, "aa bb cc dd", &argc, &argv) != 0) return 2;
    if (argc != 4) return 3;                 /* value-divergent: the parsed count */
    Tcl_Free((char *)argv);

    /* (2) Tcl_SetVar2Ex: store int-obj 38 into $x, then read it back. */
    Tcl_SetVar2Ex(ip, "x", 0, Tcl_NewIntObj(38), 0);
    Tcl_Obj *xv = Tcl_GetVar2Ex(ip, "x", 0, 0);
    int xi = 0;
    if (xv == 0 || Tcl_GetIntFromObj(ip, xv, &xi) != 0) return 4;
    if (xi != 38) return 5;                  /* value-divergent: the round-tripped int */

    /* (3) Tcl_ListObjReplace: {0 1 2 3 4}, delete 3 elements from index 1 -> length 2. */
    Tcl_Obj *lst = Tcl_NewObj();
    for (int i = 0; i < 5; i++) {
        Tcl_ListObjAppendElement(ip, lst, Tcl_NewIntObj(i));
    }
    if (Tcl_ListObjReplace(ip, lst, 1, 3, 0, 0) != 0) return 6;
    int len = 0;
    Tcl_ListObjLength(ip, lst, &len);
    if (len != 2) return 7;                  /* value-divergent: the post-replace length */

    Tcl_DeleteInterp(ip);
    puts("tcl-list-ops-ok");
    return xi + argc;                        /* 38 + 4 = 42 */
}
