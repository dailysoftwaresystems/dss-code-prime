/* `main` AND `wmain` IN ONE PROGRAM, BUILT FOR pe64 -- which realizes BOTH
 * materialization verbs, so BOTH are program-entry candidates -- STARTS AT
 * `main`, with a warning naming `wmain` (P69 fold 3e, the review of fold 3d's
 * X2). It used to be refused as ambiguous (`entry_main_and_wmain_ambiguous_pe`,
 * which this example replaces).
 *
 * ★ WHY `main`: every reference linker of the format takes it. MEASURED (P69
 * fold 3e, lane xa), this pair in one unit, in two units and in a unit beside an
 * object: link.exe 14.44 links and runs `main` with LNK4067 ("ambiguous entry
 * point; selected 'mainCRTStartup'"), lld-link 19.1.5 too ("found both wmain and
 * main; using latter"), and MinGW-w64 GNU ld 2.42 runs `main` unless
 * `-municode` names the wide startup. The choice is not an order DSS picks: the
 * format STATES that the narrow startup outranks the wide one
 * (`pe64-x86_64-windows-exec.format.json`'s `entryPrecedence`), and two
 * candidates of one rank -- two `main`s -- stay refused.
 *
 * THE EXIT CODE IS THE WITNESS: `main` returns 4 and `wmain` 7, so the wrong
 * choice is a distinct exit, as in `entry_main_and_wmain_takes_main` (the ELF and
 * Mach-O half, where `wmain` is no candidate at all and no warning is due).
 *
 * RED-ON-DISABLE: remove `entryPrecedence` from the pe64 exec document and this
 * example is refused K_ProgramEntryAmbiguous.
 */
int wmain(int argc, unsigned short **argv) {
    (void)argc;
    (void)argv;
    return 7;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 4;
}
