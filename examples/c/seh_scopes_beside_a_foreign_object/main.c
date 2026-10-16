/* TWO `__try` functions in a program that is LINKED WITH OTHER UNITS (see expected.json): one
   reference-built object named to the link, one archive member the link pulls, and - because the
   program prints - the member of DSS's own runtime that holds `printf`. Each guarded function's
   scope table must still name ITS OWN filter and the personality once every unit's symbols have
   been renumbered into one image.

     guarded_read    faults reading a no-access page; its filter accepts an access violation only.
     guarded_divide  faults dividing by zero; its filter accepts a division by zero only.

   The two filters accept DIFFERENT faults, so a scope table that named the other function's filter
   would decline its own fault and the process would die of it instead of exiting 42. Each
   `__except` body assigns a constant, so nothing but the dispatch decides the answer.

     "read 30 divide 5"  what each region caught, printed (`printf` is the third route into the
                         merge: on this format its body is a member of DSS's runtime archive).
     42  30 (the read was caught) + 5 (the division was caught) + 0 (`dss_read_tentative`, the
         named object's function) + 42 (`dss_archived_entry`, the pulled member's) - 35.
     10  VirtualAlloc failed: the environment, not this program's subject. */
#include <stdio.h>
#include <windows.h>

extern int dss_read_tentative(void); /* tests/link/data/tentative_common_x86_64_pe_msvc.obj */
extern int dss_archived_entry(void); /* the one member of libpe_directive_entry_definition_x86_64_pe.a */

static int guarded_read(void *p) {
    int rc = 0;
    __try {
        rc = *(volatile int *)p;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 30;
    }
    return rc;
}

static int guarded_divide(int by) {
    int rc = 0;
    __try {
        rc = 100 / by;
    } __except (GetExceptionCode() == 0xC0000094u /* the status of an integer division by zero */) {
        rc = 5;
    }
    return rc;
}

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;
    }
    volatile int zero = 0;
    int const read = guarded_read(p);
    int const divided = guarded_divide(zero);
    printf("read %d divide %d\n", read, divided);
    return read + divided + dss_read_tentative() + dss_archived_entry() - 35;
}
