/* D-FFI-DUPLICATE-SYMBOL-ACROSS-DESCRIPTORS-SILENTLY-ORDER-RESOLVED — the
 * RUNTIME witness for the HAND-DECLARED half of the rule, and the twin of
 * `shipped_duplicate_symbol_agreement` (which is the `#include` half).
 *
 * NOT ONE `#include` IN THIS FILE. Every name below is declared by the TU
 * itself (C23 7.1.4p2 — a standard-library function may be used through a
 * hand-written declaration), so none of them reaches the corpus through the
 * include closure. They arrive instead through the PLATFORM REALIZATION ORACLE
 * (`ffi::realizeShippedExternSymbols`), which walks the corpus index's relPath
 * sort — a DIFFERENT ORDER from the include closure, and the whole subject of
 * this row.
 *
 * WHY THESE SIX NAMES. `math.json` and `tgmath.json` both declare all of them
 * for the SAME object formats — <tgmath.h> deliberately mirrors <math.h>'s
 * surface, exactly as <memory.h> mirrors <string.h>'s. ✔MEASURED over the
 * shipped corpus: math+tgmath is the LARGEST co-live duplicate set (34 names on
 * elf/macho, 32 on pe), so every call below is a name for which TWO descriptors
 * are read, compared, and required to agree before anything binds.
 *
 * WHAT THIS PINS THAT A UNIT TEST CANNOT: that the agreement rule now runs on
 * the hand-declared path WITHOUT refusing the shipped configuration, and that
 * the surviving import still WORKS — on every leg, at RUNTIME, through both
 * example runners and the release pipeline. A check that degenerated into
 * "duplicates are forbidden" fails this at BUILD time on all four targets; a
 * check wired to the wrong side of the availability gate fails it on exactly
 * one format.
 *
 * ✔MEASURED BEFORE (pe64, `objdump -p`, a DSS_CONFIG_ROOT copy whose
 * memory.json named msvcrt.dll where string.json named ucrtbase.dll): the two
 * `#include` orders REFUSED with F_ShippedCorpusInvariantBroken, and this
 * spelling — a bare prototype, no include — built rc=0 with ZERO diagnostics
 * and imported `memcpy` from MSVCRT.DLL. One program, two spellings, two C
 * runtimes, no diagnostic.
 *
 * Every input is `volatile`, so ConstFold cannot fold a call away and each
 * check exercises the real library binding. The results are IEEE-754-exact
 * (pow(2,10)==1024, fmod(10,4)==2, ldexp(1,10)==1024, floor/ceil of 2.5) except
 * atan2, compared within 1e-15 — glibc, ucrtbase and libSystem are all well
 * inside that. A wrong winner or a dropped surface changes the EXIT CODE, not
 * merely the import table. */

double pow(double, double);
double fmod(double, double);
double atan2(double, double);
double ldexp(double, int);
double floor(double);
double ceil(double);

static int near(double got, double want) {
    double d = got - want;
    if (d < 0.0) d = -d;
    return d <= 1e-15;
}

int main(void) {
    volatile double two  = 2.0;
    volatile double ten  = 10.0;
    volatile double four = 4.0;
    volatile double one  = 1.0;
    volatile double half = 0.5;
    volatile int    tenI = 10;

    if (pow(two, ten) != 1024.0)    return 1;
    if (fmod(ten, four) != 2.0)     return 2;
    if (ldexp(one, tenI) != 1024.0) return 3;
    if (floor(two + half) != 2.0)   return 4;
    if (ceil(two + half) != 3.0)    return 5;
    if (!near(atan2(one, one) * 8.0, 6.28318530717958623)) return 6;

    return 42;
}
