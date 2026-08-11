/* c111 (D-RUNTIME-PE-MAIN-ARGS): the WIDE program-entry witness — the Windows
 * `wmain(int, wchar_t**)` counterpart of the narrow `main_argc_argv`.
 *
 * On the MSVC profile the real Windows entry is `wmain` (shell.c does exactly this
 * for sqlite), and its argv is a WIDE vector (`wchar_t**` = `unsigned short**`,
 * the pe wide-char). The PE OS entry carries NO argument vector, so a MIR-tier
 * synthesized pre-main init (`realizeEntryShape`) calls the UCRT WIDE populate
 * export `_configure_wide_argv(1)` and dereferences `__p___wargv` (plus the SHARED
 * `__p___argc` — MEASURED: argc is common to the narrow and wide worlds) to obtain
 * argc/argv, then forwards them to wmain. The WIDE arm is chosen by the RESOLVED
 * ENTRY'S SIGNATURE — this entry matches the SOURCE LANGUAGE's declared `wmain` row
 * `fn(i32, ptr-ptr-u16) -> i32`, whose materialization verb is `argc-wargv`, and
 * `pe64-x86_64-windows-exec` is the only shipped format that lists `argc-wargv` in
 * its `entryVerbs` — never a format flag. That INTERSECTION is also why this example
 * is pe64-only rather than merely untested elsewhere: on ELF and Mach-O the `wmain`
 * row does not survive candidate selection at all, so a `wmain`-only source there has
 * NO program entry (`K_ProgramEntryUndefined`, pinned by
 * `entry_wmain_only_refused_elf` and `entry_wmain_only_refused_macho`). UCRT-P4 replaced c111's msvcrt `__wgetmainargs` here because
 * `__getmainargs`/`__wgetmainargs` are msvcrt-ONLY exports (MEASURED 2026-08-10:
 * ucrtbase exports NEITHER) and could not survive the pe CRT migration.
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
 * mismatch), so this witness declares only the pe64 target. UCRT-P4 removed a
 * MACH-O target the manifest had declared in contradiction of this very
 * sentence -- see expected.json for what the pre-gate acceptance was actually
 * doing (reading dyld's narrow char** vector through an `unsigned short**`
 * parameter, which the four checks below survive by accident). RED-on-disable:
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
