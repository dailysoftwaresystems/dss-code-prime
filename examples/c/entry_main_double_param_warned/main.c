/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3): a
 * `main` whose second parameter is no pointer at all is the program's entry, as
 * gcc and MSVC make it, and it is WARNED at its declarator.
 *
 * MEASURED 2026-11-05 (lane `xa`, d131): `int main(int, double)` -- MinGW-w64 gcc
 * 13.2 and gcc 13.3 warn (`second argument of 'main' should be 'char **'`,
 * -Wmain), link it and the program runs 11; cl 19.44 links it silently and it
 * runs 11; clang 18.1.3 and clang-cl 19.1.5 refuse it. By the union the program
 * works, so DSS makes it: entered with the RICHEST row its format realizes, the
 * `double` receiving nothing it reads. DSS refused it (S_EntryShapeNotDeclared was
 * an error). Its parameter is of a type no entry shape can name, so the warning
 * renders the signature as such -- the other arm of the check than
 * entry_main_int_third_param_warned_positioned's.
 *
 * RED-ON-DISABLE: drop the semantic tier's richest-row stamp -> every target
 * refuses the program (K_ProgramEntryUndefined); make the warning an error ->
 * refused as it was; move the warning off the declarator -> the position reds.
 */
int main(int argc, double d) {
    (void)d;
    return argc > 0 ? 11 : 7;
}
