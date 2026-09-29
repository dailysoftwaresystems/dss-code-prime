/* <getopt.h> and <stdio.h> in ONE program — the commonest getopt shape: parse the options, print the result
 * (P68 round 11, D-LK-SYNTHESIZED-LIBRARY-BODY-DEFINED-STRONG-IN-EVERY-UNIT).
 *
 * On pe DSS realizes getopt from a shipped runtime unit (runtime/platform/src/getopt.c, built as its own archive
 * member), and that unit prints its diagnostics through fprintf. The printf family is SYNTHESIZED on pe — the UCRT
 * exports none of it — so the member carried its own copies of the bodies, and so did this program's CU: both
 * STRONG, and the link failed with K_SymbolRedefinedAcrossUnits for printf, fprintf, sprintf, snprintf, sscanf and
 * vfprintf (sqlite's testfixture, which does exactly this, failed the round-close link the same way). With every
 * synthesized body Weak + Hidden, the linker keeps one copy per image. With no arguments getopt returns -1 at once:
 * prints `c=-1`, exits 42. On ELF and Mach-O getopt and printf are ordinary imports, so those arms witness that the
 * program is the portable one. */
#include <stdio.h>
#include <getopt.h>
int main(int argc, char **argv) {
    int c = getopt(argc, argv, "a");
    printf("c=%d\n", c);
    return c == -1 ? 42 : 1;
}
