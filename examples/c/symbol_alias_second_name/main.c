/* A SECOND NAME OF A DEFINITION: the GNU `alias("target")` attribute.
 *
 * Each alias below is one more name on a definition of this unit -- never a body or storage
 * of its own -- so a call, a read, a write and an address taken through it are the target's.
 * gcc, clang and mingw-w64 gcc build every line on ELF and PE but ONE: the function name on an object
 * below, which gcc and mingw-w64 gcc refuse and clang builds (decided clang's; lane cs probes rv and fk).
 * Apple clang refuses an alias on Mach-O, and so does DSS there, by name. */

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

static int dss_static_store = 11;
static int dss_static_store_too __attribute__((alias("dss_static_store")));  /* `static`, of a `static` object */
extern int dss_static_store_ext __attribute__((alias("dss_static_store")));  /* external, of a `static` object */
int dss_shared = 12;
static int dss_shared_private __attribute__((alias("dss_shared")));         /* `static`, of an external object */
_Thread_local int dss_tls = 13;
extern _Thread_local int dss_tls_too __attribute__((alias("dss_tls")));      /* thread-local, of a thread-local */
int dss_visible = 14;
extern int dss_hidden __attribute__((alias("dss_visible"), visibility("hidden")));
static int dss_word = 42;
static unsigned char dss_word_low __attribute__((alias("dss_word")));  /* another type: the word's first byte */

/* A function name on an OBJECT (gcc refuses the pair, clang builds it): one storage under two names, so an
 * address taken through either is the other's. clang 18.1.3 compares the two equal through volatile storage
 * and answers the one comparison its constant folder sees, (void*)f == (void*)&g, FALSE; this compiler answers
 * it TRUE as well -- the recorded cost of the decision (lane cs, probe fk). */
int dss_storage = 42;
int dss_storage_as_function(void) __attribute__((alias("dss_storage")));

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
    dss_static_store_too += 1;
    if (dss_static_store != 12 || &dss_static_store_ext != &dss_static_store) return 8;
    dss_shared_private = 30;
    if (dss_shared != 30) return 9;
    dss_tls_too += 1;
    if (dss_tls != 14) return 10;
    dss_hidden += 1;
    if (dss_visible != 15) return 11;
    if (dss_word_low != 42) return 12;               /* little-endian: every target this example builds */
    void *volatile through = (void *)dss_storage_as_function;
    if (through != (void *)&dss_storage) return 13;  /* clang 18.1.3 answers this one equal too */
    if ((void *)dss_storage_as_function != (void *)&dss_storage) return 14;  /* clang's folder: 14 */
    return 42;
}
