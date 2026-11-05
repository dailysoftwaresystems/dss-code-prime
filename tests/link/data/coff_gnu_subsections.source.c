/* P69 fold 3d, lane p69/xa: the FOREIGN units of tests/link/test_coff_gnu_subsections.cpp. mingw-w64 gcc names a
 * function's section by what the function is (GNU's `.` subsections of `.text`), and registers a constructor or a
 * destructor in a TABLE of addresses that defines no symbol:
 *
 *   UNIT_COLD  `rare` (cold) in `.text.unlikely`, `often` (hot) in `.text.hot`, `pick` in `.text`, each with its
 *              unwind record in the same-suffixed `.xdata.*` / `.pdata.*`; under -ffunction-sections the subsections
 *              carry the function's name (`.text.unlikely.rare`, `.text.hot.often`) and `pick` its `$` group.
 *              The program of the tests' `main.c` (`ext` doubles its argument; main returns
 *              pick(-3) + pick(4) + 50) exits 43 under gcc + GNU ld.
 *   UNIT_CTOR  two constructors (one at priority 101) adding 1 and 10 to `counter`, two destructors, and a `main`
 *              returning `counter`: 11 under gcc + GNU ld and under link.exe. gcc writes the tables as `.ctors`,
 *              `.ctors.65434`, `.dtors` and `.dtors.65434` (and, at -O2, the functions in `.text.startup` and
 *              `.text.exit`); clang-cl as `.CRT$XCU`, `.CRT$XCA00101`, `.CRT$XTX`, `.CRT$XTA00101`.
 *   UNIT_CLEANUP  a `cleanup` variable around an external call: under -fexceptions the function's unwind record
 *              names gcc's handler `__gcc_personality_seh0` with its language data, and at -O2 its cold path
 *              `c.cold` sits in `.text.unlikely` with its own records. The tests' `main.c` (`unlock` adds the
 *              variable's value, `g` does nothing; main returns 42 when `c()` is 1 and the cleanup ran once)
 *              exits 42 under gcc + GNU ld.
 *   UNIT_CTOR_MSVC  MSVC's own C idiom for the same: a STATIC pointer to an initializer placed in `.CRT$XCU`
 *              (`#pragma section` + `__declspec(allocate)`), the initializer setting `counter` to 11, and the same
 *              `main`: 11 under link.exe. The table's one symbol is static (storage class 3), so no external
 *              definition names it.
 *
 * BUILT BY (`build_gnu_subsection_fixtures.py` of lane xa's work dir; each object twice, byte-identical, recording
 * no path of the host that built it), each from a copy of this file named `coff_gnu_subsections.c`:
 *   mingw-w64 gcc 13.2.0 -O2 -c -DUNIT_COLD                       -> *_cold_x86_64_pe_gcc_O2.obj
 *   mingw-w64 gcc 13.2.0 -O2 -ffunction-sections -c -DUNIT_COLD   -> *_cold_x86_64_pe_gcc_O2_function_sections.obj
 *   mingw-w64 gcc 13.2.0 -O2 -g -ffile-prefix-map=<build dir>=X:/dss-fixture -c -DUNIT_COLD
 *                                                                 -> *_cold_x86_64_pe_gcc_O2_g.obj
 *   mingw-w64 gcc 13.2.0 -O0 -fexceptions -c -DUNIT_CLEANUP       -> *_cleanup_x86_64_pe_gcc_O0_fexceptions.obj
 *   mingw-w64 gcc 13.2.0 -O2 -fexceptions -c -DUNIT_CLEANUP       -> *_cleanup_x86_64_pe_gcc_O2_fexceptions.obj
 *   mingw-w64 gcc 13.2.0 -O0 -c -DUNIT_CTOR                       -> *_ctor_x86_64_pe_gcc_O0.obj
 *   mingw-w64 gcc 13.2.0 -O2 -c -DUNIT_CTOR                       -> *_ctor_x86_64_pe_gcc_O2.obj
 *   clang-cl 19.1.5 /c /O2 /MD /Brepro -DUNIT_CTOR                -> *_ctor_x86_64_pe_clangcl_MD_O2.obj
 *   cl 19.44.35229 /nologo /c /O2 /MD /GS- /Brepro /experimental:deterministic
 *       /pathmap:<build dir>=X:\dss-fixture -DUNIT_CTOR_MSVC      -> *_ctor_x86_64_pe_cl_MD_O2.obj
 */
#if defined(UNIT_COLD)
extern int ext(int);
__attribute__((cold, noinline)) int rare(int x) { return ext(x) * 3; }
__attribute__((hot, noinline)) int often(int x) { return x + 1; }
int pick(int x) {
    if (__builtin_expect(x < 0, 0)) return ext(-x) + rare(x);
    return often(x);
}
#elif defined(UNIT_CLEANUP)
void unlock(int *p);
void g(void);
int c(void) { int x __attribute__((cleanup(unlock))) = 1; g(); return x; }
#elif defined(UNIT_CTOR)
int volatile counter;   /* volatile: no compiler folds the constructors into its initial value */
__attribute__((constructor)) static void c_plain(void) { counter += 1; }
__attribute__((constructor(101))) static void c_101(void) { counter += 10; }
__attribute__((destructor)) static void d_plain(void) { counter -= 1; }
__attribute__((destructor(101))) static void d_101(void) { counter -= 10; }
int main(void) { return counter; }
#elif defined(UNIT_CTOR_MSVC)
int volatile counter;
static void __cdecl set_counter(void) { counter = 11; }
#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) static void(__cdecl *const set_counter_entry)(void) = set_counter;
int main(void) { return counter; }
#else
#error "name the unit: -DUNIT_COLD, -DUNIT_CLEANUP, -DUNIT_CTOR or -DUNIT_CTOR_MSVC"
#endif
