/* D-CSUBSET-VLA-WIN64-UNWIND: the STACK-WALK witness for a pe64 VLA frame.
 *
 * ★ WHY A WALK AND NOT A BUILD. `.pdata`/`.xdata` bytes that assemble and link
 * prove only that bytes were emitted. What has to be true is that the OS unwinder,
 * handed those bytes, reconstructs the caller's RSP and non-volatile registers from
 * a frame whose stack pointer moved by a RUNTIME amount. Only a real dispatch
 * through the frame can say that, and this program forces one:
 *
 *     main  --__try-->  level1  -->  vla_frame  -->  trap   (access violation)
 *
 * `vla_frame` holds a VLA OBJECT and MAKES CALLS — the frame shape
 * D-CSUBSET-VLA-NONLEAF-CALL-FRAME builds — and it is a MIDDLE frame in the walk,
 * neither the faulting one nor the handling one, so the OS must unwind THROUGH it
 * twice: once virtually (searching for a handler) and once for real (RtlUnwindEx,
 * restoring the context main's __except body resumes on).
 *
 * `vla_frame`'s UNWIND_INFO recovers RSP through UWOP_SET_FPREG (frame register rbp,
 * frame offset 0) — the dynamic `sub rsp,<size>` carries no unwind code at all, and
 * needs none, because RSP is read back out of the frame register.
 *
 * ⚠ WHAT THIS PROGRAM DOES **NOT** PROVE, ✔MEASURED RATHER THAN ASSUMED. Suppressing
 * that SET_FPREG emission leaves this program STILL EXITING 42. The unwinder does
 * compute a wrong RSP and does read a bogus return address out of the middle of the
 * VLA — but RtlLookupFunctionEntry returns nothing for it, and the dispatcher then
 * falls back to a leaf-style pop and marches UP the stack eight bytes at a time until
 * it stumbles onto the real return address, after which the walk resumes correctly.
 * So "the process survived an exception thrown through the frame" is a weaker
 * instrument than it looks, and an earlier version of this comment predicted a crash
 * that does not happen. The pins that DO discriminate the frame register are
 * `tests/link/test_pe_writer.cpp`'s VlaFramePointerCaptureEmitsSetFpreg… (the three
 * UNWIND_INFO bytes) and a direct RtlCaptureContext/RtlVirtualUnwind drive over this
 * same frame, which hands back Rip 0 instead of landing in `level1`.
 *
 * What this program DOES prove on its own is the pair end to end: the OS accepts the
 * table, unwinds through a runtime-moved frame, runs the handler, and resumes on a
 * stack that still works — and, separately, that the frame model underneath holds,
 * since removing the outgoing-args watermark bias makes it exit 11.
 *
 * ✔The ORACLE for the same shape: mingw-w64 gcc 13.2.0 emits, for a VLA function
 * that calls, exactly UWOP_SET_FPREG + the fixed prologue's codes and nothing for
 * the dynamic allocation. Win64 has no "dynamic alloca" unwind opcode; the frame
 * register IS the mechanism.
 *
 * The guards are read back AFTER the handler runs: the real unwind restores
 * non-volatile registers out of `vla_frame`'s (and `level1`'s) UWOP_SAVE_NONVOL
 * slots, so a slot described against the wrong base hands main back a corrupted
 * local even when the walk itself survives.
 *
 *   10  VirtualAlloc failed — environment, not unwind.
 *   1..3, 11..14  strict in-program pins.
 *   42  the OS walked THROUGH the VLA frame to main's handler and back (SUCCESS).
 *   (a crash) the VLA frame's .pdata/.xdata does not describe the frame.
 *
 * pe64-ONLY: x64 SEH + __C_specific_handler + windows.h are Windows.
 */
#include <windows.h>

/* Wide enough to force stack arguments on ms_x64 (4 argument GPRs + 32B shadow),
 * so `vla_frame` genuinely writes into the outgoing-args area that travels with SP
 * while its VLA is live. */
int consume12(int a, int b, int c, int d, int e, int f,
              int g, int h, int i, int j, int k, int l) {
    return a + b + c + d + e + f + g + h + i + j + k + l;
}

/* The faulting leaf. Separate so `vla_frame` is a MIDDLE frame in the walk. */
int trap(volatile int *p) { return *p; }

/* The frame under test: a VLA object AND calls. */
int vla_frame(int n, volatile int *p) {
    int a[n];
    int i;
    for (i = 0; i < n; i = i + 1) a[i] = i + 1;
    if (consume12(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) != 78) return 1;
    for (i = 0; i < n; i = i + 1) {
        if (a[i] != i + 1) return 2;      /* the object survived the wide call */
    }
    return trap(p) + a[0];                /* → access violation, unwound through here */
}

int level1(int n, volatile int *p) { return vla_frame(n, p) + 1; }

int main(void) {
    void *p = VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    if (p == 0) {
        return 10;                        /* environment failure — not an unwind result */
    }
    volatile int vn = 24;
    int n = vn;
    /* Live across the __try, so the unwind has to hand them back intact. */
    int g1 = 0x11111111;
    int g2 = 0x22222222;
    int g3 = 0x33333333;
    int g4 = 0x44444444;
    int rc = 0;
    __try {
        rc = level1(n, (volatile int *)p);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) {
        rc = 42;
    }
    if (rc != 42) return 11;              /* the handler ran (the walk found it) */
    if (g1 != 0x11111111) return 12;      /* context restored across the VLA frame */
    if (g2 != 0x22222222) return 13;
    if (g3 != 0x33333333) return 14;
    if (g4 != 0x44444444) return 3;
    /* A call from the resumed frame: RSP has to be sane for this to return. */
    if (consume12(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1) != 12) return 4;
    return 42;
}
