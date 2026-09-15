// C 6.4.4.4p7 (D-CSUBSET-NARROW-HEX-ESCAPE-TRUNCATED-TO-TWO-DIGITS): a `\x`
// escape consumes EVERY following hex digit, not two.
//
// THE DEFECT THIS PINS WAS SILENT. DSS used to emit `04 31 00` for `"\x041"` —
// three bytes, rc 0, no diagnostic — where gcc 13.3.0, clang 18.1.3, mingw-w64
// gcc 13.2.0 and MSVC 19.51 ALL emit `41 00` (✔MEASURED 2026-09-02, each
// compiled and RUN with its emitted bytes read back). A program whose string is
// quietly wrong is the class the bar most abhors, and it is reachable from
// ordinary C with nothing unusual in sight.
//
// The runtime witness is deliberately VALUE-dependent, not compile-only: every
// check below reads an actual byte or an actual `sizeof`, so a regression to the
// two-digit cap changes the exit code rather than passing over a wrong string.
//   "\x041"           -> ONE byte 0x41  (the two-digit cap gave 0x04 then '1')
//   "\x0000000000041" -> ONE byte 0x41  (leading zeros are free)
//   "a\x41z"          -> 'a' 0x41 'z'   ('z' is not a hex digit, so it ends it)
// `puts` of the decoded byte gives stdout a second, independent witness.
//
// ⚠ The sharpest form is NOT here because it must be REFUSED, not run: in
// `"a\xFFb"` the `b` IS a hex digit, so the escape is `\xFFb` = 0xFFB, which
// overflows a byte — see examples/c/escape_value_exceeds_code_unit_error.

extern int puts(const char* s);

int main(void) {
    const char* s = "\x041";
    const char* t = "\x0000000000041";
    const char* m = "a\x41z";
    if (sizeof("\x041") != 2) return 1;             // one byte + the NUL
    if (s[0] != 'A' || s[1] != 0) return 2;
    if (sizeof("\x0000000000041") != 2) return 3;
    if (t[0] != 'A' || t[1] != 0) return 4;
    if (sizeof("a\x41z") != 4) return 5;
    if (m[0] != 'a' || m[1] != 'A' || m[2] != 'z') return 6;
    puts(s);
    return 42;
}
