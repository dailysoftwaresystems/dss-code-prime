/* D-LIR-NO-EXCEPTIONAL-EDGE-INTO-A-TRY-HANDLER — what a `__try` HANDLER reads.
 *
 * Nothing runs between a fault and the first instruction of the handler it is
 * delivered to except the system's unwinder, which hands over the frame and the
 * registers a call keeps, and nothing else. So every value the handler reads —
 * or the code after the region reads, once the handler has run — must already
 * be in the frame or in a call-preserved register when the fault happens,
 * wherever in the guarded body that is.
 *
 * ✔MEASURED 2026-10-08 on pe64 before the fix, each case below built as a
 * program of its own, baseline and release. Cases 11, 14, 15, 21, 22, 23 and 24
 * answered garbage: the handler read the register the parameter arrived in, and
 * the unwinder had used it. Case 16 answered 0. Cases 18 and 19 died with an
 * access violation of their own — the handler dereferenced such a register —
 * and case 20 answered garbage at baseline and died in release. Case 17 was
 * right at baseline and died in release, where the global's address is computed
 * once, before the region. Cases 12, 13, 25 and 26 answered 42 then as now: the
 * control, a local, a parameter that arrives on the stack, and a fault inside a
 * callee. The reference compiler (cl 19.51 x64, /Od and /O2) runs this file to
 * 42.
 *
 * The cases, each a function of its own (a function holding a region is never
 * inlined), and the exit code that names the first one to fail:
 *   11  a PARAMETER read by the handler
 *   12  the control: the same function, the handler assigns a constant
 *   13  a LOCAL assigned from a parameter before the region
 *   14  an integer division by zero, caught by its documented name
 *   15  a parameter read AFTER the region, once the handler has run
 *   16  a `double` parameter — the vector register class
 *   17  a GLOBAL written before the region and read by the handler
 *   18  a STRUCT RETURNED after the fault (the result's address is the caller's)
 *   19  a STRUCT PARAMETER, passed by reference under this convention
 *   20  a POINTER parameter the handler passes on to a call
 *   21  a region INSIDE A LOOP: the handler reads a parameter on every turn
 *   22  TWO REGIONS in sequence, each handler reading its own parameter
 *   23, 24  a handler WITH BRANCHES, a parameter read on each arm
 *   25  a parameter passed ON THE STACK
 *   26  the fault raised INSIDE A CALLEE of the guarded body
 *   27  a `double` kept across a CALL inside the region; nothing reads it after
 *   28  the same with the call BEFORE the region
 *   29  the fault unwinds THROUGH a callee that saved every vector register a
 *       call preserves and put its own values in them; the handler reads the
 *       `double` the guarding function kept across that call (1000 would be
 *       a value of the callee's)
 *   30  THREE `double` parameters read by the handler, the body calling first
 *   31  case 29 with a callee whose own call passes one argument on the
 *       stack — an odd count of outgoing stack slots, which used to put its
 *       register-save area 8 bytes off a multiple of 16
 *   32  case 29 with a callee that GUARDS A REGION OF ITS OWN and declines the
 *       fault: the unwind passes through a guarding function's frame
 *   10  VirtualAlloc failed (the environment, not the compiler)
 *   42  every case answered right
 * A crash instead of an exit code is a handler dereferencing a register it was
 * never handed.
 *
 * D-WIN64-XMM-UNWIND-RESTORE — cases 16 and 27..32, the vector register class.
 * A `double` that must survive a call, or reach a handler, is kept in a
 * call-preserved vector register; the function that uses such a register saves
 * it in its prologue, and its unwind information must say where — that is what
 * lets the system hand the register back to a frame further up when a fault
 * unwinds through this one. ✔MEASURED 2026-10-10 on pe64 before the fix, each
 * case a program of its own, baseline and release: 27..32 were all REFUSED at
 * the compile (the image writer had no code for a saved vector register in a
 * function that guards a region), and a function guarding nothing shipped its
 * vector saves with no code at all. The reference compiler states the code in
 * both (cl 19.51 x64 /O2: `SAVE_XMM128 xmm6`).
 *
 * pe64 only: `__try`, the handler routine and <windows.h> are Windows.
 */
#include <windows.h>

#define ACCESS_VIOLATION_ONLY (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION)

struct triple { long long a, b, c; };

static int g_value;

static int param_in_handler(void *p, int caught) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = caught;
    }
    return rc;
}

static int constant_in_handler(void *p, int caught) {
    int rc = 0;
    (void)caught;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = 42;
    }
    return rc;
}

static int local_in_handler(void *p, int caught) {
    int rc = 0;
    int keep = caught;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = keep;
    }
    return rc;
}

static int divide_in_body(int by, int caught) {
    int rc = 0;
    __try {
        rc = 100 / by;
    } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        rc = caught;
    }
    return rc;
}

static int param_after_region(void *p, int caught) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = 40;
    }
    return rc + caught;
}

