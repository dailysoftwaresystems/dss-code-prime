/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: the MACH-O counterpart of
 * `entry_wmain_only_refused_elf` — a `wmain`-only program built for Darwin has NO
 * program entry, and the refusal must say so honestly.
 *
 * ★★★ WHY THIS EXAMPLE EXISTS SEPARATELY FROM THE ELF ONE, rather than being
 * assumed from it. The sibling `wmain_argc` example used to carry TWO Mach-O
 * targets, and they PASSED — vacuously. dyld hands an `LC_MAIN` entry a NARROW
 * `char**` argv; read through an `unsigned short**` parameter, `argv[0][0]` picks
 * up the first TWO ASCII bytes of the program path as one non-zero `u16`, so the
 * example's "argv[0] is non-empty" layer was satisfied by a coincidence of
 * endianness and path spelling rather than by a wide vector, which Darwin does not
 * produce at all. Those arms were removed for exactly that reason: a green test
 * that cannot fail for the right reason is worse than no test.
 *
 * ⚠ REMOVING THEM TOOK DARWIN FROM FALSELY-GREEN TO NO COVERAGE, and that hole is
 * what this file closes. "It is refused on ELF, so it must be refused on Mach-O"
 * is an INFERENCE, and the two formats reach the answer through different config:
 * ELF declares a `processArgs` stack-vector mechanism while the Darwin execs
 * declare NO `processArgs` at all (dyld has already filled the argument registers
 * before any DSS code runs). A refusal that depended on the mechanism being
 * present would pass on ELF and silently accept here. MEASURED 2026-08-10 on both
 * Darwin execs: rc=1, `K_ProgramEntryUndefined`, naming the near miss —
 * `'wmain' is defined here but needs the 'argc-wargv' materialization verb, which
 * this format does not realize`. So candidacy really is decided by the declared
 * verb set and not by the mechanism, on a format where the two differ.
 *
 * ⓘ NO SPAN, DELIBERATELY: "this program defines no entry" is a WHOLE-PROGRAM fact
 * (in a multi-CU build one TU cannot know whether another defines `main`), so
 * there is no single declaration at fault. A fabricated span would be trusted; a
 * missing one is not.
 *
 * ⓘ THIS EXAMPLE COMPILES BUT NEVER RUNS, so it is host-independent — the refusal
 * is a cross-compile fact observable from any host, which is why no `runOn` is
 * declared. That is also what makes it a legitimate Darwin pin from a Windows or
 * Linux host, unlike the run-witness arms it replaces.
 *
 * RED-ON-DISABLE: add "argc-wargv" to macho64-arm64-darwin-exec.format.json's
 * `entryVerbs` and this example stops erroring — `wmain` would then survive the
 * intersection and be selected as the Darwin entry, whose wide argv dyld never
 * supplies. That is the defect this example exists to keep closed.
 */
int wmain(int argc, unsigned short **argv) {
    (void)argc;
    (void)argv;
    return 7;
}
