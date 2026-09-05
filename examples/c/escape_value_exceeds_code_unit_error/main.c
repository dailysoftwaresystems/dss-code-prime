// C 6.4.4.4 / 6.4.5 (D-CSUBSET-NARROW-HEX-ESCAPE-TRUNCATED-TO-TWO-DIGITS): a
// `\x` escape whose VALUE does not fit one code unit of the literal's element
// FAILS LOUD with H_EscapeValueExceedsCodeUnit (unsuppressable), naming the
// width and the value.
//
// ★ THIS IS THE HALF OF THE FIX THAT REFUSES, and the union is what decides it.
// ✔MEASURED 2026-09-02, each reference compiled and RUN: gcc 13.3.0 and
// mingw-w64 gcc 13.2.0 ACCEPT `"a\xFFb"` by TRUNCATING it to `61 FB 00` with a
// warning; clang 18.1.3 and MSVC 19.51 REFUSE it. A reference that only accepts
// by narrowing the value away is not a reference that WORKS for that literal, so
// the union taken over what works is a REFUSAL — never a truncation, which would
// be the same silent wrong answer this anchor was opened for.
//
// ⚠ THE TRAP IS THAT `b` IS A HEX DIGIT. The escape here is not `\xFF` followed
// by the letter 'b' — it is `\xFFb`, value 0xFFB, because `\x` runs to the last
// hex digit (6.4.4.4p7). DSS used to emit `61 FF 62 00` for this, matching NO
// reference in either direction, and the diagnostic says so explicitly rather
// than leaving the author to wonder where 0xFFB came from.

extern int puts(const char* s);

int main(void) {
    puts("a\xFFb");
    return 0;
}
