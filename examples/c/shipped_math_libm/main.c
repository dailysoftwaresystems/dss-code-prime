#include <math.h>
#include <stdio.h>

/* c87 (D-FFI-MATH-LIBM-DT-NEEDED): the math symbols' elf home library is
   libm.so.6 (math.json `library`), and the ELF walker emits one DT_NEEDED
   per DISTINCT referenced import library — so this binary links BOTH
   libc.so.6 (exit/puts) and libm.so.6 (every math call below) and ld.so
   resolves sqrt/pow/... from libm at load. Every input is volatile (a
   RUNTIME value — ConstFold cannot pre-fold a single call away; each check
   exercises the real library binding). The exact-equality expectations are
   IEEE-754-exact results (sqrt(4)==2, pow(2,10)==1024, log(1)==0,
   ldexp(1,10)==1024 — ldexp moved to libm on elf too, fmod(10,4)==2,
   fabs/floor/ceil); the transcendentals compare within 1e-15 absolute
   (glibc, msvcrt, and libSystem are all well inside that). RED-ON-DISABLE:
   revert math.json's elf `library` to libc.so.6 -> the image carries only
   `NEEDED libc.so.6` and ld.so refuses to load it (`undefined symbol:
   sqrt`, exit 127) — the exact pre-c87 sqlite3 failure shape. */

static int near(double got, double want) {
    double d = got - want;
    if (d < 0.0) d = -d;
    return d <= 1e-15;
}

int main(void) {
    volatile double four = 4.0;
    volatile double two  = 2.0;
    volatile double ten  = 10.0;
    volatile double one  = 1.0;
    volatile double zero = 0.0;
    volatile double half = 0.5;

    if (sqrt(four) != 2.0)        return 1;  /* exact */
    if (pow(two, ten) != 1024.0)  return 2;  /* exact */
    if (log(one) != 0.0)          return 3;  /* exact */
    if (ldexp(one, 10) != 1024.0) return 4;  /* exact */
    if (fmod(ten, four) != 2.0)   return 5;  /* exact */
    if (fabs(-two) != 2.0)        return 6;  /* exact */
    if (floor(two + half) != 2.0) return 7;  /* exact */
    if (ceil(two + half) != 3.0)  return 8;  /* exact */
    if (!near(sin(zero), 0.0))    return 9;
    if (!near(cos(zero), 1.0))    return 10;
    if (!near(exp(zero), 1.0))    return 11;
    if (!near(log10(ten), 1.0))   return 12;
    if (!near(tan(zero), 0.0))    return 13;
    if (!near(atan(one) * four, 3.14159265358979312))       return 14;
    if (!near(atan2(one, one) * 8.0, 6.28318530717958623))  return 15;
    if (!near(asin(one) * two, 3.14159265358979312))        return 16;
    if (!near(acos(zero) * two, 3.14159265358979312))       return 17;
    if (!near(sin(half), 0.479425538604203006))             return 18;
    if (!near(log(two), 0.693147180559945286))              return 19;
    puts("math-libm-ok");
    return 42;
}
