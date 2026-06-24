// FC15b (predefined macros -- C 6.10.8) runtime witness. Exercises the
// load-bearing INVOCATION-LINE semantics (C 6.10.8.1: `__LINE__` is the line of
// the macro's INVOCATION, NOT the `#define` line) END TO END through the shipped
// pipeline, plus `__STDC_VERSION__` (C23) and a SHAPE/length check on
// `__DATE__`/`__TIME__` (NEVER an exact value -- the build date/time is
// nondeterministic).
//
// The exit code is built from line-GAPS (differences), never absolute line
// numbers, so it is robust to this comment block's length:
//
//   macroGap = (second WHERE line) - (first WHERE line) = 2
//       WHERE expands to __LINE__; each invocation must resolve to ITS OWN
//       invocation line, so the two values differ by the 2-line gap. WRONG (the
//       gap would be 0 -- both the single #define line) unless the invocation
//       offset is threaded through the macro replacement. THE primary witness.
//   bareGap  = (second bare __LINE__) - (first bare __LINE__) = 1
//       two bare __LINE__ on consecutive lines -- the degenerate own-position
//       case, one line apart.
//   modern   = (__STDC_VERSION__ >= 201112L) ? 1 : 0 = 1   (C23 == 202311L)
//   dateLen  = sizeof(__DATE__) = 11 ("Mmm dd yyyy") + 1 NUL = 12
//   timeLen  = sizeof(__TIME__) =  8 ("hh:mm:ss")    + 1 NUL =  9
//
//   return macroGap*4 + bareGap + modern + dateLen + timeLen + 11
//        =     2*4    +   1     +   1     +   12     +    9    + 11
//        =      8     +   1     +   1     +   12     +    9    + 11 = 42
//
// The `release` arm runs the SHIPPED parser->semantic->codegen->native pipeline
// over the SAME source, so a mis-resolved __LINE__ (macroGap != 2) changes the
// exit code (fold-resistant -- the gaps feed live runtime arithmetic).

#define WHERE __LINE__

int main(void) {
    int wa = WHERE;                 // first  WHERE invocation  (line 34)

    int wb = WHERE;                 // second WHERE invocation  (line 36)
    int macroGap = wb - wa;         // 36 - 34 == 2  (invocation-line semantics)

    int la = __LINE__;              // first  bare __LINE__     (line 39)
    int lb = __LINE__;              // second bare __LINE__     (line 40)
    int bareGap = lb - la;          // 40 - 39 == 1

    int modern = (__STDC_VERSION__ >= 201112L) ? 1 : 0;
    int dateLen = (int)sizeof(__DATE__);   // "Mmm dd yyyy" + NUL = 12
    int timeLen = (int)sizeof(__TIME__);   // "hh:mm:ss"    + NUL =  9

    return macroGap * 4 + bareGap + modern + dateLen + timeLen + 11;
}
