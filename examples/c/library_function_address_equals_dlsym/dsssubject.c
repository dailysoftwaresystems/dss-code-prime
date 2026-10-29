/* The shared-library half of library_function_address_equals_dlsym (see expected.json).
 *
 * A DSS shared library (`-dyn` on ELF, `-dylib` on Mach-O) takes the address of the C library's `puts` in every
 * form a C program can spell — in code, in a static, a const static, a struct member and an array element — and
 * hands those values to the executable, which compares them with what the dynamic linker answers for `puts`. It
 * also defines the function `g` and the datum `v` whose addresses the executable takes across the image boundary.
 * A shared library can never make its own stub the function's address — the executable's definition, or the C
 * library's, is the one the process sees — so each of these forms is LOADED from a slot the loader fills. */
#include <stdio.h>

typedef int (*puts_fn)(const char *);
struct holder {
    int     tag;
    puts_fn fn;
};

int v = 7;
int g(void) { return 35; }

static puts_fn       lib_plain = puts;
static puts_fn const lib_const = puts;
static struct holder lib_member = {1, puts};
static puts_fn       lib_array[3] = {0, puts, 0};

__attribute__((noinline)) static puts_fn lib_code(void) { return puts; }

/* The library's five forms, in the executable's order: code, static, const, member, element. */
void puts_forms_of_the_library(void const **out) {
    out[0] = (void const *)lib_code();
    out[1] = (void const *)lib_plain;
    out[2] = (void const *)lib_const;
    out[3] = (void const *)lib_member.fn;
    out[4] = (void const *)lib_array[1];
}
