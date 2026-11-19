/* THE TWO UNWINDER ENTRY POINTS A mingw-w64 gcc `-fexceptions` C OBJECT NAMES, which gcc takes from libgcc_eh,
 * statically, by default -- a unit of the pe formats' `compilerSupport` row. ✔MEASURED 2026-10-28 (nm, mingw-w64 gcc
 * 13.2.0 -fexceptions, a C function whose local has a `cleanup`, at -O0 and -O2): the object's unwind data names
 * `__gcc_personality_seh0` as the frame's handler, with the LSDA inline as the handler data, and the landing pad that
 * runs the cleanup ends in a call of `_Unwind_Resume`.
 *
 * ★ WHAT gcc's OWN RUNTIME DOES WITH A FOREIGN EXCEPTION, AND SO WHAT THIS DOES. ✔MEASURED 2026-10-28 (probe-reference-
 * cc run 20261029-012510-b6ca0483, windows-x86_64-debug, MinGW-W64 gcc 13.2.0 and its libgcc, -O0 -fexceptions): a
 * RaiseException passing through a frame with a `cleanup` local on its way to an `__except`-shaped scope above it
 * reaches that scope's filter and then its handler, and the cleanup does NOT run (on the normal path it runs once).
 * The personality passes over every exception it did not raise itself, in both phases -- and it cannot do otherwise:
 * running the landing pad means unwinding to this frame, and the original unwind's TARGET FRAME, which resuming it
 * afterwards would need, is handed to no handler (a DISPATCHER_CONTEXT carries the target's ip and not its frame).
 * LLVM's libunwind states the same choice for the same reason (READ). So a foreign exception passes over here too.
 *
 * ⚠ libgcc's OWN EXCEPTIONS ARE REFUSED, LOUD. An exception raised in libgcc's protocol (its three codes, user-defined
 * bit, "GCC" in the low bytes: a throw, an unwind, a forced unwind) comes only from a gcc C++ unwinder, which DSS
 * does not ship; handling it -- a search phase, a landing pad, a resumed unwind -- is the C++ runtime's protocol.
 * Passing over it would skip the cleanups a libgcc personality runs, in silence, so the process ends instead, saying
 * why. `_Unwind_Resume` is reached only from a landing pad, which this personality never enters: reaching it is the
 * same refusal. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

/* libgcc's exception codes: the user-defined bit, a kind in the top byte, then 'G' 'C' 'C'. */
static int isGccProtocol(DWORD code) {
    return code == 0x20474343u || code == 0x21474343u || code == 0x22474343u;
}

static void refuse(char const *what, DWORD code) {
    fprintf(stderr,
            "DSS runtime: %s (exception code 0x%08lx): an exception of libgcc's own protocol is a gcc C++ "
            "unwinder's, and DSS ships none, so its landing pads cannot run\n",
            what, (unsigned long)code);
    fflush(stderr);
    abort();
}

EXCEPTION_DISPOSITION __gcc_personality_seh0(EXCEPTION_RECORD *record, void *establisherFrame, PCONTEXT context,
                                             DISPATCHER_CONTEXT *dispatch) {
    (void)establisherFrame;
    (void)context;
    (void)dispatch;
    if (isGccProtocol(record->ExceptionCode)) refuse("a gcc exception reached __gcc_personality_seh0",
                                                     record->ExceptionCode);
    return ExceptionContinueSearch;
}

void _Unwind_Resume(void *exception) {
    (void)exception;
    refuse("_Unwind_Resume was called, and no landing pad was entered by DSS's personality", 0);
}
