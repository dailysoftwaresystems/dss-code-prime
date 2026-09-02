// D-CSUBSET-INCOMPATIBLE-REDECL-DIAGNOSED-AT-CALL-SITE-NOT-DECLARATION.
//
// `extern int printf();` over an `#include <stdio.h>` is an INCOMPATIBLE
// REDECLARATION: C23 made an empty parameter list mean `(void)`, so this declares
// a NO-PARAMETER printf where the platform declares a variadic one taking a
// pointer. C23 6.7p4 requires the diagnostic AT THE DECLARATION; gcc, clang and
// mingw-w64 gcc all refuse this source ("conflicting types for 'printf'").
//
// ★★ THERE IS NO CALL IN THIS FILE, AND THAT IS THE ENTIRE POINT. DSS used to
// report only an ARITY error at the first CALL SITE — blaming a correct call for
// the declaration that broke it — so this exact program, with the bad declaration
// and nothing calling it, COMPILED CLEAN. That silence is the sharpest available
// witness for the tier: a message change at the call site would leave this file
// passing, and only a diagnostic raised at the DECLARATION reds it.
//
// ⚠ The `()` ≡ `(void)` reading is CORRECT and is NOT what this example pins —
// that half was already right. What is pinned is WHERE the conflict is reported.
//
// RED-ON-DISABLE: delete the C23 compatibility check in the semantic tier's
// platform-realization pass (the `includeSuppressed` loop) and this file compiles
// rc=0 with no diagnostic at any stage, exactly as it did before P44.
#include <stdio.h>

extern int printf();

int main(void) { return 0; }
