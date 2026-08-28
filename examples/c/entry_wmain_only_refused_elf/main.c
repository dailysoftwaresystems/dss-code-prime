/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: a `wmain`-only program built for ELF has NO
 * program entry, and the refusal must say so honestly.
 *
 * ★★★ THIS EXAMPLE PINS THE FIX FOR A MEASURED THREE-PART DEFECT. On HEAD
 * 3e86a187 (MEASURED 2026-08-10, build-dbg) this exact source, targeting
 * elf64-x86_64-linux-exec, was refused with a message that:
 *   (i)   ASSERTED `wmain` WAS the program entry on Linux -- it is not, and
 *         candidate selection had no way to know, because it matched entry NAMES
 *         alone and was therefore format-blind;
 *   (ii)  PRESCRIBED A REMEDY that would have MADE it the Linux entry (add a
 *         config row to the ELF format file);
 *   (iii) CITED AN UNRELATED ANCHOR about main's envp parameter.
 * A message that is confidently wrong in three ways is worse than a vague one:
 * the reader follows it, and (ii) would have shipped a Linux binary whose entry
 * expects a wide argument vector no Linux loader produces.
 *
 * ★ WHAT IT SAYS NOW, AND WHY THE NEAR-MISS CLAUSE IS LOAD-BEARING: the program
 * defines no entry this format can start, AND `wmain` is defined but needs the
 * `argc-wargv` verb that this format does not realize. Naming the near miss is
 * the only thing that explains WHY -- without it the reader is told an entry is
 * missing while looking straight at a function that appears to be one.
 *
 * ⓘ gcc's answer to the same source is `undefined reference to 'main'` -- a LINK
 * error with no span. DSS reports the same FACT earlier, with the reason. The
 * link tier's own existence checks (K_SymbolUndefined /
 * K_EntryPointResolvesToExtern) are NOT superseded: they still cover an entry
 * arriving from an object DSS never compiled, where no signature is in the input
 * at all.
 *
 * ⓘ NO SPAN, DELIBERATELY. "This program defines no entry" is a WHOLE-PROGRAM
 * fact -- in a multi-CU build one TU cannot know whether another defines `main`
 * -- so there is no single declaration at fault. A fabricated span would be
 * trusted; a missing one is not.
 *
 * RED-ON-DISABLE: add "argc-wargv" to elf64-x86_64-linux-exec.format.json's
 * `entryVerbs` and this example stops erroring -- `wmain` would then survive the
 * intersection and be selected as the ELF entry, which is precisely the defect
 * this example exists to keep closed.
 */
int wmain(int argc, unsigned short **argv) {
    (void)argc;
    (void)argv;
    return 7;
}
