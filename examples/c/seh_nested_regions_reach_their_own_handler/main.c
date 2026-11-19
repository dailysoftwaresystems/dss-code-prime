/* D-MIR-NESTED-TRY-REGIONS-REACH-THE-OUTER-HANDLER — which handler a fault reaches
 * when `__try` regions nest.
 *
 * The system's handler routine walks a function's scope table in order and
 * gives a fault to the FIRST record whose range holds the faulting address and
 * whose filter accepts it. An outer region's range holds every address of the
 * regions inside it, so the table must list a region after every region inside
 * it, and each region's range must hold exactly its own body.
 *
 * ✔MEASURED 2026-10-08 on pe64 before the fix, each case below built as a
 * program of its own, baseline and release. Case 11 answered 1000, case 16
 * answered 12 and case 17 answered 40: in each the OUTER handler ran for a fault
 * in the INNER body, because the image's table listed the outer range first.
 * Cases 13, 14 and 15 answered garbage — their outer handler reads a parameter,
 * which is the other row's defect on top of this one. Case 12 answered 42. The
 * reference compiler (cl 19.51 x64, /Od and /O2) runs this file to 42, and its
 * own table for two nested regions reads `[+20,+32) [+20,+57)`: inner first, the
 * outer range holding the inner handler.
 *
 * The cases, and the exit code that names the first one to fail:
 *   11  two deep, the fault in the INNER body          — the inner handler's
 *   12  two deep, the fault in the OUTER body, after the inner region
 *   13  three deep, one fault in each body, innermost first
 *   14  two SIBLINGS inside one parent, a fault in each, then one in the parent
 *   15  the inner filter DECLINES the fault: the outer handler runs, and reads
 *       a parameter born before both regions
 *   16  the inner body holds a LOOP and faults inside it; then the outer body
 *       faults after the inner region
 *   17  a fault raised by the INNER HANDLER is the outer handler's
 *   10  VirtualAlloc failed (the environment, not the compiler)
 *   42  every case answered right
 *
 * Case 15 is where this row meets the one about what a handler reads
 * (D-LIR-NO-EXCEPTIONAL-EDGE-INTO-A-TRY-HANDLER): the outer region's run holds
 * the inner body, the inner filter's stub and the inner handler, so the
 * parameter the outer handler reads is kept through all of them.
 *
 * pe64 only: `__try`, the handler routine and <windows.h> are Windows.
 */
#include <windows.h>

#define ACCESS_VIOLATION_ONLY (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION)

static int inner_fault(void *p) {
    int rc = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += 2;
        }
        rc += 40;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 1000;
    }
    return rc;
}

static int outer_fault(void *p) {
    int rc = 0;
    int safe = 5;
    __try {
        __try {
            rc = *(volatile int *)&safe;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += 1000;
        }
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 37;
    }
    return rc;
}

static int three_deep(void *p, int a) {
    int rc = 0;
    __try {
        __try {
            __try {
                rc = *(volatile int *)p;
            } __except (ACCESS_VIOLATION_ONLY) {
                rc += 1;
            }
            rc += *(volatile int *)p;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += 10;
        }
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 100 + a;
    }
    return rc;
}

static int two_siblings(void *p, int a) {
    int rc = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += 1;
        }
        __try {
            rc += *(volatile int *)p;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += 10;
        }
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 100 + a;
    }
    return rc;
}

static int inner_filter_declines(void *p, int outer) {
    int rc = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO) {
            rc = 1000;
        }
        rc += 500;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += outer;
    }
    return rc;
}

static int loop_in_the_inner_body(void *p, int n) {
    int rc = 0;
    int safe = 0;
    int entered = 0;
    volatile int *q = (volatile int *)p;
    __try {
        __try {
            for (int i = 0; i < n; ++i) {
                if (i == 2) {
                    rc += *q;
                }
                rc += 1;
            }
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += 30;
            entered += 1;
            if (entered > 1) {
                q = &safe;
                rc = 500;
            }
        }
        rc += *q;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 10;
    }
    return rc;
}

static int fault_in_the_inner_handler(void *p) {
    int rc = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc = 2;
            rc += *(volatile int *)p;
        }
        rc += 1000;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 40;
    }
    return rc;
}

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }
    if (inner_fault(p) != 42) return 11;
    if (outer_fault(p) != 42) return 12;
    if (three_deep(p, -69) != 42) return 13;
    if (two_siblings(p, -69) != 42) return 14;
    if (inner_filter_declines(p, 42) != 42) return 15;
    if (loop_in_the_inner_body(p, 5) != 42) return 16;
    if (fault_in_the_inner_handler(p) != 42) return 17;
    return 42;
}
