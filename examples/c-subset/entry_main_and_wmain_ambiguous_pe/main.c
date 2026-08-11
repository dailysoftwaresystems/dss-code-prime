/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: BOTH entry spellings defined, built for
 * pe64 -- which realizes BOTH materialization verbs, so BOTH are program-entry
 * candidates and the build must be REFUSED.
 *
 * This is the deliberate counterpart of `entry_main_and_wmain_takes_main`, which
 * builds the IDENTICAL construct successfully on ELF and Mach-O. The difference
 * is not a special case in the compiler: pe64-x86_64-windows-exec declares
 * `argc-wargv` in its `entryVerbs` because the UCRT publishes a wide argument
 * vector (`_configure_wide_argv` + `__p___wargv`), and no ELF or Mach-O format
 * declares it because nothing on those platforms produces one. Two candidates
 * survive the intersection here; one survives there.
 *
 * ★ WHY REFUSING IS THE ONLY ACCEPTABLE ANSWER. A program has exactly one entry.
 * Choosing between two rival entries by declaration order, or by whichever the
 * symbol walk reaches first, produces a build that RUNS THE WRONG CODE WHILE
 * REPORTING SUCCESS -- the worst outcome available at this seam. It is also
 * exactly what the merged (N>1 CU) path used to do: `mergeCuMirs` took the first
 * matching name it walked past with no ambiguity check at all, so `main` in a.c
 * and `wmain` in b.c silently picked one while a single-CU build of the same two
 * functions was refused. One owner now decides for both paths.
 *
 * ★ THE DIAGNOSTIC'S CODE AND ANCHOR ARE THE POINT, not just its presence. This
 * refusal previously emitted K_SymbolUndefined -- a code about a symbol that does
 * not exist, for a condition where two do -- and cited
 * D-CSUBSET-MULTI-FN-WIN64-CC, an anchor about calling conventions with nothing
 * to do with entry selection. A diagnostic whose code and anchor both point
 * elsewhere sends the reader to the wrong place with full confidence, which is
 * worse than a vague message. Hence K_ProgramEntryAmbiguous.
 *
 * RED-ON-DISABLE: remove "argc-wargv" from pe64-x86_64-windows-exec.format.json's
 * `entryVerbs` and this example stops being an error -- only `main` would survive
 * the intersection, so the build would succeed and the expected diagnostic would
 * never fire.
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
