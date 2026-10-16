/* A SECOND NAME OF A DEFINITION: the GNU `alias("target")` attribute.
 *
 * Each alias below is one more name on a definition of this unit -- never a body or storage
 * of its own -- so a call, a read, a write and an address taken through it are the target's.
 * gcc, clang and mingw-w64 gcc build every line on ELF and PE (Apple clang refuses an alias on
 * Mach-O, and so does DSS there, by name). */

int dss_add(int a, int b) { return a + b; }
int dss_sum(int, int) __attribute__((alias("dss_add")));             /* an external alias of a function */
int dss_weak_sum(int, int) __attribute__((weak, alias("dss_add")));  /* a weak one nothing replaces */

int dss_counter = 5;
extern int dss_counter_too __attribute__((alias("dss_counter")));    /* an external alias of an object */

static int dss_seven(void) { return 7; }
static int dss_seven_again(void) __attribute__((alias("dss_seven"))); /* a module-private alias */

static int dss_private_body(void) { return 40; }
int dss_exported_body(void) __attribute__((alias("dss_private_body"))); /* a global name on a `static` body */

int dss_tentative;   /* a tentative definition an alias names: a definition of this unit, never a common */
extern int dss_tentative_too __attribute__((alias("dss_tentative")));

int main(void) {
    int (*volatile sum)(int, int) = dss_sum;
    if (sum != dss_add) return 1;                    /* one definition, one address */
    if (&dss_counter_too != &dss_counter) return 2;
    dss_counter_too += sum(2, 3);                    /* written through the second name: 5 + 5 */
    if (dss_counter != 10) return 3;
    if (dss_seven_again() != 7) return 4;
    if (dss_weak_sum(20, 22) != 42) return 5;
    dss_tentative_too = 2;
    if (dss_tentative != 2) return 6;
    if (dss_exported_body() != 40) return 7;         /* called by its global name, the body module-private */
    return 42;
}
