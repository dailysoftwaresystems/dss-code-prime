/* The library half: `long double` across a dynamic-library boundary — a global
   the library defines, read and written through the library's own code and
   through an address it hands out, arithmetic, and a conversion. */
long double g_scale = 2.0L;

long double ld_scale_add(long double a, long double b) { return a * g_scale + b; }

int ld_to_int(long double x) { return (int)x; }

long double *ld_scale_addr(void) { return &g_scale; }
