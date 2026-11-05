/* D-MIR-TRY-FILTER-SHARED-PURE-VALUE-REFUSED-IN-RELEASE — a `__try` FILTER that
 * reads something the function also computed before the region.
 *
 * A filter is compiled as a function of its own, entered by the system while it
 * searches for a handler: it has the exception and the parent's frame, and none
 * of the parent's registers. An optimized build shares one computation between
 * the code before the region and the filter — the address of a global both name,
 * the address of one of its elements — and the shared copy lives in the parent.
 *
 * ✔MEASURED 2026-10-08 on pe64 before the fix: this shape ran to 42 at baseline
 * and was REFUSED in release ("references a value defined outside the filter
 * block that is not a recoverable parent local"). The filter now computes such a
 * value again, where the source wrote it. The reference compiler (cl 19.51 x64,
 * /Od and /O2) runs this file to 42.
 *
 * The cases, and the exit code that names the first one to fail:
 *   11  the filter reads a GLOBAL the function wrote before the region
 *   12  the filter reads an ARRAY ELEMENT the function wrote before the region
 *       (the element's address is an address computed from another)
 *   13  the filter reads a global the guarded BODY changes before it faults:
 *       it must see the value at the fault, not the one from before the region
 *   10  VirtualAlloc failed (the environment, not the compiler)
 *   42  every case answered right
 * A crash instead of an exit code means a filter declined a fault it should
 * have accepted.
 *
 * `&`, not `&&`: a filter with a branch of its own is another row's.
 * pe64 only: `__try`, the handler routine and <windows.h> are Windows.
 */
#include <windows.h>

#define ACCESS_VIOLATION_ONLY (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION)

int g_want;
int g_slots[4];
int g_changes;

static int filter_reads_a_global(void *p) {
    int rc = 0;
    g_want = 1;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY & (g_want == 1)) {
        rc = 42;
    }
    return rc;
}

static int filter_reads_an_array_element(void *p) {
    int rc = 0;
    g_slots[2] = 40;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY & (g_slots[2] == 40)) {
        rc = g_slots[2] + 2;
    }
    return rc;
}

static int filter_sees_the_body_s_store(void *p) {
    int rc = 0;
    g_changes = 1;
    __try {
        g_changes = 2;
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY & (g_changes == 2)) {
        rc = 42;
    }
    return rc;
}

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }
    if (filter_reads_a_global(p) != 42) return 11;
    if (filter_reads_an_array_element(p) != 42) return 12;
    if (filter_sees_the_body_s_store(p) != 42) return 13;
    return 42;
}
