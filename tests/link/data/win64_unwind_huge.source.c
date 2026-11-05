/* SOURCE of the committed fixtures `tests/link/data/win64_unwind_huge_x86_64_pe_<compiler>.obj` (P69 fold 3d, lane
 * p69/xa; D-WIN64-HUGE-FRAME-ALLOC).
 *
 * WHAT IT IS. Three frames past the reach of the short unwind codes, each with registers live across a call: `huge`
 * allocates 700000 bytes (past the 512 KiB - 8 of UWOP_ALLOC_LARGE op-info 0), `hugeregs` keeps seven ints across the
 * call in the same frame (cl /O2 saves some in its caller's home slots, past 512 KiB: UWOP_SAVE_NONVOL_FAR), and
 * `hugexmm` keeps three doubles across the call in a 1200000-byte frame (vector saves past 1 MiB:
 * UWOP_SAVE_XMM128_FAR). Each calls `reach`, which the program linking the object defines. DSS reads each object's
 * `.pdata`/`.xdata` into its functions and writes them back; the tests compare the two byte for byte
 * (`tests/link/test_coff_foreign_unwind.cpp`). DSS also compiles THIS FILE as C and runs it on Windows x86_64 beside a
 * `main` whose `reach` reads through a null pointer inside a `__try`: 42 is each fault dispatched through the frame
 * past 512 KiB to its caller's `__except`. The objects are READ, and a link of each is refused by name: the routines
 * they name -- the probes `__chkstk` / `___chkstk_ms`, and cl's and clang-cl's /GS family -- are the platform runtime's.
 *
 * REBUILD, in an x64 Visual Studio developer prompt (`vcvars64.bat`), in a directory holding a copy of this file:
 *   cl /nologo /c /O2 /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fowin64_unwind_huge_x86_64_pe_cl_O2.obj win64_unwind_huge.source.c
 *   cl /nologo /c /Od /experimental:deterministic /pathmap:%CD%=X:\dss-fixture /Fowin64_unwind_huge_x86_64_pe_cl_Od.obj win64_unwind_huge.source.c
 *   clang-cl /c /O2 /Brepro /Fowin64_unwind_huge_x86_64_pe_clangcl_O2.obj win64_unwind_huge.source.c
 *   gcc -O2 -c win64_unwind_huge.source.c -o win64_unwind_huge_x86_64_pe_gcc_O2.obj
 * (gcc is mingw-w64's.) Each object's md5, and the compilers that wrote it, are recorded where the fixtures are read;
 * no guard enforces them.
 */
int    reach(int volatile *p, char *b);
int    use(int x);
double dsink(double x);

int huge(int volatile *p) {
    char b[700000];
    b[0] = 0;
    b[1] = 0;
    int r = reach(p, b);
    return r + b[1];
}

int hugeregs(int volatile *p, int const *v) {
    char b[700000];
    int  x = v[0], y = v[1], z = v[2], w = v[3], s = v[4], u = v[5], t = v[6];
    b[0]  = 0;
    b[1]  = 0;
    int r = reach(p, b);
    return r + use(x) + use(y) + use(z) + use(w) + use(s) + use(u) + use(t) + b[1];
}

int hugexmm(int volatile *p, double a, double b, double c) {
    char   buf[1200000];
    double d = dsink(a), e = dsink(b), f = dsink(c);
    buf[0]   = 0;
    buf[1]   = 0;
    int r    = reach(p, buf);
    return r + (int)(d * a + e * b + f * c + dsink(d + e + f)) + buf[1];
}
