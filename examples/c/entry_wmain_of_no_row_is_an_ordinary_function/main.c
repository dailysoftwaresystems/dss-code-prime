/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3): on a
 * format that realizes no row of `wmain` -- every ELF and Mach-O exec document --
 * a function named `wmain` is an ORDINARY function, whatever its signature, and
 * the program's entry is its `main`.
 *
 * MEASURED 2026-11-05 (lane `xa`, d131): `int wmain(int, unsigned int **)`
 * (wchar_t is 32 bits off Windows) beside this `main` is linked by gcc 13.3 and
 * clang 18.1.3 on Linux without a word, and the program runs `main` (11). DSS
 * refused the unit (S_EntryShapeNotDeclared: "'wmain' is a PROGRAM ENTRY name for
 * this source language"): its entry check read the language's names without the
 * format's realized rows, a second reader of the one entry vocabulary. It now asks
 * that vocabulary (`realizedEntryNames`), so it says nothing here.
 *
 * pe64 realizes `wmain`'s rows, so there this `wmain` is an entry candidate (warned
 * as none of them, ranked below `main` by the document's precedence):
 * examples/c/entry_wmain_of_no_row_beside_main_pe pins that cell.
 *
 * RED-ON-DISABLE: make the entry check read every row the language declares
 * instead of the format's realized ones and every target warns
 * S_EntryShapeNotDeclared at `wmain` (forbidden below).
 */
int wmain(int argc, unsigned int **argv) {
    (void)argc;
    (void)argv;
    return 22;
}

int main(void) { return 11; }
