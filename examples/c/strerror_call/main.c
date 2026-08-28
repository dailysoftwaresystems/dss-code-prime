// TF-C49 (SQLITE_THREADSAFE=0 support): the shipped <string.h> `strerror`
// (C89 7.11.6.2, `char *strerror(int)`) — the symbol SQLite's unixLogErrorAtLine
// references under a NON-threadsafe build (the `#else` branch `zErr =
// strerror(iErrno)`; the default THREADSAFE build takes the `zErr = ""` branch and
// references NEITHER strerror nor strerror_r, which is why the default corpus
// compiled clean without it). Before TF-C49, strerror was absent from string.json
// -> compiling sqlite3.c with --define SQLITE_THREADSAFE=0 failed error[S0001]
// "got strerror". strerror is exported by libc on every format (glibc/msvcrt/
// libSystem `T`) so it needs no availableObjectFormats gate.
//
// RED-ON-DISABLE: delete the strerror row from
// src/dss-config/shippedLibs/string.json -> this example fails to COMPILE with
// error[S0001] got strerror (the exact failure SQLITE_THREADSAFE=0 hit).
#include <string.h>

int keep(int x) { return x; }   // opaque: keeps the errnum a RUNTIME value

int main() {
    // strerror(errnum) returns a non-NULL, non-empty human-readable string for a
    // valid errno (the value/locale of the text is impl-defined, so we assert only
    // the contract: resolved + linked + callable -> a real C string).
    char *m = strerror(keep(2));   // e.g. ENOENT on Linux
    if (m == 0) return 1;          // must be non-NULL
    if (m[0] == 0) return 2;       // must be non-empty (proves it actually ran)
    return 42;
}
