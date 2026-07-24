#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C13 2026-07-18): the RUN witness
   for the TCL REFCOUNT macros + the Tcl_DString named layout added to tcl.json
   (dissolves D-FFI-TCL-REFCOUNT-MACROS — the dominant post-C9 S0001 test-TU
   blocker: test_bestindex/test_func/test_mutex use Tcl_IncrRefCount, test_tclvar
   uses Tcl_DecrRefCount, test8 uses Tcl_DStringValue).

   Tcl_IncrRefCount / Tcl_DecrRefCount are MACROS in real tcl.h that poke
   Tcl_Obj->refCount directly. libtcl8.6.so does NOT export them — BUT it DOES
   export Tcl_DbIncrRefCount / Tcl_DbDecrRefCount (nm -D), which perform the same
   ++/--refCount (+free at 0). So tcl.json models the macros as function-macros
   over those EXPORTED funcs (the ckalloc/ckfree precedent): NO Tcl_Obj named-field
   layout is needed (Tcl_Obj stays an opaque blob), so there is NO ~30-signature
   ripple. Because the Db* backers are EXPORTED, DSS's eager-import-all path stays
   safe (unlike the C9 macro-only attempt that broke every tcl binary's LOAD 127).

   RefcCmd is a C command libtcl CALLS BACK on the eval:
     (1) it pins a freshly-created Tcl_Obj with Tcl_IncrRefCount (-> the exported
         Tcl_DbIncrRefCount), reads 40 out of it while pinned, then releases it
         with Tcl_DecrRefCount (-> Tcl_DbDecrRefCount, freeing it at refCount 0);
     (2) it reads a Tcl_DString's `string` field via Tcl_DStringValue(&ds) ->
         ((&ds)->string). That field read requires Tcl_DString's REAL named layout
         { char *string; int length; int spaceAvl; char staticSpace[200]; } — libtcl
         fills `ds` at those ABI offsets, so `string` MUST sit at offset 0 or the
         macro reads the wrong bytes. After appending the list element "2", the
         string base holds "2", so s[0]-'0' == 2.
   Result = 40 + 2 = 42, computed by DSS code invoked BY libtcl on values libtcl
   parsed/filled — value-divergent (a broken binding never yields 42).

   RED-ON-DISABLE: remove Tcl_DbIncrRefCount/Tcl_DbDecrRefCount (or the Incr/Decr
   RefCount macros, or the Tcl_DStringValue macro, or the Tcl_DString `string`
   field) from tcl.json -> S0001 "undeclared identifier" / field-resolve failure ->
   this example stops compiling (or ld.so exit 127 if only the Db* symbol is cut). */

static int RefcCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *const objv[]) {
    /* (1) refcount round-trip over the EXPORTED Db* functions. */
    Tcl_Obj *o = Tcl_NewIntObj(40);              /* refCount 0 */
    Tcl_IncrRefCount(o);                         /* Tcl_DbIncrRefCount(o,"",0): refCount 1 */
    int base = 0;
    if (Tcl_GetIntFromObj(ip, o, &base) != 0) return 1;  /* base = 40 while pinned */
    Tcl_DecrRefCount(o);                         /* Tcl_DbDecrRefCount(o,"",0): refCount 0 -> freed */

    /* (2) Tcl_DString `string`-field read via the Tcl_DStringValue macro. */
    Tcl_DString ds;
    Tcl_DStringInit(&ds);                        /* string -> staticSpace, "" */
    Tcl_DStringAppendElement(&ds, "2");          /* ds now holds the element "2" */
    char *s = Tcl_DStringValue(&ds);             /* macro -> ((&ds)->string), points at "2" */
    int add = s[0] - '0';                        /* '2' - '0' == 2 (a mis-read field != 2) */
    Tcl_DStringFree(&ds);

    Tcl_SetObjResult(ip, Tcl_NewIntObj(base + add));     /* 40 + 2 = 42 */
    return 0;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "refc", RefcCmd, 0, 0);
    if (Tcl_Eval(ip, "refc") != 0) return 2;
    int r = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &r) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-refcount-ok");
    return r;   /* 42, computed by DSS code called back from libtcl */
}
