/* c88 (D-RUNTIME-MAIN-ARGC-ARGV): the program-entry argc/argv witness.
 *
 * The runner spawns the artifact with NO extra arguments, so a correct
 * entry trampoline (stack-vector processArgs on the ELF formats) must
 * deliver exactly: argc == 1, argv non-null, argv[0] = the program
 * path (a non-empty C string), argv[argc] == NULL (C 5.1.2.2.1p2).
 * Each check exits with a DISTINCT code so a failure names its layer:
 *
 *   90 — argc wrong (the pre-c88 garbage class: DSS main saw
 *        argc=846361312 where gcc saw the real count — the crash that
 *        stopped the 2-TU sqlite3 shell INSIDE main after c87 cleared
 *        the libm load wall);
 *   91 — argv register garbage (null);
 *   92 — argv[0] slot unreadable/null (the vector address is wrong —
 *        e.g. a load-not-lea would dereference the vector and pass
 *        argv[0] AS argv);
 *   93 — argv[0] empty (points at the wrong bytes);
 *   94 — argv[argc] not NULL (vector shifted / off-by-one-slot).
 *
 * RED-on-disable: remove `processArgs` from the shipped ELF exec
 * format JSON (runtime-read config) and this exits 90 with garbage
 * argc instead of 42.
 */
int main(int argc, char **argv) {
    if (argc != 1) {
        return 90;
    }
    if (!argv) {
        return 91;
    }
    if (!argv[0]) {
        return 92;
    }
    if (!argv[0][0]) {
        return 93;
    }
    if (argv[argc]) {
        return 94;
    }
    return 42;
}
