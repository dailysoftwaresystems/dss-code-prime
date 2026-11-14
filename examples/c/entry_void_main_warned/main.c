/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3):
 * `void main(void)` is the program's entry, as every reference makes it, and it
 * is WARNED at its declarator.
 *
 * MEASURED 2026-11-05 (lane `xa`, d136): cl 19.44 links it silently; gcc 13.3,
 * MinGW-w64 gcc 13.2, clang 18.1.3 and clang-cl 19.1.5 warn (`return type of
 * 'main' is not 'int'`) and link it. Its exit status is whatever the return
 * register held (C23 5.1.2.2.3 leaves it unspecified; the references' programs
 * exited 0, 1, 41, 48 and 1086500280), so this program ends by `exit(9)` and its
 * status is the one it passes. DSS refused it: the language declares no
 * `void` row for `main`, and S_EntryShapeNotDeclared was an error.
 *
 * RED-ON-DISABLE: drop the semantic tier's richest-row stamp -> every target
 * refuses the program (K_ProgramEntryUndefined); make the warning an error ->
 * refused as it was; move the warning off the declarator -> the position reds.
 */
#include <stdlib.h>

void main(void) { exit(9); }
