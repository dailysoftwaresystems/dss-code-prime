/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: a 3-parameter `main` is refused AT ITS
 * DECLARATOR, with a real source span.
 *
 * ★★★ WHAT THIS REFUSES, MEASURED 2026-08-10 on HEAD 3e86a187 (build-dbg): this
 * exact source compiled **rc=0 with ZERO diagnostics** on BOTH
 * pe64-x86_64-windows-exec and elf64-x86_64-linux-exec, and both emitted images
 * FAULT -- observed argc=3, argv=0x...7D10, envp=0x0000000000000004, where
 * dereferencing envp gives 0xC0000005 on pe and SIGSEGV (rc=139) on elf. gcc
 * compiles the identical source and it works. (The ORIGIN of the 0x4 is
 * UNDETERMINED. An earlier probe explained it as the integer argc left in a
 * leftover register; that is REFUTED -- the measured run had argc=3 with envp
 * still 0x4. No mechanism claim is made here.)
 *
 * ★ C23 IS WHAT MAKES THIS A DEFECT RATHER THAN A PREFERENCE. 5.1.2.2.1 permits
 * `main` "in some other IMPLEMENTATION-DEFINED MANNER", so SUPPORTING the
 * 3-parameter form is conforming and REFUSING it is conforming -- accepting it
 * and faulting is the one outcome C23 rules out, and that was the shipped
 * behaviour. 3.4.1 then defines implementation-defined behavior as behavior each
 * implementation DOCUMENTS, which is why the accepted set is declared in config
 * (the language's `entryFunctions` mapping) and why the message enumerates it.
 *
 * ★★ THE SPAN IS THE POINT OF THIS EXAMPLE, hence `positioned: true` with an
 * exact line:col. The predecessor check ran at the MIR tier, where `Mir` carries
 * no BufferId or SourceSpan for a function, so it could only name the entry by
 * symbol name -- the weakest possible report of what is a plain declaration
 * mistake with an obvious location. The check now runs at the SEMANTIC tier and
 * points at the declarator. `positioned: true` is what keeps it there: if the
 * check ever regressed to a span-less tier this example goes RED even though the
 * same code would still be emitted.
 *
 * ★ DIVERGENCE, RECORDED NOT MINIMISED: gcc, clang AND MSVC all ACCEPT
 * `int main(int, char**, char**)`. Refusing it IS a real divergence from all
 * three while this anchor stays open. The cost is accepted because a loud refusal
 * beats a binary that faults on the first envp dereference -- and it denies no
 * C23 facility, since getenv (7.24.4.6) ships on every format with no
 * availableObjectFormats gate. POSIX.1 does not specify main's third parameter
 * either; it blesses `extern char **environ`.
 *
 * ⓘ THE CHECK IS FORMAT-INDEPENDENT, so this is refused for a relocatable `.o`
 * too: no format realizes the shape and no later translation unit can make it
 * legal, so the target is irrelevant to the question being asked.
 *
 * RED-ON-DISABLE: add a third "ptr-ptr-char" to `main`'s argc-argv row params in
 * c-subset.lang.json and this example stops erroring -- and the verb/signature
 * coherence rule in the language loader refuses that edit first, which is the
 * guard behind the guard.
 */
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    return 5;
}