static int double_in_handler(void *p, double caught) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = (int)caught;
    }
    return rc;
}

static int global_in_handler(void *p, int caught) {
    int rc = 0;
    g_value = caught;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = g_value;
    }
    return rc;
}

static struct triple struct_returned(void *p, int caught) {
    struct triple r;
    r.a = 0;
    r.b = 0;
    r.c = 0;
    __try {
        r.a = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        r.a = caught;
    }
    r.b = 1;
    return r;
}

static int struct_param_in_handler(void *p, struct triple s) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = (int)(s.a + s.b + s.c);
    }
    return rc;
}

static void store_through(int *out, int v) {
    *out = v;
}

static int pointer_passed_on(void *p, int *out, int caught) {
    __try {
        *out = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        store_through(out, caught);
    }
    return *out;
}

static int region_in_a_loop(void *p, int n, int step) {
    int rc = 0;
    for (int i = 0; i < n; ++i) {
        __try {
            rc += *(volatile int *)p;
        } __except (ACCESS_VIOLATION_ONLY) {
            rc += step;
        }
    }
    return rc;
}

static int two_regions(void *p, int first, int second) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = first;
    }
    __try {
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += second;
    }
    return rc;
}

static int handler_with_branches(void *p, int which, int a, int b) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        if (which) {
            rc = a;
        } else {
            rc = b;
        }
    }
    return rc;
}

static int stack_param_in_handler(void *p, int a, int b, int c, int d, int caught) {
    int rc = a + b + c + d;
    __try {
        rc = *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = caught;
    }
    return rc;
}

static int touch(volatile int *q) {
    return *q;
}

static int (*volatile touch_through)(volatile int *) = touch;

static int fault_in_callee(void *p, int caught) {
    int rc = 0;
    __try {
        rc = touch_through((volatile int *)p);
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = caught;
    }
    return rc;
}

/* Called through a pointer the optimizer cannot see through, so each call
 * below is a call in both configurations. */
static double half(double x) {
    return x * 0.5;
}

static double (*volatile half_through)(double) = half;

static int double_across_a_call_in_the_region(void *p, double keep) {
    int rc = 0;
    __try {
        rc = (int)(half_through(4.0) + keep);
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 0;
    }
    return rc;
}

static int double_across_a_call_before_the_region(void *p, double keep) {
    int rc = (int)(half_through(4.0) + keep);
    __try {
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc += 0;
    }
    return rc;
}

/* What the three callees below fill their own registers from: a volatile, so
 * each read is a value of its own that nothing can recompute after a call. */
static volatile double theirs = 1000.0;

/* Guards nothing. It keeps THIRTEEN doubles across a call — three that arrive
 * in registers, ten read from `theirs` — which is more than the ten vector
 * registers a call preserves under this convention, and it only ever CONVERTS
 * them, so it needs no vector register for anything else. Whichever of those
 * registers its caller chose for a `double` of its own, this function saved it
 * and overwrote it before it faults: the caller's handler reads its `double`
 * right only if the system's unwinder put it back, and the unwinder knows where
 * from only by this function's unwind information.
 *
 * ★ THIRTEEN, NOT ONE. A callee that kept a single `double` was given a
 * different register from the one its caller's handler read, and answered 42
 * whatever its unwind information said (✔MEASURED 2026-10-10: with that
 * callee's code for its saved register renamed IN THE IMAGE the program still
 * exited 42; with this callee's, it exits with this case's number). */
static int callee_that_saves_and_faults(void *p, double a, double b, double c) {
    double d0 = theirs, d1 = theirs, d2 = theirs, d3 = theirs, d4 = theirs;
    double d5 = theirs, d6 = theirs, d7 = theirs, d8 = theirs, d9 = theirs;
    int r = (int)half_through(8.0);
    r += (int)a + (int)b + (int)c;
    r += (int)d0 + (int)d1 + (int)d2 + (int)d3 + (int)d4;
    r += (int)d5 + (int)d6 + (int)d7 + (int)d8 + (int)d9;
    return r + *(volatile int *)p;
}

static int (*volatile callee_that_saves_through)(void *, double, double, double) = callee_that_saves_and_faults;

static int unwound_through_a_callee(void *p, double keep) {
    int rc = 0;
    __try {
        rc = callee_that_saves_through(p, 1000.0, 1000.0, 1000.0);
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = (int)keep;
    }
    return rc;
}

static int three_doubles_in_handler(void *p, double a, double b, double c) {
    int rc = 0;
    __try {
        rc = (int)half_through(2.0);
        rc += *(volatile int *)p;
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = (int)(a + b + c);
    }
    return rc;
}

static double sum_of_five(double a, int b, int c, int d, int e) {
    return a + b + c + d + e;
}

static double (*volatile sum_of_five_through)(double, int, int, int, int) = sum_of_five;

