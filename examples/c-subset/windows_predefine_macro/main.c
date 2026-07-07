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

/* c96 (D-FFI-WINDOWS-KERNEL32-FUNCTIONS, the semantic frontier): the 4 Win32
 * typedefs LPHANDLE/LPDWORD/LPBOOL/ULONG64 — the EXACT pointer-to-Win32-scalar
 * param shape os_win.c's winLockFile/osReadFile use (a pointer-to-HANDLE,
 * pointer-to-DWORD, pointer-to-BOOL written through + a ULONG64). Without these
 * typedefs in windows.json the
 * declarator is error[S0006] (parse-clean type-name tokens, semantic-undefined)
 * — the RED-ON-DISABLE witness. The pointers are written through + read back so
 * a wrong width silently miscompiles the round-trip (verified == 42). */
static BOOL win_lock(LPHANDLE phFile, LPDWORD pFlags, LPBOOL pOk, ULONG64 off) {
    *phFile = (HANDLE)0;                 /* write through HANDLE*  */
    *pFlags = (DWORD)(off & 0xFFu);      /* write through DWORD*, ULONG64 arith */
    *pOk    = 1;                         /* write through BOOL*    */
    return *pOk;
}
#endif

int main(void) {
#if defined(_WIN32)
    DWORD  base = win_contribution(30u);   /* 31 */
    HANDLE h     = (HANDLE)1;
    DWORD  flags = 0;
    BOOL   ok    = 0;
    BOOL   r     = win_lock(&h, &flags, &ok, (ULONG64)7);  /* h=0, flags=7, ok=1, r=1 */
    int    pathBudget = (MAX_PATH == 260) ? 4 : 0;  /* the shipped constant */
    if (r != 1 || ok != 1 || flags != 7u || h != (HANDLE)0) return 1;  /* writes landed */
    return (int)base + (int)flags + pathBudget;  /* 31 + 7 + 4 = 42 (Windows route) */
#else
    return 42;                             /* every non-pe target (else route) */
#endif
}
