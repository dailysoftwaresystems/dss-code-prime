/* THE x86-64 WINDOWS ROUTINES A REFERENCE TOOLCHAIN'S OWN STATIC RUNTIME DEFINES FOR THE OBJECTS IT COMPILES, which
 * keep the ordinary calling convention -- DSS's half beside `pe_compiler_support_x86_64.s` (the routines whose register
 * contract is narrower than the convention) and `pe_gcc_unwind.c` (gcc's unwinder entry points), the three units of
 * the pe formats' `compilerSupport` row. A FOREIGN object names each routine BY NAME, so it is linked from the
 * shipped runtime archive only when an input leaves it undefined. ✔MEASURED 2026-10-28 (nm over 31 reference
 * objects: cl 19.44 /Od and /O2, clang-cl 19.1 /O2, mingw-w64 gcc 13.2 -O2):
 *   - the /GS family -- `__security_cookie` and `__security_check_cookie` in every /GS function of cl and clang-cl,
 *     `__GSHandlerCheck` as the exception handler cl names for a /GS frame, `__GSHandlerCheck_SEH` for one that also
 *     has `__try`, `__report_rangecheckfailure` where cl cannot prove an array store's index in range (cl /Od, and
 *     cl 19.51 /O2 for an index bounded by a loop condition and then stored past it);
 *   - `__main`, which every mingw-w64 gcc `main` calls first;
 *   - `_fltused`, which cl references from every object that uses floating point;
 *   - gcc's stack protector (`-fstack-protector*`, gcc's analogue of /GS) -- `__stack_chk_guard`, the canary every
 *     protected frame holds, and `__stack_chk_fail`, called when it moved (✔MEASURED 2026-11-03, MinGW-W64 gcc 13.2.0
 *     at -O0 and -O2: gcc takes the guard from libmingwex.a and the reporter from libssp-0.dll, an image a plain
 *     Windows does not have).
 * `__security_init_cookie` seeds BOTH stack cookies -- the /GS cookie and the stack protector's guard -- and is the ONE
 * seeding routine: the format document's `compilerSupport` block names it as the routine DSS's PE startup calls
 * before any initializer and before the entry, so one startup call covers both families. */
#include <stdint.h>
#include <windows.h>

/* The cookie every /GS frame is guarded by, combined with the frame's stack pointer. This initial value is the one
 * MSVC's runtime ships (x64: 0x00002B992DDFA232) -- what the cookie is until `__security_init_cookie` runs, and what
 * that routine refuses to leave in place. */
uintptr_t __security_cookie = 0x00002B992DDFA232ull;

/* Defined in `pe_compiler_support_x86_64.s`: it changes nothing but %r10 and the flags. */
void __security_check_cookie(uintptr_t cookie);

/* A floating-point object of cl's names this so the linker brings in the floating-point support of the runtime that
 * defines it; DSS's runtime needs none, so it is only the definition, with the value MSVC's runtime gives it. */
int _fltused = 0x9875;

/* gcc's `main` calls `__main` first: mingw-w64's runs the `.ctors` table there (`__do_global_ctors`, once) and
 * registers the `.dtors` walk at exit. In a DSS image that is the startup's work, not this routine's: the entry runs
 * the image's whole initializer schedule before `main` and its terminators after it, so `__main` running a table
 * again would run each constructor twice. It is empty by design, and it is not what runs a gcc object's own
 * `.ctors`/`.dtors`: the object reader does not yet turn those tables into the schedule, so their entries do not run
 * (D-LK-FOREIGN-OBJECT-INITIALIZER-TABLES-NEVER-RUN, the reading side). */
void __main(void) {}

/* gcc's stack-protector canary, read by every protected frame on entry and compared on exit. This initial value is
 * DSS's own (mingw-w64's library seeds its guard per run, ✔MEASURED 2026-11-03: three runs, three values) -- what the guard is until
 * `__security_init_cookie` runs, and what that routine refuses to leave in place. */
uintptr_t __stack_chk_guard = 0x595E9FBD94FDA766ull;

/* The stack cookies, seeded: the system time, the process and thread ids, the tick count and the performance counter,
 * mixed with the address of a local for the layout the loader chose. The /GS cookie is kept to 48 bits as MSVC's x64
 * runtime keeps it; the guard takes the whole word, mixed apart from the cookie; neither is left equal to its shipped
 * value, nor the guard zero. A value already seeded is left alone, so a second call changes nothing a guarded frame
 * has stored. ONE routine seeds both families, so the startup's one call covers every guarded frame of the image. */
void __security_init_cookie(void) {
    int const cookieShipped = __security_cookie == (uintptr_t)0x00002B992DDFA232ull;
    int const guardShipped  = __stack_chk_guard == (uintptr_t)0x595E9FBD94FDA766ull;
    if (!cookieShipped && !guardShipped) return;
    FILETIME      now;
    LARGE_INTEGER counter;
    GetSystemTimeAsFileTime(&now);
    QueryPerformanceCounter(&counter);
    uintptr_t mixed = ((uintptr_t)now.dwHighDateTime << 32) | (uintptr_t)now.dwLowDateTime;
    mixed ^= (uintptr_t)GetCurrentThreadId();
    mixed ^= (uintptr_t)GetCurrentProcessId();
    mixed ^= (uintptr_t)GetTickCount64() << 24;
    mixed ^= ((uintptr_t)(uint32_t)counter.QuadPart << 32) ^ (uintptr_t)counter.QuadPart;
    mixed ^= (uintptr_t)&mixed;
    if (cookieShipped) {
        uintptr_t cookie = mixed & 0x0000FFFFFFFFFFFFull;
        if (cookie == (uintptr_t)0x00002B992DDFA232ull) ++cookie;
        __security_cookie = cookie;
    }
    if (guardShipped) {
        uintptr_t guard = (mixed ^ (mixed >> 29) ^ (mixed << 35)) * (uintptr_t)0x9E3779B97F4A7C15ull;
        if (guard == (uintptr_t)0x595E9FBD94FDA766ull || guard == 0) guard ^= (uintptr_t)0xA5A5A5A5A5A5A5A5ull;
        __stack_chk_guard = guard;
    }
}

