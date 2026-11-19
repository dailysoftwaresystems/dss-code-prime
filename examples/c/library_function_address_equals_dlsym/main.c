/* D-LK-LIBRARY-FUNCTION-ADDRESS-IS-THE-IMAGE-STUB — ONE address per library function, across the process.
 *
 * C23 6.5.9: two pointers to the same function compare equal. POSIX dlsym answers with the address the dynamic
 * linker binds for the name. So every way this program can spell the address of `puts` — code, a static
 * initializer, a const one, a struct member, an array element — must be that ONE address, the pointer must
 * still call `puts`, and the SHARED LIBRARY this program loads (dsssubject.c, built by DSS as a `-dyn` /
 * `-dylib`) must answer the same five times. The addresses of the library's own `g` and `v`, taken here in code
 * and in static initializers, must equal what dlsym answers for them, and calls through them must work.
 *
 * ✔MEASURED 2026-09-30, before the fix: DSS's ELF executables named the image's own PLT stub everywhere
 * (0 of 5 equal to dlsym); its PIEs and Mach-O images DISAGREED WITH THEMSELVES — the code named the stub, the
 * static initializers the library's function (`s_plain != code_address()`). Every reference (gcc and clang,
 * -no-pie and -pie, both ISAs; Apple clang 21 on both Mac ISAs) answers 42 here.
 *
 * Exit: 42 when every check holds; otherwise 1, after printing `bad=<bitset>` (a bitset EXIT would be ambiguous:
 * 2|8|32 is 42):
 *   1 the code form, 2 the static, 4 the const, 8 the member, 16 the element (each against dlsym)
 *   32 the static against the code form (6.5.9 inside the program), 64 the call through the element failed
 *   128 << i  the library's form i (code, static, const, member, element) against dlsym
 *   4096 &g (code or static) against dlsym, 8192 &v (code or static) against dlsym, 16384 g() + v is not 42 */
#include <dlfcn.h>
#include <stdio.h>

typedef int (*puts_fn)(const char *);

static puts_fn       s_plain = puts;              /* a plain static initializer */
static puts_fn const s_const = puts;              /* a const one: read-only data */
struct holder { int tag; puts_fn fn; };
static struct holder s_struct = { 7, puts };      /* a struct member */
static puts_fn       s_array[3] = { 0, puts, 0 }; /* an array element */

/* The shared library's definitions, and its five forms of &puts. */
extern int v;
int  g(void);
void puts_forms_of_the_library(void const **out);
static int (*s_g)(void) = g;
static int *s_v = &v;

/* The address taken in CODE, in a function of its own so the optimizer cannot fold it into a comparison. */
__attribute__((noinline)) static puts_fn code_address(void) { return puts; }
__attribute__((noinline)) static void const *code_g(void) { return (void const *)g; }
__attribute__((noinline)) static void const *code_v(void) { return (void const *)&v; }

int main(void) {
    void *self = dlopen(NULL, RTLD_NOW);
    void *bound = self ? dlsym(self, "puts") : NULL;
    void *bound_g = self ? dlsym(self, "g") : NULL;
    void *bound_v = self ? dlsym(self, "v") : NULL;
    if (!bound || !bound_g || !bound_v) return 90;
    int bad = 0;
    if ((void *)code_address() != bound) bad |= 1;
    if ((void *)s_plain != bound) bad |= 2;
    if ((void *)s_const != bound) bad |= 4;
    if ((void *)s_struct.fn != bound) bad |= 8;
    if ((void *)s_array[1] != bound) bad |= 16;
    if (s_plain != code_address()) bad |= 32;     /* 6.5.9 inside the program itself */
    if (s_array[1]("one address") < 0) bad |= 64; /* and the address still calls puts */
    void const *lib[5] = { 0, 0, 0, 0, 0 };
    puts_forms_of_the_library(lib);
    for (int i = 0; i < 5; ++i) {
        if (lib[i] != bound) bad |= 128 << i;
    }
    if ((void const *)s_g != bound_g || code_g() != bound_g) bad |= 4096;
    if ((void const *)s_v != bound_v || code_v() != bound_v) bad |= 8192;
    if (s_g() + *s_v != 42) bad |= 16384;
    if (bad != 0) {
        printf("bad=%d\n", bad);
        return 1;
    }
    return 42;
}
