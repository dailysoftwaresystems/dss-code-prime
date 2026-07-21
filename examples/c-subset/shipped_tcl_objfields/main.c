#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C34a 2026-07-21): the RUN witness
   for the REAL Tcl_Obj field layout + Option C (D-FFI-DESCRIPTOR-TYPEDEF-NAME-
   RESOLUTION, "define once / reference by name").

   Before C34a, tcl.json modeled Tcl_Obj as an OPAQUE `arr<u64,6>` blob, so ANY
   field access was S000D "member access requires a composite-typed operand" — the
   exact blocker at sqlite src/test1.c:6182-6183 (`pVar->typePtr` and, one level
   deeper, `pVar->typePtr->name`). C34a models the real 5-field 48-byte Tcl_Obj
   { int refCount@0; char *bytes@8; int length@16; const Tcl_ObjType *typePtr@24;
   internalRep@32 (arr<u64,2>, kept opaque) } and the 40-byte Tcl_ObjType
   { const char *name@0; + 4 opaque proc ptrs }, so both reads resolve AND land at
   the ABI offsets libtcl fills (offsetof-verified vs real tcl8.6/tcl.h). Option C
   is what lets Tcl_Obj->typePtr spell `ptr<Tcl_ObjType>` BY NAME instead of
   re-inlining the struct at ~45 sites.

   RUNTIME LAYOUT WITNESS (a wrong Tcl_Obj layout would misread these bytes):
     * a fresh Tcl_NewIntObj(42) has refCount 0; one Tcl_IncrRefCount makes it 1,
       read back through `obj->refCount` @ offset 0;
     * an int Tcl_Obj is TYPED, so `obj->typePtr` @ offset 24 is non-NULL and
       `obj->typePtr->name` (the 2-level access — the test1 shape) is a real string
       ("int"). The exit value 42 is Tcl_GetIntFromObj's readback of the object.
   The interp is created first (Tcl_CreateInterp) so the Tcl library is initialized
   before Tcl_DecrRefCount frees the object — the shipped_tcl_cmdinfo/refcount idiom.

   RED-ON-DISABLE: revert tcl.json's Tcl_Obj typedef body to `arr<u64,6>` -> every
   `obj->...` line below fails to compile (S000D) and this example stops building. */

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;

    Tcl_Obj *obj = Tcl_NewIntObj(42);
    Tcl_IncrRefCount(obj);                    /* refCount 0 -> 1 (macro over Tcl_DbIncrRefCount) */

    if (obj->refCount != 1) return 2;         /* refCount @ 0  — the layout witness */
    if (obj->typePtr == 0) return 3;          /* typePtr  @ 24 — an int obj is typed */
    if (obj->typePtr->name == 0) return 4;    /* typePtr->name — the 2-level test1 read */

    int n = 0;
    if (Tcl_GetIntFromObj(ip, obj, &n) != TCL_OK) return 5;   /* n == 42 */

    Tcl_DecrRefCount(obj);                     /* refCount 1 -> 0 -> freed (needs an interp) */
    Tcl_DeleteInterp(ip);
    puts("tcl-objfields-ok");
    return n;                                  /* 42 */
}
