#include <tcl.h>
#include <stdio.h>

/* D-FFI-TCL-DESCRIPTOR (SQLite testfixture arc, C15 2026-07-18): the RUN witness
   for the Tcl BYTE-ARRAY signature ABI fix in tcl.json. The real Tcl 8.6 API is
   `unsigned char *Tcl_GetByteArrayFromObj(Tcl_Obj*, int*)` and
   `Tcl_Obj *Tcl_NewByteArrayObj(const unsigned char *bytes, int len)` — the byte
   pointers are `unsigned char *` (u8*), NOT `char *`. tcl.json previously modeled
   both as `ptr<char>`, so every sqlite test-TU that passes/receives an
   `unsigned char *` byte buffer (test5/test_blob/test_hexio: `(u8*)bytes`, a
   `unsigned char *zBuf`, `unsigned char *zBlob`, `unsigned char *aOut`) hit S0003
   (char* vs unsigned char* is a strict pointer-type mismatch, correctly fail-loud).
   C15 corrects both signatures to `ptr<u8>` — matching the ABI — and the three TUs
   go fully CLEAN (29->32).

   BytesCmd is a C command libtcl CALLS BACK on the eval:
     - it builds a byte array from an `unsigned char[]` buffer via Tcl_NewByteArrayObj
       (the FIRST param is now ptr<u8>, so passing `buf` [unsigned char*] type-checks);
     - it reads the bytes back via Tcl_GetByteArrayFromObj, whose RETURN is now ptr<u8>,
       assigned to an `unsigned char *back` with NO cast (was a char*->u8* S0003);
     - sums back[0]+back[1] = 40+2 = 42, on bytes libtcl round-tripped through its
       own ByteArray Tcl_Obj — value-divergent (a broken binding never yields 42).

   RED-ON-DISABLE: revert tcl.json's two byte-array signatures to `ptr<char>` ->
   BOTH the `Tcl_NewByteArrayObj(buf, 2)` arg (u8* -> char* param) AND the
   `unsigned char *back = Tcl_GetByteArrayFromObj(...)` return (char* -> u8* var)
   raise S0003 -> this example stops compiling. elf-x86_64 only (tcl.json
   availableObjectFormats:[elf]); the CI ubuntu x86_64 legs run it (libtcl8.6
   installed). */

static int BytesCmd(void *cd, Tcl_Interp *ip, int objc, Tcl_Obj *const objv[]) {
    unsigned char buf[3];
    buf[0] = 40;
    buf[1] = 2;
    buf[2] = 0;
    /* ptr<u8> param: `buf` decays to unsigned char* and type-checks without a cast. */
    Tcl_Obj *o = Tcl_NewByteArrayObj(buf, 2);
    int n = 0;
    /* ptr<u8> return: assigned to `unsigned char *back` with NO cast (was S0003). */
    unsigned char *back = Tcl_GetByteArrayFromObj(o, &n);
    if (n != 2) return 1;
    int sum = (int)back[0] + (int)back[1];   /* 40 + 2 = 42 */
    Tcl_SetObjResult(ip, Tcl_NewIntObj(sum));
    return 0;
}

int main(void) {
    Tcl_Interp *ip = Tcl_CreateInterp();
    if (ip == 0) return 1;
    Tcl_CreateObjCommand(ip, "bytes", BytesCmd, 0, 0);
    if (Tcl_Eval(ip, "bytes") != 0) return 2;
    int r = 0;
    if (Tcl_GetIntFromObj(ip, Tcl_GetObjResult(ip), &r) != 0) return 3;
    Tcl_DeleteInterp(ip);
    puts("tcl-bytearray-ok");
    return r;   /* 42, computed by DSS code called back from libtcl over ByteArray bytes */
}