/* Ends the process at once, running no handler, with the status cl's own runtime ends it with (✔MEASURED 2026-10-28,
 * cl 19.51 /O2 /GS: an overwritten cookie and an out-of-range store both exit 0xC0000409; RaiseFailFastException with
 * this record exits 0xC0000409 too, where a NULL record gives 0xC0000602). */
static void failFast(ULONG_PTR code) {
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode           = STATUS_STACK_BUFFER_OVERRUN;
    record.ExceptionFlags          = EXCEPTION_NONCONTINUABLE;
    record.NumberParameters        = 1;
    record.ExceptionInformation[0] = code;
    RaiseFailFastException(&record, NULL, 0);
}

/* A /GS frame's cookie no longer matched (from `__security_check_cookie`, with the cookie it found). */
void __report_gsfailure(uintptr_t cookie) {
    (void)cookie;
    failFast(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

/* cl's check that an array store's index is in range failed. */
void __report_rangecheckfailure(void) {
    failFast(FAST_FAIL_RANGE_CHECK_FAILURE);
}

/* A gcc stack-protected frame's canary no longer matched `__stack_chk_guard`: the same report as a /GS frame's, so a
 * stack overrun ends the process alike whichever compiler wrote the check. mingw-w64 gcc's own (libssp's) ends it with
 * a trap instead -- exit 0xC000001D, nothing said (✔MEASURED 2026-11-03, -O0 and -O2) -- an exception the dispatcher
 * would walk the overwritten stack for, which a caller's `__try` could catch and go on from; the fast fail runs no
 * handler, Windows' own reason for it. */
void __stack_chk_fail(void) {
    failFast(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
}

/* The cookie check a /GS frame's exception handler makes, from the GS data cl writes after the handler's RVA in the
 * frame's unwind data: the cookie's offset in the frame, whose low three bits are flags (1 check while dispatching,
 * 2 while unwinding, 4 the frame is realigned and two more words follow -- the offset from the establisher frame to
 * the aligned base, and the alignment). The cookie was stored combined with the frame's stack pointer, which is the
 * establisher frame plus, for a function with a frame register, the frame register's offset from it (the high nibble
 * of the unwind data's fourth byte, in 16-byte units). */
static void checkFrameCookie(ULONG const *gs, ULONG_PTR establisherFrame, DISPATCHER_CONTEXT const *dispatch) {
    ULONG_PTR base = establisherFrame;
    if ((gs[0] & 4u) != 0) base = (base + gs[1]) & ~((ULONG_PTR)gs[2] - 1u);
    ULONG_PTR combinedWith = establisherFrame;
    unsigned char const *unwind = (unsigned char const *)(dispatch->ImageBase + dispatch->FunctionEntry->UnwindData);
    if ((unwind[3] & 0x0Fu) != 0) combinedWith += (ULONG_PTR)(unwind[3] >> 4) * 16u;
    uintptr_t const cookie = *(uintptr_t const *)(base + (gs[0] & ~7u)) ^ combinedWith;
    __security_check_cookie(cookie);
}

/* The handler cl names for a /GS frame with no `__try`: the cookie is checked whenever an exception passes through
 * the frame, which then lets it go on. */
EXCEPTION_DISPOSITION __GSHandlerCheck(EXCEPTION_RECORD *record, void *establisherFrame, PCONTEXT context,
                                       DISPATCHER_CONTEXT *dispatch) {
    (void)record;
    (void)context;
    checkFrameCookie((ULONG const *)dispatch->HandlerData, (ULONG_PTR)establisherFrame, dispatch);
    return ExceptionContinueSearch;
}

/* The handler cl names for a /GS frame that also has `__try`: its handler data is the scope table
 * `__C_specific_handler` reads (a count, then sixteen bytes per scope), with the GS data after it. The cookie is
 * checked in the phase the GS data's flags name, then the scope table's handler decides. */
EXCEPTION_DISPOSITION __GSHandlerCheck_SEH(EXCEPTION_RECORD *record, void *establisherFrame, PCONTEXT context,
                                           DISPATCHER_CONTEXT *dispatch) {
    ULONG const *scopes = (ULONG const *)dispatch->HandlerData;
    ULONG const *gs     = scopes + 1 + scopes[0] * 4u;
    ULONG const  phase  = (record->ExceptionFlags & EXCEPTION_UNWIND) != 0 ? 2u : 1u;
    if ((gs[0] & phase) != 0) checkFrameCookie(gs, (ULONG_PTR)establisherFrame, dispatch);
    return __C_specific_handler(record, establisherFrame, context, dispatch);
}
