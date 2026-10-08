/* staticlib_reads_a_library_datum — see expected.json. The program and the static library it links must read ONE
 * `stdout`, the library's write through it must succeed, and the library's two forms of `&puts` — code and a static
 * — must be ONE address, the one the platform's loader answers for `puts` (GetProcAddress on Windows, dlsym
 * elsewhere), which is also this program's own `&puts`. Exit 42, or 100 + a bitset (never 42; at most 163):
 *   1 the two reads of stdout differ, 2 the library's write failed, 4 the loader lookup failed,
 *   8 the library's code &puts differs from the loader's, 16 its static differs, 32 its code &puts differs from
 *   this program's. */
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef int (*put_fn)(const char *);

FILE  *lib_stdout(void);
int    lib_prints(void);
extern put_fn lib_static_puts;
put_fn lib_code_puts(void);

/* What the platform's loader binds `puts` to. */
static void const *bound_puts(void) {
#if defined(_WIN32)
    HMODULE crt = LoadLibraryA("ucrtbase.dll");
    return crt ? (void const *)GetProcAddress(crt, "puts") : 0;
#else
    void *self = dlopen(NULL, RTLD_NOW);
    return self ? dlsym(self, "puts") : 0;
#endif
}

int main(void) {
    int bad = 0;
    if (lib_stdout() != stdout) bad |= 1;
    if (!lib_prints()) bad |= 2;
    void const *want = bound_puts();
    if (want == 0) bad |= 4;
    if ((void const *)lib_code_puts() != want) bad |= 8;
    if ((void const *)lib_static_puts != want) bad |= 16;
    if ((void const *)lib_code_puts() != (void const *)puts) bad |= 32;
    fflush(stdout);
    return bad == 0 ? 42 : 100 + bad;
}
