// FF11 end-to-end proof (2026-06-05): the C-faithful angle include.
//
// This is examples/c-subset/hello_puts/main.c MINUS the inline
// `extern int puts(const char* s);` line. Instead, `#include <stdio.h>`
// pulls the prototype off the SYSTEM search path
// (semantics.shippedLibDirs → src/dss-config/shippedLibs/windows-x86_64/
// stdio.h), exactly like real C: the system header carries the
// signature, and the linker resolves `puts` against the default runtime
// (msvcrt.dll via externLibraryByFormat). NO inline extern anywhere in
// this file.
//
// What this proves that hello_puts does not:
//   * The angle form `#include <stdio.h>` lexes (HashOp pushes the
//     line-scoped include-directive mode; `<` opens a header path;
//     `>` closes it) and resolves on the shippedLibDirs system path.
//   * The shipped header is parsed by c-subset's OWN grammar and merged
//     via the existing include-following resolver, so `puts` becomes a
//     real declaration WITH its signature — then flows through FF5
//     synthesizeFfiFromSourceDecls like a program's own extern.
//
// Expected (mirrors hello_puts): prints "hello\r\n" to captured stdout
// and exits 42. A regression in ANY layer flips the exit code OR the
// captured stdout and the harness fails immediately.

#include <stdio.h>

int main() {
    puts("hello");
    return 42;
}
