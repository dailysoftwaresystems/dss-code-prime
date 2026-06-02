// ★ D-FF6-RUNTIME-PRINT pin (2026-06-02). The first DSS-emitted
// binary that calls into msvcrt.dll's `puts`, prints "hello\r\n" to
// captured stdout, and exits 42 — all asserted byte-for-byte by the
// examples_runner harness (runOn=["windows"], expectedStdout="hello\r\n").
//
// What this proves end-to-end:
//   * Source-declared `extern int puts(const char* s);` reaches the
//     linker carrying msvcrt.dll as its `importLibrary` AND "puts" as
//     its `mangledName` via `compileSingleUnit`'s step-2.5
//     `synthesizeFfiFromSourceDecls` call (FF5 cycle).
//   * The produced PE binary's .idata section contains TWO
//     `ImageImportDescriptor` entries — kernel32.dll for the
//     trampoline's synthetic `ExitProcess` + msvcrt.dll for the
//     user-declared `puts`. First DSS binary exercising the
//     multi-library IAT path at runtime
//     (D-LK6-2A-MULTI-LIBRARY-PIN structurally + runtime verified).
//   * Per-language `externLibraryByFormat` config (in
//     `c-subset.lang.json`) routes the `pe` key to "msvcrt.dll"
//     with zero `if (target == ...)` in the synthesis kernel.
//   * `main`'s prologue emits `sub $0x28, %rsp` (40 bytes — 32 Win64
//     shadow space + 8 alignment for the post-CALL RSP delta).
//     Without this, msvcrt's `puts` (which uses SSE `movaps` stores
//     on its home-area arg-spill path) AVs at runtime with
//     0xC0000005. The fix lives in ML7 callconv
//     (`computeFrameLayout` + `hasCalls` + `callPushBytes` field on
//     TargetCallingConvention) — closed D-LK10-ENTRY-ML7-FRAME-BIAS-UNIFY.
//   * `main`'s call to `puts` uses `FF 15 disp32` (`call qword ptr
//     [rip + IAT_slot]`), NOT `E8 disp32` (direct rel32). The
//     direct form would execute the IAT slot's bytes as code →
//     SEGV. The fix lives in MIR→LIR (`CallIndirectViaExtern`
//     MnemonicSlot + `externSymbols` set in Lowerer).
//
// Capture chain: msvcrt's puts writes "hello\n" to its stdout FILE*,
// which is text-mode by default → CRT translates `\n` → `\r\n` on
// emit. The DSS test harness's `runBinary(captureStdout=true)`
// opens an anonymous pipe and feeds it as the child's stdio handle;
// pipes are binary-mode, but the CRT-side text-mode translation
// happens BEFORE the bytes reach the pipe, so the parent captures
// "hello\r\n". A `\r\n` vs `\n` mismatch on this pin would be a
// harness/CRT-detail issue, NOT a codegen bug — the manifest's
// `expectedStdout` matches what msvcrt emits through a pipe under
// text-mode stdio.
//
// A regression in ANY layer flips either the exit code OR the
// captured stdout and the harness fails immediately.

extern int puts(const char* s);

int main() {
    puts("hello");
    return 42;
}