/* As `callee_that_saves_and_faults`, but its own call passes a fifth argument on
 * the stack. One stack slot above the 32 bytes every call reserves ends the
 * outgoing-argument area 8 bytes off a multiple of 16, and the register-save
 * area used to begin right there; the unwind information can only say a
 * vector register's slot at a multiple of 16, so the save area now begins at
 * the next one. */
static int callee_with_a_stack_argument(void *p, double a, double b, double c) {
    double d0 = theirs, d1 = theirs, d2 = theirs, d3 = theirs, d4 = theirs;
    double d5 = theirs, d6 = theirs, d7 = theirs, d8 = theirs, d9 = theirs;
    int r = (int)sum_of_five_through(1.0, 2, 3, 4, 5);
    r += (int)a + (int)b + (int)c;
    r += (int)d0 + (int)d1 + (int)d2 + (int)d3 + (int)d4;
    r += (int)d5 + (int)d6 + (int)d7 + (int)d8 + (int)d9;
    return r + *(volatile int *)p;
}

static int (*volatile callee_with_a_stack_argument_through)(void *, double, double, double) =
    callee_with_a_stack_argument;

static int unwound_through_an_odd_frame(void *p, double keep) {
    int rc = 0;
    __try {
        rc = callee_with_a_stack_argument_through(p, 1000.0, 1000.0, 1000.0);
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = (int)keep;
    }
    return rc;
}

/* GUARDS A REGION OF ITS OWN, whose filter declines an access violation. It
 * keeps its thirteen doubles across a call inside that region and then faults
 * there: the search goes on to the caller's handler and the unwind passes
 * THROUGH this frame, so this function's own unwind information has to say
 * where it saved every register it overwrote. 7 would be its own handler. */
static int guarding_callee_that_declines(void *p, double a, double b, double c) {
    int r = 0;
    __try {
        double d0 = theirs, d1 = theirs, d2 = theirs, d3 = theirs, d4 = theirs;
        double d5 = theirs, d6 = theirs, d7 = theirs, d8 = theirs, d9 = theirs;
        r = (int)half_through(8.0);
        r += (int)a + (int)b + (int)c;
        r += (int)d0 + (int)d1 + (int)d2 + (int)d3 + (int)d4;
        r += (int)d5 + (int)d6 + (int)d7 + (int)d8 + (int)d9;
        r += *(volatile int *)p;
    } __except (GetExceptionCode() == EXCEPTION_INT_DIVIDE_BY_ZERO) {
        r = 7;
    }
    return r;
}

static int (*volatile guarding_callee_through)(void *, double, double, double) = guarding_callee_that_declines;

static int unwound_through_a_guarding_callee(void *p, double keep) {
    int rc = 0;
    __try {
        rc = guarding_callee_through(p, 1000.0, 1000.0, 1000.0);
    } __except (ACCESS_VIOLATION_ONLY) {
        rc = (int)keep;
    }
    return rc;
}

static int divide_case(void) {
    volatile int zero = 0;
    return divide_in_body(zero, 42);
}

static int struct_returned_case(void *p) {
    struct triple r = struct_returned(p, 41);
    return (int)(r.a + r.b);
}

static int struct_param_case(void *p) {
    struct triple s;
    s.a = 40;
    s.b = 1;
    s.c = 1;
    return struct_param_in_handler(p, s);
}

static int pointer_case(void *p) {
    int out = 0;
    return pointer_passed_on(p, &out, 42);
}

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }
    if (param_in_handler(p, 42) != 42) return 11;
    if (constant_in_handler(p, 7) != 42) return 12;
    if (local_in_handler(p, 42) != 42) return 13;
    if (divide_case() != 42) return 14;
    if (param_after_region(p, 2) != 42) return 15;
    if (double_in_handler(p, 42.0) != 42) return 16;
    if (global_in_handler(p, 42) != 42) return 17;
    if (struct_returned_case(p) != 42) return 18;
    if (struct_param_case(p) != 42) return 19;
    if (pointer_case(p) != 42) return 20;
    if (region_in_a_loop(p, 6, 7) != 42) return 21;
    if (two_regions(p, 40, 2) != 42) return 22;
    if (handler_with_branches(p, 1, 42, 7) != 42) return 23;
    if (handler_with_branches(p, 0, 7, 42) != 42) return 24;
    if (stack_param_in_handler(p, 1, 2, 3, 4, 42) != 42) return 25;
    if (fault_in_callee(p, 42) != 42) return 26;
    if (double_across_a_call_in_the_region(p, 40.0) != 42) return 27;
    if (double_across_a_call_before_the_region(p, 40.0) != 42) return 28;
    if (unwound_through_a_callee(p, 42.0) != 42) return 29;
    if (three_doubles_in_handler(p, 20.0, 12.0, 10.0) != 42) return 30;
    if (unwound_through_an_odd_frame(p, 42.0) != 42) return 31;
    if (unwound_through_a_guarding_callee(p, 42.0) != 42) return 32;
    return 42;
}
