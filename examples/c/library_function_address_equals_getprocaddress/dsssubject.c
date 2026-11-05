/* The DLL half of library_function_address_equals_getprocaddress (see expected.json).
 *
 * It takes the address of the C library's `puts` in every form a C program can spell — in code, in a
 * static, a const static, a struct member and an array element — and hands those values to the
 * executable, which compares them with its own forms and with what GetProcAddress answers. It also
 * defines the function `g`, the datum `v` and the array `arr` whose addresses the executable takes across
 * the image boundary, and it holds a slot its own loader cannot bind: the address of a C-library DATUM
 * plus an offset, which this DLL's import-slot residue runner writes at load (P69 review M1 (a)). */
#include <stdio.h>

typedef int (*put_fn)(const char *);
struct holder {
    int    tag;
    put_fn fn;
};

int v = 7;
int arr[4] = {10, 11, 12, 13};
int g(void) { return 35; }

static put_fn              dll_plain = puts;
static put_fn const        dll_const = puts;
static struct holder       dll_member = {1, puts};
static put_fn              dll_array[3] = {0, puts, 0};

/* A ucrtbase.dll DATA export (ctype.json declares it for pe), and three bytes into it: the loader writes an
 * import's address and nothing else, so this slot is the DLL's residue. */
extern unsigned char *_mbcasemap;
static char const *dll_mbcasemap_plus_3 = (char const *)&_mbcasemap + 3;

__attribute__((noinline)) static put_fn dll_code(void) { return puts; }

/* The DLL's five forms, in the executable's order: code, static, const, member, element. */
void puts_forms_of_the_dll(void const **out) {
    out[0] = (void const *)dll_code();
    out[1] = (void const *)dll_plain;
    out[2] = (void const *)dll_const;
    out[3] = (void const *)dll_member.fn;
    out[4] = (void const *)dll_array[1];
}

/* The DLL's residue slot, as the executable compares it. */
char const *dll_mbcasemap_plus_three(void) { return dll_mbcasemap_plus_3; }
