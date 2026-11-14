/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3): pe64
 * realizes `wmain`'s rows, so a `wmain` of none of them is an entry CANDIDATE
 * there (warned at its declarator), and beside a `main` the format's stated
 * precedence elects `main` (warned, naming `wmain`).
 *
 * MEASURED 2026-11-05 (lane `xa`, d131): `int wmain(int, unsigned int **)` beside
 * this `main` -- cl 19.44 links it with LNK4067 ("ambiguous entry point; selected
 * 'mainCRTStartup'") and MinGW-w64 gcc 13.2 silently; each program runs `main`
 * (11). DSS refused the unit (S_EntryShapeNotDeclared was an error). Off Windows
 * the same source has no warning at all:
 * examples/c/entry_wmain_of_no_row_is_an_ordinary_function.
 *
 * RED-ON-DISABLE: drop the semantic tier's richest-row stamp -> `wmain` is no
 * candidate and the precedence warning stops, which `expectWarnings` reds; make
 * the shape warning an error -> refused as it was.
 */
int wmain(int argc, unsigned int **argv) {
    (void)argc;
    (void)argv;
    return 22;
}

int main(void) { return 11; }
