/* SOURCE of the committed fixtures `tests/link/data/win64_unwind_filter_x86_64_pe_<compiler>.obj` (P69 fold 3d, lane
 * p69/xa; D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION).
 *
 * WHAT IT IS. A `__try` whose `__except` filter is an EXPRESSION, as a C object holds it: `cs_trygs` raises `code` and
 * takes it only when it is 0xE0000002; `cs_trygs_outer` raises 0xE0000003 through `cs_trygs`, whose filter declines
 * it, and catches it in a constant `__except`. cl writes the expression as a filter FUNCLET, `cs_trygs$filt$0` (a
 * STATIC DTYPE_FUNCTION), which `cs_trygs`'s scope table names by ADDR32NB; under /O2, and under /Gy, the funclet's
 * `.text$x` is a COMDAT of selection ASSOCIATIVE to `cs_trygs`'s NODUPLICATES `.text$mn`. A program whose `main`
 * returns `cs_trygs(0xE0000002u) + cs_trygs_outer()` runs to 42 only when the filter runs and decides both ways
 * (`tests/link/test_coff_foreign_unwind.cpp`, `AFilterFunclet...`).
 *
 * REBUILD: built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal),
 * the probe's own directory mapped away so the object records no host path:
 *   cl  /O2 /GS /MD /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture     -> ..._cl_O2.obj
 *   cl  /Od /Gy /GS- /MD (the rest alike)                                                  -> ..._cl_OdGy.obj
 * (cl 19.51, windows-x86_64-release; `/pathmap:` is ignored without `/experimental:deterministic`, cl's D9007.)
 * Each object's md5, and the compiler that wrote it, are recorded where the fixtures are read; no guard enforces them.
 */
#include <windows.h>

__declspec(noinline) int cs_trygs(unsigned code) {
    int r = 0;
    __try {
        RaiseException(code, 0, 0, 0);
        r = 1;
    } __except (GetExceptionCode() == 0xE0000002u ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        r = 40;
    }
    return r;
}

__declspec(noinline) int cs_trygs_outer(void) {
    int r = 0;
    __try {
        r = cs_trygs(0xE0000003u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = 2;
    }
    return r;
}
