// FF11 shipped system header (windows-x86_64) — the analogue of C's
// <stdio.h> on the default runtime search path. A c-subset SOURCE file
// (extern prototypes), NOT a JSON schema: it is loaded via the existing
// include-following resolver when a program does `#include <stdio.h>`,
// parsed by c-subset's OWN grammar, and merged into the compilation
// unit. Its extern decls then flow through FF5
// `synthesizeFfiFromSourceDecls` exactly like a program's own externs —
// the symbol becomes a real declaration WITH ITS SIGNATURE, and the
// per-format `externLibraryByFormat` (c-subset.lang.json: pe →
// msvcrt.dll) routes it to the runtime import library at link time.
//
// This mirrors the inline `extern int puts(const char* s);` that
// examples/c-subset/hello_puts/main.c declares by hand: the angle
// include is the ADDITIVE C-faithful alternative — a program may EITHER
// write the forward declaration itself OR `#include <stdio.h>`, and
// both resolve `puts` against msvcrt.dll. Keep this signature in lockstep
// with that example.
//
// Platform note: this dir is named windows-x86_64 explicitly. Platform
// auto-select (choosing this vs. a future linux-x86_64 dir from the
// active target) is DEFERRED — anchor D-FFI-SHIPPED-LIB-PLATFORM-SELECT.

extern int puts(const char* s);
