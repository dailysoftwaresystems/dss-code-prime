/* library_function_address_equals_getprocaddress — see expected.json.
 *
 * One address per function and per datum across the process: every form of `&puts` in this
 * executable AND in the DLL it loads must equal what GetProcAddress answers for `puts`, and the
 * addresses of the DLL's own `g` and `v`, taken here in code and in static initializers, must equal
 * what GetProcAddress answers for them. Then a call through each kind of slot reaches its function.
 *
 * And the slots no loader can bind (P69 review M1, D-LK-PE-IMPORT-ADDRESS-SLOT-RESIDUE): an import's
 * address PLUS an offset (`&arr[3]` of the DLL's array, in writable and in const data, and the DLL's
 * own `&_mbcasemap + 3`), and an import's address in a THREAD-LOCAL initializer — the loader copies the
 * main thread's block from the template before it binds any import. Each image's residue runner (its
 * first TLS callback) writes them at load; every thread, the main one and one started later, must see
 * the bound values.
 *
 * Exit 42 when every check holds; otherwise 1000 + a bitset naming what failed (never 42):
 *   1          a GetProcAddress lookup failed (the comparisons below would prove nothing)
 *   2 << i     the executable's form i of &puts differs (i: code, static, const, member, element)
 *   64 << i    the DLL's form i of &puts differs
 *   2048       &g (code or static) differs from GetProcAddress("g")
 *   4096       &v (code or static) differs from GetProcAddress("v")
 *   8192       a call through the static `puts` slot failed
 *   16384      g() + *(&v) through the static slots is not 42
 *   32768      &_mbcasemap (a C-library DATUM, static) differs from GetProcAddress("_mbcasemap")
 *   65536      the static `&arr[3]` differs from GetProcAddress("arr") + 3 ints, or does not read 13
 *   131072     the CONST static `&arr[1]` differs, or does not read 11
 *   262144     the DLL's `&_mbcasemap + 3` differs from GetProcAddress("_mbcasemap") + 3
 *   524288     on the main thread, a thread-local slot holds something but the bound value
 *   1048576    on a thread started later, a thread-local slot holds something but the bound value
 *   2097152    the later thread could not be started */
#include <stdio.h>
#include <windows.h>

typedef int (*put_fn)(const char *);
struct holder {
    int    tag;
    put_fn fn;
};

extern int v;
extern int arr[4];
int  g(void);
void puts_forms_of_the_dll(void const **out);
char const *dll_mbcasemap_plus_three(void);
/* A ucrtbase.dll DATA export (ctype.json declares it for pe): it holds a pointer to the case map. */
extern unsigned char *_mbcasemap;

static put_fn        exe_plain = puts;
static put_fn const  exe_const = puts;
static struct holder exe_member = {1, puts};
static put_fn        exe_array[3] = {0, puts, 0};
static int (*exe_g)(void) = g;
static int *exe_v = &v;
static unsigned char **exe_mbcasemap = &_mbcasemap;
/* The residue: an import's address plus an offset, in writable and in read-only data. */
static int *exe_arr3 = &arr[3];
static int *const exe_arr1 = &arr[1];
/* ...and an import's address in a thread-local TEMPLATE: a function, a datum, a datum plus an offset. */
static _Thread_local put_fn tl_puts = puts;
static _Thread_local int   *tl_v = &v;
static _Thread_local int   *tl_arr2 = &arr[2];

__attribute__((noinline)) static put_fn exe_code(void) { return puts; }
__attribute__((noinline)) static void const *code_g(void) { return (void const *)g; }
__attribute__((noinline)) static void const *code_v(void) { return (void const *)&v; }

static void const *want_puts;
static void const *want_v;
static void const *want_arr;

/* 0 when THIS thread's copies of the thread-locals hold the bound values. */
static int thread_locals_wrong(void) {
    int wrong = 0;
    if ((void const *)tl_puts != want_puts) wrong |= 1;
    if ((void const *)tl_v != want_v) wrong |= 2;
    if ((void const *)tl_arr2 != (void const *)((int const *)want_arr + 2) || *tl_arr2 != 12) wrong |= 4;
    return wrong;
}

static volatile int later_thread_wrong = -1;
static unsigned long later_thread(void *arg) {
    (void)arg;
    later_thread_wrong = thread_locals_wrong();
    return 0;
}

int main(void) {
    /* The image the shipped descriptors bind `puts` to on this format, and the DLL this program
     * imports from (already loaded: LoadLibraryA only returns its handle). */
    HMODULE     crt = LoadLibraryA("ucrtbase.dll");
    HMODULE     sub = LoadLibraryA("dsssubject.dll");
    want_puts = crt ? (void const *)GetProcAddress(crt, "puts") : 0;
    void const *want_g = sub ? (void const *)GetProcAddress(sub, "g") : 0;
    want_v = sub ? (void const *)GetProcAddress(sub, "v") : 0;
    want_arr = sub ? (void const *)GetProcAddress(sub, "arr") : 0;
    void const *want_mbc = crt ? (void const *)GetProcAddress(crt, "_mbcasemap") : 0;

    void const *exe[5] = {(void const *)exe_code(), (void const *)exe_plain, (void const *)exe_const,
                          (void const *)exe_member.fn, (void const *)exe_array[1]};
    void const *dll[5] = {0, 0, 0, 0, 0};
    puts_forms_of_the_dll(dll);

    int bad = 0;
    if (want_puts == 0 || want_g == 0 || want_v == 0 || want_arr == 0 || want_mbc == 0) bad |= 1;
    for (int i = 0; i < 5; ++i) {
        if (exe[i] != want_puts) bad |= 2 << i;
        if (dll[i] != want_puts) bad |= 64 << i;
    }
    if ((void const *)exe_g != want_g || code_g() != want_g) bad |= 2048;
    if ((void const *)exe_v != want_v || code_v() != want_v) bad |= 4096;
    if (exe_plain("one address") < 0) bad |= 8192;
    if (exe_g() + *exe_v != 42) bad |= 16384;
    if ((void const *)exe_mbcasemap != want_mbc) bad |= 32768;
    if ((void const *)exe_arr3 != (void const *)((int const *)want_arr + 3) || *exe_arr3 != 13) bad |= 65536;
    if ((void const *)exe_arr1 != (void const *)((int const *)want_arr + 1) || *exe_arr1 != 11) bad |= 131072;
    if ((void const *)dll_mbcasemap_plus_three() != (void const *)((char const *)want_mbc + 3)) bad |= 262144;
    if (thread_locals_wrong() != 0) bad |= 524288;
    void *h = CreateThread(0, 0, (void *)later_thread, 0, 0, 0);
    if (h == 0) {
        bad |= 2097152;
    } else {
        WaitForSingleObject(h, 0xFFFFFFFFu);
        CloseHandle(h);
        if (later_thread_wrong != 0) bad |= 1048576;
    }
    return bad == 0 ? 42 : 1000 + bad;
}
