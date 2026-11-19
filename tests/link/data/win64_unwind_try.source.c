/* SOURCE of the committed fixtures `tests/link/data/win64_unwind_try_x86_64_pe_<compiler>.obj` (P69 fold 3c, lane
 * p69/xa; D-LK-COFF-FOREIGN-UNWIND-DROP).
 *
 * WHAT IT IS. Two `__try` regions as the reference compilers write them for a C object: `guarded` faults in its own
 * body, `guarded_call` in a function it calls (`deref`, defined by the program that links the object). Each catches
 * the fault in a constant `__except (1)`. cl names `__C_specific_handler` and hands it a scope table: at /O2 the
 * table's filter field holds the constant 1, at /Od it names a filter funclet `cl` writes beside the function. A
 * program linking the object with a `main` that reads through a null pointer runs to 42 when the scope table reached
 * the image (`tests/link/test_coff_foreign_unwind.cpp`, the suite `CoffForeignUnwindNative`).
 *
 * ⚠ clang-cl /O2 DROPS `guarded`'s `__try`: it writes `guarded` with no handler and no scope table (MEASURED P69 fold
 * 3c, clang-cl 19.1.5; INFERRED why: without /EHa clang models a fault only at a call). Its `guarded_call` keeps the
 * region — so a clang-cl object is run through `guarded_call` alone.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a directory holding a copy of this file:
 *   cl /nologo /c /O2 /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fowin64_unwind_try_x86_64_pe_cl_O2.obj win64_unwind_try.source.c
 *   cl /nologo /c /Od /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fowin64_unwind_try_x86_64_pe_cl_Od.obj win64_unwind_try.source.c
 *   clang-cl /c /O2 /Brepro /Fowin64_unwind_try_x86_64_pe_clangcl_O2.obj win64_unwind_try.source.c
 * Each object's md5, and the compilers that wrote it, are recorded where the fixtures are read; no guard enforces them.
 */
int deref(int volatile *p);

int guarded(int volatile *p) {
    int r = 0;
    __try {
        r = *p;
    } __except (1) {
        r = -1;
    }
    return r;
}

int guarded_call(int volatile *p) {
    int r = 0;
    __try {
        r = deref(p);
    } __except (1) {
        r = -1;
    }
    return r;
}
