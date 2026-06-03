// ★ D-LK10-KERNEL32-WRITE-PATH closure + D-CSUBSET-LOCAL-INT-CODEGEN
// payoff (step 13.3, 2026-06-02).
//
// The first DSS-emitted Windows binary that prints WITHOUT msvcrt:
// pure kernel32-direct I/O (GetStdHandle + WriteFile + ExitProcess).
// Compared to hello_puts (step 13.1's payoff), this is more honest —
// hello_puts works because msvcrt.dll's own DllMain initializes
// stdout's FILE* at DLL-attach. With WriteFile we depend on NOTHING
// but the loader resolving kernel32's IAT entries.
//
// End-to-end substrate exercised by this example:
//
//   * D-ML7-2.2 stack-passed args (step 13.1) — WriteFile is a 5-arg
//     call; Win64 ms_x64 passes args 1-4 in RCX/RDX/R8/R9 and arg 5
//     (`lpOverlapped`) on the stack.
//
//   * D-LANG-POINTER-VOID-CONVERT (step 13.2) — `void* h` types as
//     `Ptr<Void>`. GetStdHandle's return value (`void*` HANDLE)
//     flows into WriteFile's arg 1; `"hello\r\n"` (Ptr<Char>) flows
//     into arg 2 (char* lpBuffer); `&written` (Ptr<I32>) flows
//     into arg 4 (int* lpWritten).
//
//   * D-LANG-NULL-POINTER-CONSTANT (step 13.3a) — literal `0` flows
//     into arg 5 (`void* lpOverlapped`) as a null pointer constant
//     per C §6.3.2.3.3. Windows reads this as synchronous-mode +
//     no OVERLAPPED struct.
//
//   * D-CSUBSET-EXTERN-LIBRARY-SYNTAX (step 13.3a) — trailing
//     `"kernel32.dll"` library override on each extern routes the
//     symbols to kernel32 (vs c-subset's default msvcrt). The
//     per-symbol override threads through `HirExternRecord.libraryOverride`
//     → `ExternDeclRef.libraryOverride` → `synthesizeFfiFromSourceDecls`.
//
//   * D-CSUBSET-LOCAL-INT-CODEGEN (step 13.3b) — `int written;` body
//     declaration emits an `alloca` LIR op which the materialize
//     pass rewrites to `lea reg, [rsp + localAreaOffset() + i*16]`.
//     Win64 ms_x64 frame layout: [outgoing-args 32B shadow + 8B
//     overflow arg5 | saved-regs | spill | LOCALS]. Pre-13.3b the
//     LIR alloca opcode reached the assembler unencoded; now the
//     materialize pass rewrites it before encoding.
//
//   * D-LK10-ENTRY Stage 1 — main's call to ExitProcess flows
//     through the trampoline emitter's synthetic ExitProcess
//     import. Exit code 42 propagates to the OS.
//
// Capture chain: WriteFile writes "hello\r\n" (7 bytes) DIRECTLY
// to the stdout HANDLE — no CRT text-mode translation in this
// path. The captured pipe receives those exact 7 bytes byte-for-
// byte; the runner asserts both `exitCode==42` and
// `capturedStdout=="hello\r\n"`.
//
// STD_OUTPUT_HANDLE = -11 (DWORD). Sign-extending int -11 in RCX
// low-32 bits matches the constant: 0xFFFFFFF5 = (DWORD)-11.

extern void* GetStdHandle(int nStdHandle) "kernel32.dll";
extern int   WriteFile(void* hFile, char* lpBuffer, int n,
                       int* lpWritten, void* lpOverlapped) "kernel32.dll";

int main() {
    void* h = GetStdHandle(-11);
    int written;
    WriteFile(h, "hello\r\n", 7, &written, 0);
    return 42;
}
