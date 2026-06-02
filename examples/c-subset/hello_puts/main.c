// Plan 11 FF5 FFI-metadata wiring pin (2026-06-02). COMPILE-ONLY
// (runOn=[] in expected.json — no runtime assertion).
//
// What this proves:
//   * Source-declared `extern int puts(const char* s);` reaches
//     the linker carrying msvcrt.dll as its `importLibrary` AND
//     "puts" as its `mangledName` via
//     `compileSingleUnit`'s step-2.5 `synthesizeFfiFromSourceDecls`
//     call.
//   * The produced PE binary's .idata section contains TWO
//     `ImageImportDescriptor` entries (kernel32.dll for the
//     trampoline's synthetic `ExitProcess` + msvcrt.dll for the
//     user-declared `puts`) — the structural shape of the FFI
//     wiring is observable in the on-disk image.
//   * Per-language `externLibraryByFormat` config (in
//     `c-subset.lang.json`) routes the `pe` key to "msvcrt.dll"
//     without any `if (target == ...)` in the synthesis kernel.
//
// What this does NOT yet prove (anchored open):
//   * The binary actually RUNS and PRINTS — anchored
//     **D-FF6-RUNTIME-PRINT**. Calling msvcrt's `puts` requires
//     CRT initialization (stdout `FILE*` setup, locale, etc.)
//     which the current LK10 entry trampoline does not perform —
//     the OS spawns straight into our trampoline → user `main`
//     → puts → SEGV (0xC0000005) because the CRT-internal state
//     puts touches is zero-initialised. The runtime-print
//     deliverable belongs to the cycle that closes ONE of:
//       (a) D-LK10-CRT-INIT-INVOKE — invoke msvcrt's
//           CRT-init shim (`_initterm` / `__main` /
//           `_get_osfhandle` for the stdio FILE*s) from the
//           trampoline prologue before `call main`; OR
//       (b) D-LK10-KERNEL32-WRITE-PATH — a separate example
//           uses `kernel32.WriteFile` (5 args; needs ML7
//           L_StackPassedArg + GetStdHandle's HANDLE type
//           handling) which is CRT-init-free and IS the
//           runtime-print path the harness's
//           captureStdout pipe was already wired for in
//           Slice 1.
//   * Multi-library IAT runtime correctness — anchored
//     **D-LK6-2A-MULTI-LIBRARY-PIN** (the on-disk structure is
//     proven via examples_runner's compile assertion; a runtime
//     pin that the kernel32 + msvcrt slots BOTH resolve to
//     correct entry points requires the runtime-print path to
//     land first).
//
// Slice 1's stdout-capture harness extension (CreatePipe +
// ReadFile + RunResult.capturedStdout) IS load-bearing
// infrastructure for the future runtime-print cycle — wiring
// it here, before the print path lands, means the next cycle's
// `examples_runner` already speaks the pipe protocol and the
// `expectedStdout` manifest field already deserializes.

extern int puts(const char* s);

int main() {
    puts("hello");
    return 42;
}
