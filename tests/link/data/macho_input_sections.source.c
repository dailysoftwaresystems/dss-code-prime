/* The reference objects of tests/link/test_input_section_names.cpp's Mach-O cells: each CELL_* is ONE input section a
 * reference compiler (Apple clang) places a function's constant in, reached from `check()`, which returns 42 when the
 * constant reads back as written. `volatile` keeps every constant out of the instruction stream, so the bytes the
 * section holds are the ones the code reads.
 *   CELL_CSTRING       a string literal, reached from code          (__TEXT,__cstring)
 *   CELL_CSTRING_DATA  a string literal, reached from a pointer      (__TEXT,__cstring, from __DATA,__data)
 *   CELL_LITERAL4      a float constant                             (x86_64: __TEXT,__literal4; arm64: an immediate)
 *   CELL_LITERAL8      a double constant                            (x86_64: __TEXT,__literal8; arm64: an immediate)
 *   CELL_LITERAL16     a 16-byte vector constant                    (__TEXT,__literal16)
 *   CELL_CONST         a constant table                             (__TEXT,__const)
 *   CELL_RELRO         a constant table of addresses of literals    (__DATA,__const, into __TEXT,__cstring)
 * WITH_MAIN adds the reference's own program of the unit: `main` returns what `check` returns.
 *
 * HOW EACH WAS BUILT (P69 fold 3e, lane `xa`, 2026-11-13; the orchestrator's ledger 577). On macOS (arm64 host) by
 * probe-reference-cc: Apple clang version 21.0.0 (clang-2100.4.1.1), `cc -c -arch <arch> -std=c17 -O2
 * -ffile-prefix-map=<probe dir>=. -DCELL_<cell> macho_input_sections.c` from this file's code (this record was added
 * to its comment after), each object compiled TWICE, the two byte-identical; no object records a host path. arm64 in run 20261113-223526-701281de, x86_64 in run
 * 20261113-223609-b9727adc; the sections, relocations and symbols read by `objdump --macho`; and the reference's own
 * program of each (the same flags plus -DWITH_MAIN, linked by the same driver; x86_64 under the host's translation)
 * RAN, each exiting 42. Fixture `macho_input_section_<cell>_<arch>_clang_O2.o`, bytes and md5:
 *   arm64   cstring 872 15243f68f5d27d10c035a527f697b9e8   cstring_data 872 a1cb04df8f580440929e810153142fe8
 *           literal4 736 277ee8b6c2c90f081b61a7561295e6c2  literal8 752 837a641529d242169a0c9278936109b4
 *           literal16 912 e08e63ce6c21a08e8de0194a4dd9c130 const 872 d4f937c763558bf40de0561e09d52808
 *           relro 1136 94bc2e5f443af8a06f48718290047f8f
 *   x86_64  cstring 864 bb8c77bab467664a72afbfa5f7c028d4   cstring_data 888 79ba2bc492d6453620816c7d790da287
 *           literal4 888 1b896007f879afc7b0826b28a59db8c2  literal8 912 a8d279f947c028ea50013e8c43c258b4
 *           literal16 952 b1727e12796f06654871d00a3543b62d const 896 e8d850fea78552a4e14fb748690f2041
 *           relro 1048 51482598af55c5f10811eef5f14676f7
 * What the objects hold (READ): arm64 keeps `l_.str` / `lCPI0_0` labels in `__cstring` / `__literal16` and reaches
 * them by ARM64_RELOC_PAGE21 + PAGEOFF12 r_extern=1; its float and double constants are immediates (no literal
 * section). x86_64 keeps no label in `__cstring` / `__literal{4,8,16}` and reaches them by X86_64_RELOC_SIGNED
 * r_extern=0 from code and X86_64_RELOC_UNSIGNED r_extern=0 from `__DATA` (`__data`, `__const`). */
#if defined(CELL_CSTRING)
static volatile int k = 4;
int check(void) {
    const char *s = "dss-cstring-literal";
    return s[k] == 'c' ? 42 : 1;
}
#elif defined(CELL_CSTRING_DATA)
static const char *volatile p = "dss-literal-in-data";
int check(void) { return p[4] == 'l' && p[18] == 'a' ? 42 : 1; }
#elif defined(CELL_LITERAL4)
static volatile float f = 2.0f;
int check(void) {
    float const r = f * 0.1f;
    return r > 0.19f && r < 0.21f ? 42 : 1;
}
#elif defined(CELL_LITERAL8)
static volatile double x = 1.5;
int check(void) {
    double const r = x * 3.1;
    return r > 4.6 && r < 4.7 ? 42 : 1;
}
#elif defined(CELL_LITERAL16)
typedef int v4 __attribute__((vector_size(16)));
static volatile v4 in = {1, 1, 1, 1};
int check(void) {
    v4 const r = in + (v4){2, 4, 6, 41};
    return r[0] == 3 && r[1] == 5 && r[2] == 7 ? r[3] : 1;
}
#elif defined(CELL_CONST)
static const int table[8] = {3, 1, 4, 1, 5, 9, 2, 42};
static volatile int k = 7;
int check(void) { return table[k]; }
#elif defined(CELL_RELRO)
static const char *const names[3] = {"zero", "one", "forty-two"};
static volatile int k = 2;
int check(void) { return names[k][0] == 'f' && names[k][8] == 'o' ? 42 : 1; }
#else
#error "define one CELL_*"
#endif
#if defined(WITH_MAIN)
int main(void) { return check(); }
#endif
