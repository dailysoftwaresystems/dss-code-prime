/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3):
 * `int wmain(void)` alone is a pe64 program's entry, as every Windows reference
 * makes it, and it is WARNED: it is none of the rows the language declares for
 * `wmain`.
 *
 * MEASURED 2026-11-05 (lane `xa`, d136): cl 19.44, clang-cl 19.1.5 and MinGW-w64
 * gcc 13.2 -municode link it and the program runs 22; on Linux gcc 13.3 and clang
 * 18.1.3 refuse the program (no `main`), as DSS does (K_ProgramEntryUndefined).
 * DSS refused it on pe64 (S_EntryShapeNotDeclared was an error): the language has
 * no zero-parameter `wmain` row, deliberately -- such a row would carry the verb
 * `none`, which every format realizes, and put `wmain` back on ELF and Mach-O.
 * Since fold 3e no row is needed: a definition of a name the format realizes is
 * the entry all the same, called with the RICHEST row the format realizes for it
 * (`argc-wargv-wenvp`), its absent parameters receiving nothing they read.
 *
 * RED-ON-DISABLE: drop the semantic tier's richest-row stamp -> the program has
 * no entry (K_ProgramEntryUndefined); make the warning an error -> refused as it
 * was; move the warning off the declarator -> the position reds.
 */
int wmain(void) { return 22; }
