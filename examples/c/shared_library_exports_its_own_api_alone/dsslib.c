/* The LIBRARY half: one function, `lib_entry`, that calls shipped-runtime functions so the runtime archive's members
 * are linked INTO this image — memalignment (DSS's runtime body on every format), strfromd and snprintf (DSS's C23
 * entry points on pe and Mach-O). Those bodies are this library's implementation, never its API: a reference
 * toolchain exports `lib_entry` alone. The program never calls it; main.c reads this file's export table. */
#include <stdio.h>
#include <stdlib.h>

int lib_entry(void *p) {
    char number[32];
    strfromd(number, sizeof number, "%g", 1.5);
    char line[48];
    snprintf(line, sizeof line, "[%s]", number);
    return (int)memalignment(p) + line[1];
}
