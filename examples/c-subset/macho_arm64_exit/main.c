// macOS-ARM64 runnable smoke (D-LK10-ENTRY-MACHO-EXIT, v0.0.3). The
// first DSS Mach-O binary for Apple Silicon: this compiles to an
// AArch64 Darwin MH_EXECUTE whose synthetic `_start`-equivalent
// trampoline calls main, moves the return value into x0, and makes a
// DIRECT BL to the libSystem `_exit` __stubs entry (externCallDispatch
// = direct-plt) — terminating the process with the return value as the
// exit code.
//
// Runs natively on the macos-latest (= Apple Silicon / arm64) CI leg;
// SkippedCrossHost everywhere else (the binary's target arch differs
// from the host and no emulator is declared). The compile step is
// asserted clean on EVERY host — it proves the whole c-subset →
// AArch64-Mach-O cross-compilation path, including the ad-hoc code
// signature the macOS kernel requires.
int main() {
    return 42;
}
