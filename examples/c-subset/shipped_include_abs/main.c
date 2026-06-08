// FF11 build-out proof (2026-06-08): a NON-stdio shipped library resolves
// end-to-end. The cycle-21 `shipped_include_puts` example proves only
// `stdio.json`; this proves the broader shippedLibs build-out (string /
// ctype / math / stdlib) is LIVE on the system search path, not merely
// well-formed JSON that decodes in a unit test.
//
// `#include <stdlib.h>` pulls `abs`'s prototype off the system search path
// (semantics.shippedLibDirs → src/dss-config/shippedLibs/windows-x86_64/
// stdlib.json) with NO inline extern anywhere in this file — exactly like
// real C. The language-neutral descriptor carries the signature
// (`fn(i32) -> i32`, a hir-text type string); the descriptor-injected
// `abs` extern is synthesized before Pass 2, flows through FF5
// synthesizeFfiFromSourceDecls, and the linker resolves `abs` against the
// default runtime (msvcrt.dll via externLibraryByFormat).
//
// What this proves that shipped_include_puts does not:
//   * The angle resolver maps a DIFFERENT stem (`<stdlib.h>` → stdlib.json,
//     not `<stdio.h>` → stdio.json) — the resolution is generic, not
//     hardcoded to one header.
//   * A descriptor authored in THIS build-out (stdlib.json) produces an
//     extern that actually links and runs — a decode-only unit test cannot
//     prove the synthesized extern links against the runtime.
//
// The `abs` call is an extern (opaque to the optimizer): `7 - 49` may const
// fold to `-42`, but the CALL itself cannot — so the optimized pipeline
// genuinely executes a runtime FFI call. abs(-42) = 42 → exit 42. A
// regression in ANY layer (resolver / injection / synthesis / link) flips
// the exit code and the harness fails immediately.

#include <stdlib.h>

int main() {
    return abs(7 - 49);
}
