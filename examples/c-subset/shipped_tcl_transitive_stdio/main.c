#include <tcl.h>

/* D-FFI-DESCRIPTOR-INCLUDES (SQLite testfixture arc, C32 2026-07-20): the RUN
   witness for the TRANSITIVE shipped-header `#include` graph.

   There is DELIBERATELY no direct `#include <stdio.h>` here. The entire stdio
   surface this TU uses — the FILE typedef, fopen/fwrite/fread/fseek/fclose/remove,
   and the SEEK_SET constant — arrives ONLY through tcl.json's
   `"includes": ["stdio.h"]` transitive edge: real /usr/include/tcl8.6/tcl.h
   `#include`s <stdio.h>, so a TU that reaches tcl.json via `#include <tcl.h>`
   (exactly the sqlite src/test_md5.c → tclsqlite.h → <tcl.h> path) ALSO gets
   stdio.json's surface injected. DSS resolves the tcl.h descriptor, walks its
   `includes` closure, and injects stdio.json's symbols/typedefs/constants
   alongside tcl's — the flat-descriptor FILE §B fix.

   The value 42 is reconstructed from two bytes round-tripped through a REAL file
   (fwrite then fread), so no optimizer pass can fold it away — a genuine
   end-to-end exercise of the transitively-injected stdio surface, not a constant
   the release pipeline could precompute. Tcl_CreateInterp/DeleteInterp exercise
   the PARENT (tcl.h) descriptor in the same TU, proving the parent and its
   transitive child coexist (and grounding the NEEDED libtcl8.6.so DT_NEEDED).

   RED-ON-DISABLE: remove tcl.json's `"includes"` field -> the FILE/fopen/... names
   are no longer injected (tcl.json alone does not carry them) -> this TU fails to
   compile with S0001 on `FILE *f;` and the fopen/fread/fseek/fclose cascade.

   ELF-ONLY: tcl.json is availableObjectFormats:[elf], so a pe64/macho target
   fails the <tcl.h> availability gate at compile (loud) — mirrors every other
   shipped_tcl_* witness; only x86_64-linux RUNS (libtcl present). */

int main(void) {
    Tcl_Interp *interp;
    FILE *f;                       /* FILE — via tcl.h -> stdio.h (NO direct include) */
    unsigned char wbuf[2];
    unsigned char rbuf[2];
    size_t n;                      /* size_t — rides stdio.h too */
    int a;
    int b;

    /* The PARENT descriptor (tcl.h) still works alongside the transitive edge. */
    interp = Tcl_CreateInterp();
    if (interp == 0) return 1;
    Tcl_DeleteInterp(interp);

    /* Write two bytes whose values sum to 42 through a real file. */
    wbuf[0] = 40;
    wbuf[1] = 2;
    f = fopen("dss_c32_transitive.tmp", "wb");   /* fopen — transitive */
    if (f == 0) return 2;
    if (fwrite(wbuf, 1, 2, f) != 2) return 3;    /* fwrite — transitive */
    if (fclose(f) != 0) return 4;                /* fclose — transitive */

    /* Read them back; fseek to the start with SEEK_SET exercises the constant. */
    f = fopen("dss_c32_transitive.tmp", "rb");
    if (f == 0) return 5;
    if (fseek(f, 0, SEEK_SET) != 0) return 6;    /* fseek + SEEK_SET — transitive */
    n = fread(rbuf, 1, 2, f);                    /* fread — transitive */
    if (fclose(f) != 0) return 7;
    remove("dss_c32_transitive.tmp");            /* remove — transitive */

    if (n != 2) return 8;
    a = rbuf[0];                                 /* 40, read back from the file */
    b = rbuf[1];                                 /* 2 */
    return a + b;                                /* 42 — reconstructed from the file */
}
