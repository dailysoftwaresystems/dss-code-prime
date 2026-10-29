/* D-MIR-TRY-HANDLER-SHARED-FILTER-VALUE-ABORTS-IN-RELEASE — a `__try` HANDLER
 * THAT NAMES WHAT ITS FILTER NAMED.
 *
 * `__except (g == 5) { rc = g + 37; }` shares nothing in the source: the filter
 * is an expression, the handler a statement. An optimized build shares one
 * computation of `g`'s address between them — and the filter is compiled as a
 * function of its own, entered by the system while it searches for a handler,
 * so nothing it computed is in the function that holds the `__try`. That
 * function computes the address again when its handler starts.
 *
 * The cases, and the exit code that names the first one to fail:
 *   11  the handler reads the global its filter read
 *   12  the handler reads the array element its filter read (an address plus
 *       an offset: a value computed FROM another)
 *   13  both directions in one region: the filter reads a global the function
 *       wrote before the region (the filter computes that address again) and
 *       the handler reads the global the filter read (the function does)
 *   14  the same through array elements
 *   15  a region inside the handler: its filter and its handler both name the
 *       global the OUTER filter named
 *   10  VirtualAlloc failed (the environment, not the compiler)
 *   42  every case answered right
 * A crash instead of an exit code means a filter declined a fault it should
 * have accepted.
 *
 * ✔MEASURED before the fix, on pe64: the baseline compiled this file and the
 * RELEASE build ABORTED THE COMPILER (`MirFunctionRebuilder fatal
 * [pass=SynthSehParent]: rewriteOperand: … has no rewrite entry`). The
 * reference compiler (cl 19.51 x64, /Od and /O2) runs it to 42.
 *
 * pe64 only: `__try`, the handler routine and <windows.h> are Windows.
 */
#include <windows.h>

int g_shared;
int g_table[4];
int g_before;
int g_filter;
int g_row_before[4];
int g_row_filter[4];
int g_outer;

/* 11 */
static int handler_names_what_the_filter_named(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (g_shared == 5) {
        rc = g_shared + 37;
    }
    return rc;
}

/* 12 */
static int handler_names_the_element_the_filter_named(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (g_table[2] == 40) {
        rc = g_table[2] + 2;
    }
    return rc;
}

/* 13 */
static int both_directions(void *p) {
    int rc = 0;
    g_before = 3;
    __try {
        rc = *(volatile int *)p;
    } __except ((g_before == 3) & (g_filter == 5)) {
        rc = g_filter + g_before + 34;
    }
    return rc;
}

/* 14 */
static int both_directions_through_an_element(void *p) {
    int rc = 0;
    g_row_before[1] = 3;
    __try {
        rc = *(volatile int *)p;
    } __except ((g_row_before[1] == 3) & (g_row_filter[3] == 5)) {
        rc = g_row_filter[3] + g_row_before[1] + 34;
    }
    return rc;
}

/* 15 */
static int region_inside_the_handler(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (g_outer == 5) {
        __try {
            rc = *(volatile int *)p;
        } __except (g_outer == 5) {
            rc = g_outer + 37;
        }
    }
    return rc;
}

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }
    g_shared = 5;
    g_table[2] = 40;
    g_filter = 5;
    g_row_filter[3] = 5;
    g_outer = 5;
    if (handler_names_what_the_filter_named(p) != 42) return 11;
    if (handler_names_the_element_the_filter_named(p) != 42) return 12;
    if (both_directions(p) != 42) return 13;
    if (both_directions_through_an_element(p) != 42) return 14;
    if (region_inside_the_handler(p) != 42) return 15;
    return 42;
}
