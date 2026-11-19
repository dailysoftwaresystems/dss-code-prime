/* SOURCE of the committed fixtures `tests/link/data/coff_seh_comdat_{a,b}_cl_O2.obj` and `..._a_cl_O2_GyOff.obj` (P69
 * fold 3d, lane p69/xa; D-LK-COFF-COMDAT-UNSUPPORTED-SELECTION, its SEH funclet cells).
 *
 * WHAT IT IS. A C `__try` frame's funclet as cl writes it: a `__finally` body run while an exception unwinds through
 * it is the funclet `<fn>$fin$0`, and an `__except` filter that is an expression is `<fn>$filt$0`, each in its own
 * `.text$x` section that is a COMDAT of selection ASSOCIATIVE to its function's section (hm, cl 19.51 /O2 /GS: the
 * selection holds on CODE as well as on data). Each funclet records that it RAN:
 *   * a `__finally` writes `seh_finally_ran`, and the catching frame above returns 42 only when it reads that value;
 *   * an `__except` filter's comma expression writes `seh_filter_ran` before it accepts, and the handler returns 42
 *     only when it reads it.
 * So a program that returns 42 has run every funclet; an exception that never reached one reads 1.
 *
 * TWO UNITS, ONE SOURCE (`/DSEH_UNIT_A` selects the first):
 *   * unit A defines the EXTERNAL functions -- the parent a link keeps, the only copy -- and a C `inline` pair with
 *     its `extern inline` definitions;
 *   * unit B defines the same `inline` pair, alone. Linked beside A, B's copies are the duplicates the link drops,
 *     and every funclet, `.pdata` and `.xdata` section associative to them goes with them.
 * Built twice over: with function-level linking (each function its own COMDAT; an external one NODUPLICATES) and
 * without (`/Gy-`: an external function in the ordinary `.text$mn`). ✔MEASURED (run 20261104-214436-0900150a): /O2
 * IMPLIES /Gy -- `/O2` and `/O2 /Gy` wrote byte-identical objects -- so the second build states `/Gy-` explicitly. The
 * `inline` definitions are COMDATs of selection ANY under either flag set (`dumpbin /symbols`); the fixtures' reading
 * records them, not an assumption of this file. ✔MEASURED (runs 20261104-214611-5a1e9568, -214704-d4dd3ebb,
 * -214810-38709078): B's `/Gy-` build is its `/O2` build byte for byte -- B defines only the `inline` pair, a COMDAT
 * either way -- so B has ONE fixture, read by both flag sets, and the {/Gy, no /Gy} axis is A's external pair alone.
 *
 * REBUILD: built through probe-reference-cc's keep mode (object mode, each twice, the two reports' md5s equal), the
 * probe's own directory mapped away so the object records no host path:
 *   cl /O2 /GS /MD /std:c17 /DSEH_UNIT_A /Brepro /experimental:deterministic /pathmap:{probe}=X:\dss-fixture
 *                                                                                           -> ..._a_cl_O2.obj
 *   cl /O2 /GS /MD /std:c17 (the rest alike)                                                -> ..._b_cl_O2.obj
 *   the first with /Gy-                                                                     -> ..._a_cl_O2_GyOff.obj
 * (cl 19.51, windows-x86_64-release.) Each object's md5, and the compiler that wrote it, are recorded where the
 * fixtures are read; no guard enforces them.
 */
#include <windows.h>

#define SEH_RAISE 0xE0000041u

extern volatile int seh_finally_ran;
extern volatile int seh_filter_ran;

/* Each frame hands its buffer to this function, defined in unit A. ✔MEASURED (run 20261104-214704-d4dd3ebb): in unit
 * B, where the function is external, the buffer's address leaves the frame, /GS guards it, and the frame's handler is
 * `__GSHandlerCheck_SEH` (which checks the cookie, then dispatches as `__C_specific_handler` does); in unit A, which
 * sees its body, cl puts no cookie in the frame and the handler is `__C_specific_handler`. So B's `inline` copies are
 * other bytes than A's -- an ANY selection asks no equality -- and whichever unit a link takes first decides which
 * handler the kept copies run under. */
void seh_touch(char *frame, unsigned code);

/* The `inline` pair: an inline definition here, and (in each unit) the `extern inline` declaration that makes it the
 * unit's external definition -- a copy per unit. */
inline int seh_inline_finally(unsigned code) {
    char frame[32];
    seh_touch(frame, code);
    __try {
        RaiseException(code, 0, 0, 0);
    } __finally {
        seh_finally_ran += 9;
    }
    return frame[0];
}
inline int seh_inline_except(unsigned code) {
    char frame[32];
    int r = 1;
    seh_touch(frame, code);
    __try {
        RaiseException(code, 0, 0, 0);
    } __except (seh_filter_ran += 9, GetExceptionCode() == SEH_RAISE ? EXCEPTION_EXECUTE_HANDLER
                                                                     : EXCEPTION_CONTINUE_SEARCH) {
        r = seh_filter_ran == 9 ? 42 : 1;
    }
    return r + frame[0] - (char)code;
}
extern inline int seh_inline_finally(unsigned code);
extern inline int seh_inline_except(unsigned code);

#if defined(SEH_UNIT_A)
volatile int seh_finally_ran = 0;
volatile int seh_filter_ran = 0;

__declspec(noinline) void seh_touch(char *frame, unsigned code) { frame[0] = (char)code; }

/* The external pair: the parent a link keeps. */
__declspec(noinline) int seh_ext_finally(unsigned code) {
    char frame[32];
    seh_touch(frame, code);
    __try {
        RaiseException(code, 0, 0, 0);
    } __finally {
        seh_finally_ran += 7;
    }
    return frame[0];
}
__declspec(noinline) int seh_ext_except(unsigned code) {
    char frame[32];
    int r = 1;
    seh_touch(frame, code);
    __try {
        RaiseException(code, 0, 0, 0);
    } __except (seh_filter_ran += 7, GetExceptionCode() == SEH_RAISE ? EXCEPTION_EXECUTE_HANDLER
                                                                     : EXCEPTION_CONTINUE_SEARCH) {
        r = seh_filter_ran == 7 ? 42 : 1;
    }
    return r + frame[0] - (char)code;
}

/* The catching frames: each takes the exception a `__finally` lets through, and reads what the funclet wrote. A
 * constant filter is no funclet, so these frames name none. */
__declspec(noinline) static int seh_catch(int (*fn)(unsigned), int expected) {
    seh_finally_ran = 0;
    __try {
        (void)fn(SEH_RAISE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return seh_finally_ran == expected ? 42 : 1;
    }
    return 2;
}

/* What a program's `main` calls: 42 when the four funclets ran (the external pair's, then the kept `inline` copies'),
 * 10 + k when funclet k did not. */
int seh_comdat_check(void) {
    if (seh_catch(seh_ext_finally, 7) != 42) return 10;
    seh_filter_ran = 0;
    if (seh_ext_except(SEH_RAISE) != 42) return 11;
    if (seh_catch(seh_inline_finally, 9) != 42) return 12;
    seh_filter_ran = 0;
    if (seh_inline_except(SEH_RAISE) != 42) return 13;
    return 42;
}
#endif
