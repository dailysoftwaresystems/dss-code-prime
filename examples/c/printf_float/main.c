// c155 D-LK10-CRT-INIT-INVOKE closure witness #1 (2026-07-17):
// floating-point printf — the anchor's first cited "genuinely
// CRT-init-requiring" case ("printf with %f (locale-dependent)").
//
// The c155 diagnosis DISPROVED the premise: %f formatting works on
// every runnable leg TODAY, with the trampoline calling main directly
// (no user-side CRT-init sequence), because every hosted format's
// LOADER runs libc's own initialization before the DSS entry:
//
//   * pe64/msvcrt — the Windows loader runs msvcrt.dll's DllMain
//     (_CRT_INIT) at DLL_PROCESS_ATTACH, before the exe entry point:
//     stdio, locale, and FP-formatting state are live when the
//     trampoline runs (the same self-init D-FF6-RUNTIME-PRINT proved
//     for puts on 2026-06-02, now proven to cover %f).
//   * elf/glibc — ld.so runs libc.so.6's OWN ELF initializers
//     (DT_INIT/init_array, called with argc/argv/envp) before
//     transferring to e_entry; glibc fully self-initializes stdio,
//     locale, and malloc. What __libc_start_main would ADD (running
//     the EXECUTABLE's own .init_array, __libc_argv bookkeeping,
//     atexit(_dl_fini)) is nothing DSS-emitted C programs use.
//   * macho/libSystem — dyld runs every loaded image's initializers
//     (libSystem's full libc init included) BEFORE calling the
//     LC_MAIN entry; every DSS macho-exec loads libSystem
//     (image.loadDylibs + the processExit `_exit` import).
//
// Substrate this pins beyond the CRT question: a DOUBLE passed as a
// VARIADIC argument on every ABI — SysV AMD64 (xmm0 + the al=1
// vector-count stamp), ms_x64 (the MS varargs rule: the vararg
// double rides the slot's GPR image so the callee's va_arg walk
// reads it positionally), AAPCS64 (v0), and Apple arm64 (the
// always-stack variadic rule, FC12c).
//
// Exact-output pin: "%f\n" of 3.14 is "3.140000\n" (C17 7.21.6.1 —
// default precision 6) — msvcrt's text mode translates to CRLF.
// A regression in EITHER the variadic-double lowering OR the
// loader-init assumption flips stdout or the exit code and the
// harness fails immediately.

extern int printf(const char* fmt, ...);

int main(void) {
    printf("%f\n", 3.14);
    return 42;
}
