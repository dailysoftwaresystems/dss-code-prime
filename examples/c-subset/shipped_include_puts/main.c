// FF11 end-to-end proof (2026-06-05): the C-faithful angle include.
//
// This is examples/c-subset/hello_puts/main.c MINUS the inline
// `extern int puts(const char* s);` line. Instead, `#include <stdio.h>`
// pulls the prototype off the SYSTEM search path
// (semantics.shippedLibDirs → src/dss-config/shippedLibs/windows-x86_64/
// stdio.json), exactly like real C: the language-NEUTRAL JSON descriptor
// carries the signature (a hir-text type string), and the linker resolves
// `puts` against the default runtime (msvcrt.dll via externLibraryByFormat).
// NO inline extern anywhere in this file.
//
// What this proves that hello_puts does not:
//   * The angle form `#include <stdio.h>` lexes (HashOp pushes the
//     line-scoped include-directive mode; `<` opens a header path;
//     `>` closes it) and resolves to the neutral descriptor `stdio.json`
//     on the shippedLibDirs system path.
//   * The descriptor's `puts` symbol (name + decoded FnSig) is injected
//     into semantic scope before Pass 2 (so the call resolves with NO
//     inline extern), then flows through FF5 synthesizeFfiFromSourceDecls
//     like a program's own extern (D-FFI-SHIPPED-LIB-DESCRIPTOR-AGNOSTIC).
//
// Expected (mirrors hello_puts): prints "hello\r\n" to captured stdout
// and exits 42. A regression in ANY layer flips the exit code OR the
// captured stdout and the harness fails immediately.

#include <stdio.h>

int main() {
    puts("hello");
    return 42;
}
