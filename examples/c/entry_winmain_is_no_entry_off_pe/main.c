/* P69 fold 3f (lane xa; D-LK-PE-IMAGE-CANNOT-HONOUR-EVERY-LINKER-DIRECTIVE item (6)): a `WinMain`-only program built
 * for ELF or Mach-O has NO program entry, and is refused as one.
 *
 * `WinMain` is the entry the C runtime's GUI startup calls, and only pe64-x86_64-windows-exec realizes its verb
 * (`instance-prev-cmdline-show`): on every other format it is an ordinary function, so this program defines no
 * entry there. ✔MEASURED 2026-11-17 (lane xa): gcc 13.3.0 and clang 18.1.3 on x86_64 Linux refuse the link
 * ("undefined reference to `main'"), and Apple clang 21 on arm64 macOS refuses it ("Undefined symbols for
 * architecture arm64: _main"). DSS says the same fact earlier, naming the near miss: `WinMain` is defined but needs
 * a verb this format does not realize.
 *
 * No span, deliberately: "this program defines no entry" is a WHOLE-PROGRAM fact.
 */
int WinMain(void *instance, void *previous, char *commandLine, int show) {
    (void)instance;
    (void)previous;
    (void)commandLine;
    (void)show;
    return 44;
}
