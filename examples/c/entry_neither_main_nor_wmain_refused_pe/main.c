/* D-DIAG-ENTRY-RESOLUTION-CODE-SPLIT-CORPUS-COVERAGE: the pe64 arm of
 * K_ProgramEntryUndefined -- an exec build whose translation unit defines
 * NEITHER `main` NOR `wmain`.
 *
 * WHY pe NEEDS ITS OWN INPUT: the elf and Mach-O arms
 * (entry_wmain_only_refused_elf / _macho) refuse a `wmain`-only program because
 * those formats do not realize the wide verb, so `wmain` is simply not a
 * candidate there. pe64-x86_64-windows-exec DOES realize it -- a `wmain`-only
 * program BUILDS on pe -- so the only way to reach this code on pe is a program
 * with no entry spelling at all, which this is.
 *
 * ⓘ NO SPAN, DELIBERATELY. "This program defines no entry" is a WHOLE-PROGRAM
 * fact -- in a multi-CU build one TU cannot know whether another defines `main`
 * -- so there is no single declaration at fault.
 *
 * RED-ON-DISABLE: rename `helper` to `main` and this example stops erroring (the
 * program then has its entry).
 */
int helper(int x) {
    return x + 1;
}
