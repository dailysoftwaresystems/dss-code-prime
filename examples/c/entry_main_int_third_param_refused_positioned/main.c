/* D-RUNTIME-MAIN-ENVP-ENTRY-SHAPE: a `main` whose signature matches no declared
 * entry row is refused AT ITS DECLARATOR, with a real source span.
 *
 * ★ THIS EXAMPLE USED TO REFUSE `int main(int, char**, char**)`, and that form is
 * now SUPPORTED on every exec format (examples/c/entry_main_envp). The refusal
 * mechanism it pinned did not go away with it -- the language still declares a
 * closed set of entry signatures -- so the pin moved to a third parameter no
 * reference gives a meaning. MEASURED 2026-09-24 on `int main(int, char**, int)`:
 * clang 18.1.3 REFUSES it (`third parameter of 'main' (environment) must be of type
 * 'char **'`), gcc 13.3.0 and mingw-w64 gcc 13.2.0 warn (`third argument of 'main'
 * should probably be 'char **'`, -Wmain), MSVC 19.51 accepts it silently -- and
 * where it is accepted the parameter receives the environment pointer's register,
 * reinterpreted as an int (INFERRED from the calling conventions). A value no
 * reference defines is not a form that works, so DSS refuses it.
 *
 * ★★ THE SPAN IS THE POINT, hence `positioned: true` with an exact line:col. The
 * check's predecessor ran at the MIR tier, where `Mir` carries no BufferId or
 * SourceSpan for a function, so it could only name the entry by symbol name. The
 * check now runs at the SEMANTIC tier and points at the declarator; if it ever
 * regressed to a span-less tier this example goes RED even though the same code
 * would still be emitted.
 *
 * ⓘ THE CHECK IS FORMAT-INDEPENDENT: no format realizes a shape the language does
 * not declare, so the target is irrelevant to the question and a relocatable `.o`
 * is refused too.
 *
 * RED-ON-DISABLE: add a row `fn(i32, ptr-ptr-char, i32) -> i32` to `main` in
 * c.lang.json and this stops erroring -- and the language loader refuses that edit
 * first, because no verb materializes that parameter list (the verb/signature
 * coherence rule), which is the guard behind the guard.
 */
int main(int argc, char **argv, int envc) {
    (void)argc;
    (void)argv;
    (void)envc;
    return 5;
}
