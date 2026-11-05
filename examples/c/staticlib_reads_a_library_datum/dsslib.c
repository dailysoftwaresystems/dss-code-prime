/* The library half of staticlib_reads_a_library_datum (see expected.json): a DSS static library whose code READS a
 * C-library datum, `stdout`, and hands the value it read to the program that links it — and which takes the
 * address of a C-library FUNCTION, `puts`, in code and in a static, for the program to compare with what the
 * platform's loader answers (P69 review M2). It names `puts` ONLY by its address: nothing here calls it, so no
 * relocation of the member states that `puts` is a function, and the link takes the kind from the library that
 * defines it (D-LK-MEMBER-UNTYPED-EXTERN-TAKEN-AS-DATA, see expected.json). */
#include <stdio.h>

typedef int (*put_fn)(const char *);

FILE *lib_stdout(void) { return stdout; }

int lib_prints(void) { return fputs("from the library\n", stdout) >= 0; }

/* The library's two forms of &puts: a static initializer, and the address taken in code (a function of its own,
 * so no optimizer folds it into a comparison). */
put_fn lib_static_puts = puts;
__attribute__((noinline)) put_fn lib_code_puts(void) { return puts; }
