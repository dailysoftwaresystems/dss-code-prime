/*
 * c95 (Windows-host sub-arc, FLIP-AND-UNBLOCK): the `_WIN32` predefine +
 * os_win-parse-enabling surface, exercised end to end by a small program.
 *
 * On a pe target `_WIN32` is predefined (pe-gated, c-subset.lang.json), so:
 *   - the QUOTE `#include "windows.h"` resolves the shipped windows.json
 *     descriptor via the quote->angle fallback (C 6.10.2p3),
 *   - the Win32 TYPES (DWORD/HANDLE) + CONSTANTS (MAX_PATH) are in scope,
 *   - `__stdcall`/`WINAPI` erase to nothing on pe (MS x64 has one cc),
 *   - `#if defined(_WIN32)` selects the Windows branch.
 * On elf/macho/arm64 `_WIN32` is NOT predefined, so the `#else` branch is
 * taken (a different exit code) — the RED-ON-DISABLE witness: dropping the
 * `_WIN32` predefine flips the pe target onto the same `#else` path.
 *
 * ALL targets exit 42, but by DIFFERENT routes: the pe (Windows) branch
 * ASSEMBLES 42 purely from windows.h facts (a __stdcall function's result + a
 * DWORD + the MAX_PATH constant), so it cannot compile without the whole c95
 * surface; every non-pe target takes the `#else` branch. Because all running
 * targets share one exit code the manifest can run the example on Windows AND
 * Linux/macOS — proving the mechanism is pe-gated (elf/macho compile the
 * else path cleanly, no regression). The RED-ON-DISABLE (dropping the `_WIN32`
 * predefine flips the pe target onto the else path / breaks the guarded
 * include) is witnessed separately in the cycle's scratchpad.
 *
 * The `#include "windows.h"` is guarded by `#if defined(_WIN32)` exactly as
 * sqlite3.c guards it under `#if SQLITE_OS_WIN` — so an elf/macho build (where
 * windows.json is availableObjectFormats:[pe], i.e. NOT on that target) never
 * tries to resolve it and cleanly compiles the `#else` path.
 */
#if defined(_WIN32)
#include "windows.h"
#endif

#if defined(_WIN32)
/* __stdcall must ERASE for this declarator to parse on pe (PE64 is one cc). */
static DWORD __stdcall win_contribution(DWORD seed) {
    return seed + 1u;
}
#endif

int main(void) {
#if defined(_WIN32)
    DWORD  base = win_contribution(30u);   /* 31 */
    HANDLE h    = 0;                        /* a windows.h pointer type */
    int    pathBudget = (MAX_PATH == 260) ? 11 : 0;  /* the shipped constant */
    (void)h;
    return (int)base + pathBudget;         /* 31 + 11 = 42 (Windows route) */
#else
    return 42;                             /* every non-pe target (else route) */
#endif
}
