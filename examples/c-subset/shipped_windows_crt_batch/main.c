/*
 * c98 (Windows-host sub-arc, the 3-header F001A batch): the shipped
 * Microsoft-CRT headers <io.h> + <direct.h> + <process.h> (io.json /
 * direct.json / process.json, availableObjectFormats:[pe], msvcrt.dll),
 * exercised END TO END by live msvcrt calls on the pe route.
 *
 * On a pe target `_WIN32` is predefined (c95), so the Windows branch:
 *   - angle-includes all three headers (each resolves its descriptor; on
 *     the pre-c98 tree every one was error[F001A] F_ShippedHeaderNotFound
 *     — the exact 4-error frontier of the post-c96 sqlite 2-TU re-probe),
 *   - <direct.h>: _getcwd fills a buffer (the live-run witness that needs
 *     no argv — pe argv is D-RUNTIME-PE-MAIN-ARGS, not landed) and
 *     _chdir(".") is a deterministic no-op directory change,
 *   - <io.h>: _access(".",0) probes the always-existing cwd and _unlink
 *     on a never-created name fails cleanly with -1 (never destructive),
 *   - <process.h>: _beginthreadex spawns a REAL msvcrt thread in EXACTLY
 *     sqlite3ThreadCreate's shape (sqlite3.c ~35943): the thread-proc
 *     designator decays to unsigned(__stdcall*)(void*) — `__stdcall`
 *     erases on pe (c95) — and the uintptr_t (u64) result casts to void*;
 *     the proc terminates via a live _endthreadex(0), mirroring
 *     sqlite3ThreadProc (~35916). No join is needed: the proc touches no
 *     shared state, and process exit reaps it whatever its progress.
 *
 * ALL targets exit 42, but the pe route DERIVES every point from a live
 * CRT-call result (10+10+5+5+4+8), so a wrong signature, a failed msvcrt
 * import, or a miscompiled call chain cannot reach 42; every non-pe
 * target compiles the trivial `#else` branch (the three descriptors are
 * pe-only, and the guarded includes never resolve off-pe) — proving the
 * gate adds no elf/macho/arm64 regression. RED-ON-DISABLE: with any of
 * the three descriptors removed the pe compile fails with that header's
 * exact F001A — witnessed in the c98 scratchpad (red_on_disable.txt).
 */
#if defined(_WIN32)
#include <io.h>
#include <direct.h>
#include <process.h>

/* sqlite3ThreadProc's exact shape: __stdcall erases on pe; the live
 * _endthreadex(0) terminates the spawned thread (the return is NOT
 * reached, same as sqlite's "NOT REACHED" tail). */
static unsigned __stdcall thread_proc(void *pArg) {
    (void)pArg;
    _endthreadex(0u);
    return 0u; /* NOT REACHED */
}
#endif

int main(void) {
#if defined(_WIN32)
    char cwd[260];
    unsigned id = 0;
    void *tid;
    int score = 0;
    cwd[0] = 0;
    if (_getcwd(cwd, 260) != 0) score += 10;  /* <direct.h> live: returns the buffer */
    if (cwd[0] != 0)            score += 10;  /* ...and wrote a real path into it    */
    if (_chdir(".") == 0)       score += 5;   /* <direct.h> live: no-op chdir        */
    if (_access(".", 0) == 0)   score += 5;   /* <io.h> live: the cwd exists         */
    if (_unlink("dss_c98_windows_crt_batch_never_created.tmp") == -1)
        score += 4;                           /* <io.h> live: absent file fails cleanly */
    tid = (void*)_beginthreadex(0, 0u, thread_proc, 0, 0u, &id);
    if (tid != 0)               score += 8;   /* <process.h> live: thread spawned    */
    return score;                             /* 10+10+5+5+4+8 = 42 (Windows route)  */
#else
    return 42;                                /* every non-pe target (else route)    */
#endif
}
