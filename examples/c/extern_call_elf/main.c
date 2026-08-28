// D-FFI-EXTERN-CALL-DISPATCH de-risk pin (2026-06-08): the FIRST DSS
// binary that calls a shipped-library function through the ELF dynamic
// FFI path end-to-end. The ELF PLT/GOT linker machinery
// (`encodeElfExecDynamic` + the per-machine PLT stubs) was COMPLETE but
// had NEVER actually run a shipped call — every prior FFI corpus targeted
// Windows-PE only, so the ELF extern-call path was unexercised.
//
// It calls `abs` (libc) via an INLINE `extern` — NO shipped `#include`
// (isolating the extern-call CODEGEN from descriptor resolution, a
// separate concern / D-FFI-SHIPPED-LIB-PLATFORM-SELECT) and an INT arg
// (NO string literal — isolating it from the ELF rodata-producer gap,
// another separate concern / D-LK1-RODATA: elf64-x86_64-linux-exec does
// not yet advertise a `rodata` section, so string-arg ELF FFI awaits that
// arm). `abs(7 - 49)` = abs(-42) = 42 → exit 42.
//
// This is the empirical proof that x86_64-ELF FFI was LATENTLY BROKEN and
// is now fixed:
//   * The ELF linker points `abs`'s extern symbolVa at its PLT STUB
//     (CODE: `jmp [rip+GOT]`), so the call site MUST be a PLAIN DIRECT
//     `call` (E8 disp32) to the stub — the stub does the GOT indirection.
//   * The historical `call_indirect_via_extern` (FF 15 = `call [ptr]`)
//     would dereference the PLT stub's CODE bytes as a function pointer →
//     SIGSEGV. That is what an ELF target emitted before this fix (the
//     dispatch was hardcoded to the PE/IAT shape).
//
// The fix is FORMAT-DRIVEN: `elf64-x86_64-linux-exec.format.json` declares
// `externCallDispatch: "direct-plt"`, and MIR→LIR selects the plain `call`
// opcode (vs PE/Mach-O's `indirect-slot` → FF 15). RED-on-disable: flip the
// format's `externCallDispatch` to `indirect-slot` and this binary SIGSEGVs
// / wrong-exits at runtime. Runs natively on the x86_64-Linux CI leg (no
// emulator) — the fast de-risk gate for the ELF dynamic FFI path before
// the ARM64 variant (a later cycle) reuses the same shape.

extern int abs(int n);

int main() {
    return abs(7 - 49);
}
