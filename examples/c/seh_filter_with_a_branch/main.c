/* D-MIR-TRY-FILTER-WITH-A-BRANCH-REFUSED — a `__try` FILTER WHOSE EXPRESSION HAS A
 * BRANCH.
 *
 * `&&`, `||` and `?:` evaluate one of their operands only on one path, so a
 * filter written with any of them is more than one basic block. A filter is
 * compiled as a function of its own, entered by the system while it searches
 * for a handler; the whole of its expression — every path of it — belongs to
 * that function, and the function that holds the `__try` keeps none of it.
 *
 * The cases, and the exit code that names the first one to fail:
 *   11  `&&` whose right operand reads a global
 *   12  `?:` choosing between EXCEPTION_EXECUTE_HANDLER and
 *       EXCEPTION_CONTINUE_SEARCH — the shape the vendor's documentation writes
 *   13  `||` whose left operand is false
 *   14  `&&` whose right operand reads a LOCAL of the function
 *   15  the false path is really taken: an inner filter declines through its
 *       `?:` and the outer region's handler runs, the inner one's does not
 *   16  the right operand of `&&` is not evaluated when the left one is false
 *       (a call that counts itself), and is evaluated once when it is true
 *   17  branches inside branches, with a call that takes the exception record
 *   18  the handler names a global only the filter named before it
 *   19  a region with a branching filter inside a loop, faulting every turn
 *   20  the right operand reads a global the guarded body changes before it
 *       faults: the filter must see the value at the fault
 *   21  the conditional answers EXCEPTION_CONTINUE_EXECUTION (-1) on its taken
 *       arm, after its left operand committed the reserved page the guarded
 *       body read: the faulting read runs again and reads the page's zero, and
 *       the handler does not run (the demand-commit shape of the vendor's
 *       documentation, written with a branch)
 *   22  the value of each filter is COMPUTED on the arm taken, neither arm a
 *       constant: the inner filter answers `g_disposition - 1` (0, it declines)
 *       and the outer one `g_disposition` (1, its handler runs)
 *   10  VirtualAlloc failed (the environment, not the compiler)
 *   42  every case answered right
 * A crash instead of an exit code means a filter declined a fault it should
 * have accepted, and no region outside it took the fault.
 *
 * ✔MEASURED before the fix, on pe64, baseline and release: the file of cases
 * 11..20 was REFUSED, once for each of its twelve regions (`error[I_SehStructure]: …
 * filter block #N must terminate in SehFilterReturn`) — the region's skeleton
 * took a filter to be one basic block. The reference compiler (cl 19.51 x64,
 * /Od and /O2) runs this file to 42.
 *
 * pe64 only: `__try`, the handler routine and <windows.h> are Windows.
 */
#include <windows.h>

#define IS_ACCESS_VIOLATION (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION)

int g_want;
int g_zero;
int g_calls;
int g_shared;
int g_changes;
int g_inner_ran;
int g_disposition;

static int counts_itself(void) {
    g_calls = g_calls + 1;
    return 1;
}

/* Commits the page `q` reserved: the filter's way to make the faulting read
 * succeed when it runs again. */
static int commits(void *q) {
    g_calls = g_calls + 1;
    return VirtualAlloc(q, 4096, MEM_COMMIT, PAGE_READWRITE) != 0;
}

static int record_says_access_violation(EXCEPTION_POINTERS *info) {
    return info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION;
}

static int and_reads_a_global(void *p) {
    int rc = 0;
    g_want = 1;
    __try {
        rc = *(volatile int *)p;
    } __except (IS_ACCESS_VIOLATION && g_want == 1) {
        rc = 42;
    }
    return rc;
}

static int conditional_chooses_the_disposition(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (IS_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        rc = 42;
    }
    return rc;
}

static int or_whose_left_is_false(void *p) {
    int rc = 0;
    g_want = 1;
    __try {
        rc = *(volatile int *)p;
    } __except (g_want == 7 || IS_ACCESS_VIOLATION) {
        rc = 42;
    }
    return rc;
}

static int and_reads_a_local(void *p) {
    int rc = 0;
    int want = 3;
    __try {
        rc = *(volatile int *)p;
    } __except (IS_ACCESS_VIOLATION && want == 3) {
        rc = 39 + want;
    }
    return rc;
}

