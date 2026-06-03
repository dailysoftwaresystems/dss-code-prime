// D-LANG-VARIADIC (step 13.4, 2026-06-02): the first DSS-emitted
// binary that calls a C variadic function. printf is declared with
// `...`; the c-subset grammar admits the call with the fixed param
// (`fmt`) plus one variadic int (`42`).
//
// End-to-end substrate exercised:
//
//   * D-LANG-VARIADIC grammar — `extern int printf(const char* fmt, ...);`
//     parses via the new `paramOrEllipsis` alt + `ellipsisParam`
//     wrapper around `EllipsisOp`.
//
//   * D-LANG-VARIADIC semantic — the externDecl's params subtree
//     contains the `EllipsisOp` token; semantic analyzer detects it
//     via `paramsSubtreeHasVariadicMarker` (driven by the
//     declaration's `variadicMarker` config) and builds a variadic
//     FnSig (scalars=[CcSysV, 1]).
//
//   * D-LANG-VARIADIC call admission — `printf("hello\n")` passes 1
//     arg against the 1-fixed-param variadic FnSig; `checkCall`
//     admits because `variadicFnSig && argNodes.size() >= fixedCount`.
//     The 1-fixed/0-vararg shape is the minimal admission pin; the
//     2-arg variadic case (`printf("answer=%d\n", 42)`) is exercised
//     by sibling example `printf_int/`.
//
//   * D-LANG-VARIADIC ABI — MIR Call's payload carries
//     (isVariadic=true, fixedArgCount=1). This binary runs on Win64
//     (per `expected.json` runOn=windows), so the ms_x64 cc applies
//     and `variadicVectorCountReg` is empty — the count-mov is
//     skipped. SysV runs (Linux/macOS) would emit `mov rax, 0`
//     before the call (count of FPR args in vararg region = 0
//     because c-subset has no float type today). The count-of-FPRs
//     scan is type-driven for future float vararg support.
//
//   * D-CSUBSET-EXTERN-LIBRARY-SYNTAX (step 13.3) — printf comes
//     from msvcrt.dll on Windows (the c-subset default for PE) — no
//     per-symbol override needed.

extern int printf(const char* fmt, ...);

int main() {
    // Use bare `\n` so msvcrt's stdout text-mode translation produces
    // exactly `\r\n` on Windows (the manifest's expectedStdout). A
    // source with `\r\n` here would yield `\r\r\n` after translation
    // and trip the runner's byte-for-byte stdout pin.
    printf("hello\n");
    return 42;
}
