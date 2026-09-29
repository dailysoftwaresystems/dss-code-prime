/* The STATIC LIBRARY half (see main.c): every printf-family function DSS synthesizes on pe, called from a DSS-built
 * archive member — so the member carries its own copies of the bodies, as the program's CU does. The stream arrives
 * as a parameter: this example is about synthesized BODIES, and a member that names a library datum such as
 * `stdout` itself is a separate subject (see expected.json). */
#include <stdarg.h>
#include <stdio.h>

static int vsay(FILE *out, char const *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int const r = vfprintf(out, fmt, ap);
    va_end(ap);
    return r;
}

int lib_value(FILE *out) {
    char buf[32];
    int v = 0;
    sprintf(buf, "%d", 7);
    snprintf(buf, sizeof buf, "%d%d", 1, 4);   /* "14" */
    if (sscanf(buf, "%d", &v) != 1) return -1;
    printf("lib: %d\n", v);
    fprintf(out, "lib: fprintf\n");
    vsay(out, "lib: %s\n", "vfprintf");
    return v;
}
