// c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS) shipped-name DEDUP witness —
// the shell.c popen/pclose shape: the TU BOTH `#include <stdio.h>` AND
// bare-re-declares a shipped symbol (`int puts(const char *);`). Goal-2 makes
// the user declaration the sole authority (the descriptor's own `puts`
// injection is SUPPRESSED) — so after c86 the bare prototype itself must
// yield the working import: the synthesis reads the suppressed descriptor's
// per-format library map (stdio.json: msvcrt.dll / libc.so.6 / libSystem) and
// binds the SAME libc import the descriptor would have. NOT an undefined
// symbol, NOT a no-library cross-TU row — the descriptor's library rides the
// user's prototype.
//
// Runtime-witnessed: puts must actually reach libc for the line to appear on
// stdout; exit 42 proves the call's return path too (puts returns >= 0 on
// success; a negative/garbage binding flips the exit).
//
// RED-ON-DISABLE (either lever):
//   * drop `prototypeSynthesizesExtern` from the topLevelDecl row -> the
//     bare proto emits nothing -> H0009 at the call (compile failure);
//   * drop the c86 suppressed-library record in the semantic analyzer's
//     goal-2 arm -> the proto synthesizes with NO library -> the linker
//     rejects LOUD: undefined symbol 'puts' (K_SymbolUndefined).

#include <stdio.h>

int puts(const char *s);   // bare re-declaration of the SHIPPED symbol

int main(void) {
    int const rc = puts("shipped-redecl-ok");
    return rc >= 0 ? 42 : 7;
}
