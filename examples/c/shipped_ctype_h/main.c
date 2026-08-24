/* Runtime witness for shipped <ctype.h> (shippedLibs/ctype.json).
 *
 * WHY THIS EXAMPLE EXISTS: ctype is the one shipped surface whose answers are
 * LOCALE-DEPENDENT — every classifier consults the C runtime's current locale
 * table. That table is populated by the CRT's startup, which DSS's entry does
 * not run on every platform, so a runtime whose locale data is not live can
 * return 0 for EVERYTHING (or nonzero for everything) without faulting: a
 * silent wrong answer, not a crash. Before this example the corpus had no
 * ctype.h user at all, so nothing would have caught it.
 *
 * That is not hypothetical: the pe C runtime is migrating msvcrt.dll ->
 * ucrtbase.dll (D-FFI-PE-CRT-UCRT-MIGRATION), and ctype.json is in the first
 * phase. This example is the permanent per-target guard for that surface.
 *
 * BOTH POLARITIES are asserted deliberately. A null/unpopulated locale table
 * typically answers 0 for every query, which the positive checks catch; a
 * table filled with the wrong mask answers nonzero for everything, which only
 * the negative checks catch. Testing one direction would leave the other
 * failure mode invisible.
 *
 * ANTI-FOLD: every argument rides a MUTABLE global (the c11_atomic /
 * c99_tgmath precedent) rather than a literal. DSS does not elide an extern
 * call today regardless — but a literal argument is exactly what would let a
 * future const-fold or builtin-recognition pass answer `isalpha('A')` at
 * COMPILE time, at which point the `release` arm would stop exercising the
 * runtime's locale table and this example would quietly become a tautology.
 * Mutable globals keep the question a runtime one on both arms.
 *
 * A wrong classification exits with its own distinct code (60..83) so a
 * failure names the exact predicate rather than just "not 42".
 */
#include <ctype.h>

int g_upper_a = 'A';
int g_lower_z = 'z';
int g_digit_7 = '7';
int g_space   = ' ';
int g_tab     = '\t';
int g_comma   = ',';
int g_lower_q = 'q';
int g_upper_q = 'Q';

int main(void) {
    /* --- positive classifications --- */
    if (!isalpha(g_upper_a))  return 60;
    if (!isalpha(g_lower_z))  return 61;
    if (!isdigit(g_digit_7))  return 62;
    if (!isalnum(g_upper_a))  return 63;
    if (!isalnum(g_digit_7))  return 64;
    if (!isspace(g_space))    return 65;
    if (!isspace(g_tab))      return 66;
    if (!isupper(g_upper_a))  return 67;
    if (!islower(g_lower_z))  return 68;
    if (!ispunct(g_comma))    return 69;
    if (!isprint(g_upper_a))  return 70;
    if (!isxdigit(g_digit_7)) return 71;

    /* --- negative classifications (the all-nonzero failure mode) --- */
    if (isalpha(g_digit_7))   return 72;
    if (isdigit(g_lower_z))   return 73;
    if (isspace(g_upper_a))   return 74;
    if (isupper(g_lower_z))   return 75;
    if (islower(g_upper_a))   return 76;
    if (ispunct(g_lower_z))   return 77;
    if (iscntrl(g_upper_a))   return 78;
    if (isgraph(g_space))     return 79;

    /* --- case mapping (returns a VALUE, not a flag) --- */
    if (toupper(g_lower_q) != 'Q') return 80;
    if (tolower(g_upper_q) != 'q') return 81;
    if (toupper(g_upper_q) != 'Q') return 82;  /* already-upper is identity */
    if (tolower(g_digit_7) != '7') return 83;  /* non-alpha is identity     */

    return 42;
}
