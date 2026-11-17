// D-CSUBSET-INCOMPATIBLE-REDECL-DIAGNOSED-AT-CALL-SITE-NOT-DECLARATION, restated by P69
// (lane cs, fold f26d — main's ledgers 554, 598, 599).
//
// `extern int printf();` over an `#include <stdio.h>` declares printf WITHOUT a
// prototype (ledger 554: an empty list that is not a definition gives none). Beside
// the header's `int printf(const char *, ...)` that is the pair C 6.7.6.3p15 judges,
// and an ellipsis is never compatible with no prototype — so the pair is diagnosed
// AT THE DECLARATION, the tier this row is about (S_PrototypeIncompatibleWith-
// ADeclarationWithoutOne). It is a WARNING because one reference builds and runs the
// program: ✔MEASURED (lane cs probe rcp, built and RUN) cl 19.52 builds it, at its
// default standard and /std:clatest, and the call prints; gcc 13.3.0, clang 18.1.3,
// mingw-w64 gcc 13.2.0 and Apple clang 21 refuse it ("conflicting types for 'printf'").
//
// ★ THE HEADER'S PROTOTYPE IS THE FUNCTION'S TYPE FROM HERE ON, as cl takes it: the
// call below is a VARIADIC call with an `int` through `...` (on SysV, `%al` states the
// vector registers; on pe, the UCRT shim stands for printf), and a call the prototype
// rejects (`printf()`) is the arity error every reference gives. Until f26d the
// declaration was an error here, and every call through it saw no prototype at all.
//
// RED-ON-DISABLE: route the pair back to the prototype oracle (the check against the
// shipped declaration) and this file is refused; stop reading the header's prototype
// at the call and the call is lowered without one.
#include <stdio.h>

extern int printf();

// The count `printf` returns for "42\n", in a function the release pipeline inlines,
// so the release image differs from the baseline on every target (the bare `== 3`
// left the aarch64 ELF image byte-identical at release).
static int written(void) { return 3; }

int main(void) { return printf("%d\n", 42) == written() ? 42 : 1; }
