/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: Darwin's four-parameter `main` is NOT a program
 * entry on the ELF and pe formats, and the build says so instead of handing the
 * fourth parameter garbage.
 *
 * WHY IT IS REFUSED HERE, MEASURED 2026-09-24: no ELF or Windows loader hands an
 * entry a fourth vector. On x86_64 Linux clang 18.1.3 rejects the form outright
 * (`too many parameters (4) for 'main': must be 0, 2, or 3`), while gcc 13.3.0
 * accepts it with a -Wmain warning and glibc's startup leaves garbage in the
 * fourth parameter; MSVC 19.51 compiles it and passes garbage there too. So no
 * reference makes the form WORK off Darwin, and DSS refuses it.
 *
 * HOW: the language declares the four-parameter row (c.lang.json, verb
 * `argc-argv-envp-apple`), so the SIGNATURE is legal C for this implementation
 * and the semantic tier accepts it -- but a row is a CANDIDATE only where the
 * active format realizes its verb, and only the Mach-O formats list this one. With
 * no candidate left the build fails with K_ProgramEntryUndefined, a whole-program
 * fact with no single declaration at fault (hence no span).
 *
 * RED-ON-DISABLE: add "argc-argv-envp-apple" to elf64-x86_64-linux-exec's
 * `entryVerbs` and the ELF target is refused AT FORMAT LOAD instead -- the
 * stack-vector mechanism produces at most (argc, argv, envp) -- rather than
 * building a program whose fourth parameter is garbage.
 */
int main(int argc, char **argv, char **envp, char **apple) {
    (void)argc;
    (void)argv;
    (void)envp;
    (void)apple;
    return 7;
}
