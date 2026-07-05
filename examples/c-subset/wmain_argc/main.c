/* c111 (D-RUNTIME-PE-MAIN-ARGS): the WIDE program-entry witness — the Windows
 * `wmain(int, wchar_t**)` counterpart of the narrow `main_argc_argv`.
 *
 * On the MSVC profile the real Windows entry is `wmain` (shell.c does exactly this
 * for sqlite), and its argv is a WIDE vector (`wchar_t**` = `unsigned short**`,
 * the pe wide-char). The PE OS entry carries NO argument vector, so a MIR-tier
 * synthesized pre-main init (synthesizePeStartup) calls the msvcrt WIDE arg-fetch
 * export `__wgetmainargs` (chosen over the narrow `__getmainargs` by the argv
 * ELEMENT width — u16 here) to populate argc/argv, then forwards them to wmain.
 *
 * The runner spawns with NO extra args, so a correct wide arg-fetch delivers
 * exactly: argc == 1, argv non-null, argv[0] = the program path (a non-empty WIDE
 * C string — argv[0][0] is its first u16 code unit, non-zero), argv[argc] == NULL.
 * Each check exits a DISTINCT code so a failure names its layer:
 *   90 argc wrong · 91 argv null · 92 argv[0] null · 93 argv[0] empty ·
 *   94 argv[argc] not NULL.
 *
 * pe64-ONLY: `wmain`+wide-argv is a Windows construct; on ELF the stack-vector
 * mechanism would hand a NARROW char** vector to this wide entry (a category
 * mismatch), so this witness declares only the pe64 target. RED-on-disable:
 * remove `processArgs` from the pe64 shipped exec JSON -> the synth never runs ->
 * wmain reads entry-register garbage argc -> exit 90 instead of 42.
 */
int wmain(int argc, unsigned short **argv) {
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
