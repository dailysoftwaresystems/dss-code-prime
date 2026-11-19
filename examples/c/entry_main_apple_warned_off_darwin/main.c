/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE (P69 fold 3e, lane `xa`, class C's N3; renamed
 * from entry_main_apple_refused_off_darwin): Darwin's four-parameter `main` built
 * for the ELF and pe formats is the program's entry all the same, called with the
 * richest row those formats realize (argc, argv, the environment), and WARNED at
 * its declarator.
 *
 * MEASURED 2026-09-24: on x86_64 Linux clang 18.1.3 rejects the form outright
 * (`too many parameters (4) for 'main': must be 0, 2, or 3`), while gcc 13.3.0
 * accepts it with a -Wmain warning and glibc's startup leaves garbage in the
 * fourth parameter; MSVC 19.51 compiles it and passes garbage there too. It used
 * to be refused (K_ProgramEntryUndefined) on the ground that no reference makes
 * the fourth parameter mean anything off Darwin. DECIDED in fold 3e by the same
 * rule as `int main(int, double)` and `int main(int, char**, int)`, which gcc and
 * MSVC link and run: the rows that count on a format are the ones it REALIZES,
 * this definition is none of them, so it is entered as every reference's startup
 * enters any `main` -- with the arguments the format's startup makes -- and the
 * program, which reads no fourth value, runs to 7 under gcc's and MSVC's links and
 * DSS's alike. A program that reads the fourth parameter off Darwin reads what
 * gcc's and MSVC's leave there.
 *
 * RED-ON-DISABLE: add "argc-argv-envp-apple" to elf64-x86_64-linux-exec's
 * `entryVerbs` and the ELF target is refused AT FORMAT LOAD (the stack-vector
 * mechanism produces at most three values); drop the richest-row stamp and every
 * target refuses the program (K_ProgramEntryUndefined).
 */
int main(int argc, char **argv, char **envp, char **apple) {
    (void)argc;
    (void)argv;
    (void)envp;
    (void)apple;
    return 7;
}
