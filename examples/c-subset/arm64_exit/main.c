// ARM64 runnable smoke (D-LK10-ENTRY-ARM64, v0.0.2 V2-1). The first
// DSS binary for a NON-HOST architecture: this compiles to an AArch64
// Linux ELF executable whose synthetic `_start` trampoline calls
// main, moves the return value into x0, loads the exit_group syscall
// number (94) into x8 via MOVZ, and issues SVC #0 — terminating the
// process with the return value as the exit code.
//
// Runs under qemu-aarch64 on a non-ARM64 host (e.g. the x86_64 Linux
// CI runner); SkippedCrossHost where no emulator is on PATH (e.g. the
// Windows dev host). The compile step is asserted clean on EVERY host
// — it proves the whole c-subset → AArch64-ELF cross-compilation path.
int main() {
    return 42;
}
