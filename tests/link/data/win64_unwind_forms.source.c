/* SOURCE of the committed fixtures `tests/link/data/win64_unwind_forms_x86_64_pe_<compiler>.obj` (P69 fold 3c, lane
 * p69/xa; D-LK-COFF-FOREIGN-UNWIND-DROP).
 *
 * WHAT IT IS. One function per frame shape whose x64 unwind information the reference compilers write differently:
 * a leaf, a stack buffer beside a call (the GS cookie and its handler), a frame past one page (UWOP_ALLOC_LARGE), an
 * `_alloca` frame (a frame register, set 32 bytes above the stack pointer by cl and gcc), many callee-saved general
 * and vector registers live across calls (pushes, home-area saves and UWOP_SAVE_XMM128), an early return before the
 * frame, and a `__try` beside a GS buffer (`__GSHandlerCheck_SEH`: a scope table and a cookie offset in one handler's
 * data). DSS reads each object's `.pdata`/`.xdata` into its functions and writes them back; the tests compare the two
 * byte for byte (`tests/link/test_coff_foreign_unwind.cpp`). These objects are READ, not linked: the GS and probe
 * routines they name (`__security_check_cookie`, `__chkstk`, `___chkstk_ms`) are the platform runtime's.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a directory holding a copy of this file:
 *   cl /nologo /c /O2 /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fowin64_unwind_forms_x86_64_pe_cl_O2.obj win64_unwind_forms.source.c
 *   cl /nologo /c /Od /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fowin64_unwind_forms_x86_64_pe_cl_Od.obj win64_unwind_forms.source.c
 *   clang-cl /c /O2 /Brepro /Fowin64_unwind_forms_x86_64_pe_clangcl_O2.obj win64_unwind_forms.source.c
 *   gcc -O2 -c win64_unwind_forms.source.c -o win64_unwind_forms_x86_64_pe_gcc_O2.obj
 * (gcc is mingw-w64's; it has no `__try`, and the source leaves `trygs` out for it.) `/experimental:deterministic
 * /pathmap:` keeps the builder's directory out of cl's object, and `/Brepro` the time out of clang-cl's (the reasons
 * are measured in `libpe_directive_stack_x86_64_pe.source.c`).
 * Each object's md5, and the compilers that wrote it, are recorded where the fixtures are read; no guard enforces them.
 */
#include <malloc.h>

void   sink(char *);
double dsink(double);

int leaf(int x) { return x * 3 + 1; }

int gsbuf(int n) {
    char b[64];
    sink(b);
    return b[n];
}

int big(int n) {
    char b[70000];
    sink(b);
    return b[n];
}

int dyn(int n) {
    char *b = (char *)_alloca(n);
    sink(b);
    return b[0];
}

double regs(double a, double b, double c, int *p) {
    int    x = p[0], y = p[1], z = p[2], w = p[3], v = p[4], u = p[5];
    double d = dsink(a);
    double e = dsink(b);
    double f = dsink(c);
    sink((char *)p);
    return d * a + e * b + f * c + x + y + z + w + v + u + dsink(d + e + f);
}

int shrink(int *p) {
    if (!p) return 0;
    char b[64];
    sink(b);
    return b[*p];
}

#ifdef _MSC_VER
int trygs(int volatile *p, int n) {
    char b[64];
    int  r = 0;
    sink(b);
    __try {
        r = *p + b[n];
    } __except (1) {
        r = -1;
    }
    return r;
}
#endif
