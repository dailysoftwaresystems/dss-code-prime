// FC15c (`#pragma` -- C 6.10.6) runtime witness. The preprocessor
// CONSUMES-AND-DROPS each `#pragma` line with NO error (C 6.10.6p2 licenses
// ignoring an unrecognized pragma; DSS recognizes none). This program is laced
// with `#pragma` lines in several positions -- before, between, and inside the
// function -- and still compiles -> links -> runs to exit 42 through the SHIPPED
// release pipeline.
//
// RED-ON-DISABLE: without the `#pragma`-consume arm in handleDirective, each
// `#pragma` falls through to the generic unsupported-directive fail-loud
// (P_PreprocessorUnsupported) -> the compile errors and no binary is produced
// (the example fails). The pragmas carry GCC/STDC-style payloads (a string
// literal, parens, identifiers) to prove the WHOLE line is dropped, not just the
// directive word.

#pragma once
#pragma GCC optimize("O2")

int main(void) {
#pragma STDC FP_CONTRACT ON
    int a = 20;
    int b = 22;
#pragma GCC diagnostic push
    int sum = a + b;
#pragma GCC diagnostic pop
    return sum;            // 20 + 22 == 42
}
