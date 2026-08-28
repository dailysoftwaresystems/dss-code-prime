/* c114 (D-WIN64-PDATA-XDATA-UNWIND): the .pdata/.xdata RUNTIME witness.
 *
 * DSS now emits a RUNTIME_FUNCTION (.pdata) + UNWIND_INFO (.xdata) per pe64
 * function and points IMAGE_DATA_DIRECTORY[3] (EXCEPTION) at .pdata. This
 * proves the tables are CORRECT by asking the OS itself: RtlLookupFunctionEntry
 * binary-searches .pdata for the RUNTIME_FUNCTION covering a given code address
 * and returns it (+ the image base). A missing/malformed .pdata → it returns
 * NULL; a wrong BeginAddress → the entry names the wrong function.
 *
 * main takes the address of `target_fn`, then asks the OS to find target_fn's
 * unwind entry and checks the entry's BeginAddress RVA equals target_fn's own
 * RVA (pc - imageBase). Exit 42 iff the OS found the right entry.
 *   90 target_fn miscompiled · 91 no entry found (.pdata missing/broken) ·
 *   92 image base out-param not filled · 93 entry names the WRONG function.
 *
 * pe64-ONLY: RtlLookupFunctionEntry + the .pdata unwind format are Windows-x64
 * (windows.json is availableObjectFormats:["pe"]). RED-on-disable: drop the
 * pe.cpp .pdata/.xdata emission -> RtlLookupFunctionEntry returns NULL -> 91.
 */
#include <windows.h>

typedef unsigned long long u64;
typedef unsigned int u32;

/* The function whose unwind entry we look up. Address-taken (forces an
 * out-of-line copy that carries its own .pdata entry) AND called (so it is
 * genuinely emitted with a real frame). */
static int target_fn(int x) {
    int a = x + 1;
    int b = a * 2;
    return a + b;
}

int main(void) {
    u64 base = 0;
    u64 pc = (u64)(void *)&target_fn;   /* target_fn's runtime code address */

    if (target_fn(3) != 12) {           /* 4 + 8 = 12 — keep it live + correct */
        return 90;
    }
    void *rf = RtlLookupFunctionEntry(pc, &base, (void *)0);
    if (!rf) {
        return 91;                      /* no RUNTIME_FUNCTION covers target_fn */
    }
    if (base == 0) {
        return 92;                      /* image-base out-param not written */
    }
    u32 beginRva = *(u32 *)rf;          /* RUNTIME_FUNCTION.BeginAddress (first field) */
    u64 expected = pc - base;           /* target_fn's RVA */
    if ((u64)beginRva != expected) {
        return 93;                      /* the entry names a different function */
    }
    return 42;
}
