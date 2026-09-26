// [[D-TOK-STRING-STYLE-MULTILINE-IS-NEVER-READ]] + [[D-PP-CONVERSION-DIAGNOSTIC-FIRES-ON-A-TOKEN-NEVER-CONVERTED]]
// — prose with a LONE quote on a line nobody compiles, the everyday shape.
//
// A character constant, string literal or header name cannot hold a new-line
// (C23 6.4.4.4, 6.4.5, 6.4.7), so a lone ' or " ends at its own line. DSS used
// to run the literal on to the NEXT quote, lines later, and either refused the
// file (one lone quote: "unterminated") or swallowed every directive in
// between (two). And a lone quote that phase 7 never converts — skipped text,
// an unexpanded #define, #warning words — is not judged at all.
//
// ✔MEASURED 2026-09-22: gcc 13.3.0, clang 18.1.3 and MinGW gcc accept every
// line below (MSVC VS 18 errors on the #define and the live #warning; one
// working reference makes acceptance required). Runtime witness: exits 42, and
// the live #warning is pinned by the manifest's expectWarnings.

#if 0
This block isn't compiled; it's a note for the reader.
It says "hello and never closes that quote.
#error it's skipped, so this never fires
#warning don't show this either
#include <never_closed.h
#endif

#ifdef NOT_DEFINED
The author's draft: a "second" quote, and one more ' for luck.
#endif

#define UNUSED_NOTE don't expand me

#warning don't panic: this line is live, and it only warns

int main(void) {
    int answer = 40;
#if 1
    answer += 2;   /* it's live: a quote inside a comment is inert */
#endif
    return answer;
}