static int inner_declines_and_outer_takes_it(void *p) {
    int rc = 0;
    g_want = 1;
    g_inner_ran = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (g_want == 99 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
            g_inner_ran = 1;
            rc = 1;
        }
    } __except (IS_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        rc = 42;
    }
    if (g_inner_ran != 0) return 2;
    return rc;
}

static int right_operand_runs_only_when_the_left_holds(void *p) {
    int rc = 0;
    g_zero = 0;
    g_calls = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (g_zero != 0 && counts_itself()) {
            rc = 1;
        }
    } __except (IS_ACCESS_VIOLATION && counts_itself()) {
        rc = 41;
    }
    /* The system asks each filter once while it searches: the inner one stops
     * at its left operand, the outer one calls. */
    return rc + g_calls;
}

static int branches_inside_branches(void *p) {
    int rc = 0;
    g_want = 1;
    __try {
        rc = *(volatile int *)p;
    } __except ((record_says_access_violation(GetExceptionInformation()) && g_want != 0)
                    ? (g_want == 1 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
                    : EXCEPTION_CONTINUE_SEARCH) {
        rc = 42;
    }
    return rc;
}

static int handler_names_what_the_filter_named(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (g_shared == 5 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        rc = g_shared + 37;
    }
    return rc;
}

static int region_in_a_loop(void *p) {
    int sum = 0;
    g_want = 1;
    for (int i = 0; i < 3; i = i + 1) {
        __try {
            sum = sum + *(volatile int *)p;
        } __except (IS_ACCESS_VIOLATION && g_want == 1) {
            sum = sum + 14;
        }
    }
    return sum;
}

static int filter_sees_the_body_s_store(void *p) {
    int rc = 0;
    g_changes = 1;
    __try {
        g_changes = 2;
        rc = *(volatile int *)p;
    } __except (IS_ACCESS_VIOLATION && g_changes == 2) {
        rc = 42;
    }
    return rc;
}

static int conditional_answers_continue_execution(void *q) {
    int rc = 7;
    g_calls = 0;
    __try {
        rc = *(volatile int *)q;
    } __except ((IS_ACCESS_VIOLATION && commits(q)) ? EXCEPTION_CONTINUE_EXECUTION
                                                    : EXCEPTION_CONTINUE_SEARCH) {
        rc = 1;
    }
    /* The read ran again and read 0, the filter was asked once: 0 + 41 + 1. */
    return rc + 41 + g_calls;
}

static int computed_disposition(void *p) {
    int rc = 0;
    g_disposition = 1;
    g_inner_ran = 0;
    __try {
        __try {
            rc = *(volatile int *)p;
        } __except (IS_ACCESS_VIOLATION ? g_disposition - 1 : EXCEPTION_EXECUTE_HANDLER) {
            g_inner_ran = 1;
            rc = 1;
        }
    } __except (IS_ACCESS_VIOLATION ? g_disposition : EXCEPTION_CONTINUE_SEARCH) {
        rc = 42;
    }
    if (g_inner_ran != 0) return 2;
    return rc;
}

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    /* Reserved and not committed: a read faults until a filter commits it. */
    void *q = VirtualAlloc(0, 4096, MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0 || q == 0) {
        return 10;
    }
    g_shared = 5;
    if (and_reads_a_global(p) != 42) return 11;
    if (conditional_chooses_the_disposition(p) != 42) return 12;
    if (or_whose_left_is_false(p) != 42) return 13;
    if (and_reads_a_local(p) != 42) return 14;
    if (inner_declines_and_outer_takes_it(p) != 42) return 15;
    if (right_operand_runs_only_when_the_left_holds(p) != 42) return 16;
    if (branches_inside_branches(p) != 42) return 17;
    if (handler_names_what_the_filter_named(p) != 42) return 18;
    if (region_in_a_loop(p) != 42) return 19;
    if (filter_sees_the_body_s_store(p) != 42) return 20;
    if (conditional_answers_continue_execution(q) != 42) return 21;
    if (computed_disposition(p) != 42) return 22;
    return 42;
}
