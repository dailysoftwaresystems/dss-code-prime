/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3): a `main`
 * whose signature is none of the rows its format realizes is the program's entry
 * all the same, WARNED at its declarator with a real source span.
 *
 * MEASURED 2026-09-24 on `int main(int, char**, int)`: clang 18.1.3 REFUSES it
 * (`third parameter of 'main' (environment) must be of type 'char **'`), gcc
 * 13.3.0 and mingw-w64 gcc 13.2.0 warn (`third argument of 'main' should probably
 * be 'char **'`, -Wmain) and link it, MSVC 19.51 links it silently. MEASURED
 * 2026-11-05 (lane `xa`, d131) on `int main(int, double)`: the same split -- gcc
 * and cl link and run it, clang and clang-cl refuse it. By the union of the
 * references the program works, so DSS makes it: the definition is entered as
 * every reference's startup calls any `main`, with the RICHEST row its format
 * realizes (argc, argv and the environment; Mach-O's four values), and the third
 * parameter receives the environment's register as an `int`. It used to be
 * refused (S_EntryShapeNotDeclared was an error, `..._refused_positioned`).
 *
 * ★★ THE SPAN IS THE POINT, hence `positioned: true` with an exact line:col. The
 * check's predecessor ran at the MIR tier, where `Mir` carries no BufferId or
 * SourceSpan for a function, so it could only name the entry by symbol name. If
 * the check regressed to a span-less tier this example goes RED.
 *
 * RED-ON-DISABLE: drop the semantic tier's richest-row stamp (the definition then
 * has no verb, so no candidate) and every target refuses the program
 * (K_ProgramEntryUndefined); make the warning an error again and the program is
 * refused as it was; move the warning off the declarator and the position reds.
 */
int main(int argc, char **argv, int envc) {
    (void)argc;
    (void)argv;
    (void)envc;
    return 5;
}
