/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: BOTH entry spellings defined, and on every
 * format that does NOT realize a wide argument vector this must BUILD and run
 * `main`.
 *
 * WHY THIS EXAMPLE EXISTS. Program-entry candidate selection used to match entry
 * NAMES alone, so it was FORMAT-BLIND, and this source was REFUSED everywhere as
 * an ambiguous entry (MEASURED 2026-08-10 on HEAD 3e86a187: K_SymbolUndefined,
 * "ambiguous user-entry", citing an anchor about calling conventions). That was
 * wrong on ELF and Mach-O: `wmain` needs the `argc-wargv` materialization verb,
 * those formats do not realize it, so `wmain` is an ORDINARY FUNCTION there and
 * exactly one candidate survives.
 *
 * Selection now intersects the LANGUAGE's declared entry rows (c-subset.lang.json
 * `entryFunctions`: `main` -> {none, argc-argv}, `wmain` -> {argc-wargv}) with the
 * FORMAT's declared `entryVerbs`. Two declared sets, one intersection, no
 * format-identity branch anywhere.
 *
 * ★ THE EXIT CODE IS THE WITNESS AND IT IS NOT INTERCHANGEABLE. `main` returns 4
 * and `wmain` returns 7. A green run at 4 proves `main` was selected; if the
 * wrong candidate were ever chosen the program would exit 7, which is a DISTINCT
 * observable rather than a crash — so this example fails LOUD on a wrong
 * selection instead of merely failing to build.
 *
 * ★ THE SAME SOURCE IS DELIBERATELY REFUSED ON pe64, and that is not an
 * inconsistency: pe64 realizes BOTH verbs (the UCRT publishes a wide vector), so
 * both candidates survive and picking one silently is the defect. That half is
 * pinned by the sibling example `entry_main_and_wmain_ambiguous_pe`. The two
 * examples together are the actual claim: candidacy is per-format DATA.
 *
 * `argv` is spelled `unsigned short**`, never `wchar_t**` — wchar_t is 16-bit on
 * Windows and 32-bit on Linux, so a wchar_t spelling would silently change shape
 * per platform while the entry ABI does not.
 *
 * RED-ON-DISABLE: add "argc-wargv" to elf64-x86_64-linux-exec.format.json's
 * `entryVerbs` and this example stops building — two candidates then survive on
 * ELF and the ambiguity refusal fires, exactly as it does on pe64 today.
 */
int wmain(int argc, unsigned short **argv) {
    (void)argc;
    (void)argv;
    return 7;   /* never selected on these formats -- see the docblock */
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 4;
}
