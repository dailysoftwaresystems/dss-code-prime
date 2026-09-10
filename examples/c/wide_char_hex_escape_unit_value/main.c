// C 6.4.4.4 (D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE, CHARACTER half): a `\x` hex
// or `\ooo` octal escape in a wide/UTF character constant names a RAW CODE-UNIT
// VALUE, and DSS now assembles it directly.
//
// ★ THIS REPLACES A REFUSAL THAT WAS ITSELF REPAIRING A SILENT MISCOMPILE, so it
// has to pin BOTH hazards at once. The original defect collapsed `u'\xC3\xA9'`
// through the UTF-8 re-decoder into ONE wrong 0x00E9 unit; the repair refused
// every byte escape in a wide literal; this closes it properly by keeping the
// VALUE instead of flattening it to bytes. A regression in either direction —
// back to the collapse, or back to the blanket refusal — changes the exit code.
//
// ★★ THE ESCAPE IS NOT A CODE POINT, and the checks below are chosen to prove
// exactly that, because the tempting "just treat it as a code point" shortcut
// passes the easy cases and fails these:
//   u'\xD800'     a LONE SURROGATE — valid as a unit, and `u'\uD800'` (the same
//                 number as a UCN) is refused by all four references
//   U'\xFFFFFFFF' PAST U+10FFFF — not a Unicode scalar value at all
//   u'\777'       octal is capped at three digits but NOT at 255; 0x1FF is an
//                 ordinary char16_t unit (DSS used to reject it as malformed)
//   u'\x101'      more than two hex digits, which the narrow decoder truncated
// ✔MEASURED 2026-09-02: every value below is what gcc 13.3.0, clang 18.1.3,
// mingw-w64 gcc 13.2.0 and MSVC 19.51 all produce, each compiled and RUN with
// its emitted unit read back.
//
// ⓘ The wide STRING form is not here yet — it waits on the semantic tier's
// code-unit-count thread, and until then it still fails loud
// (examples/c/wide_string_hex_escape_error).

extern int puts(const char* s);

int main(void) {
    unsigned short exact    = (unsigned short)u'\xFFFF';       // fills 16 bits
    unsigned int   wide     = (unsigned int)U'\xFFFFFFFF';     // fills 32 bits
    unsigned short surro    = (unsigned short)u'\xD800';       // a lone surrogate
    unsigned short octal    = (unsigned short)u'\777';         // 0x1FF, > 255
    unsigned short manyHex  = (unsigned short)u'\x101';        // three hex digits
    unsigned short small    = (unsigned short)u'\x41';
    if (exact   != 0xFFFFu)     return 1;
    if (wide    != 0xFFFFFFFFu) return 2;
    if (surro   != 0xD800u)     return 3;
    if (octal   != 0x1FFu)      return 4;
    if (manyHex != 0x101u)      return 5;
    if (small   != 0x41u)       return 6;
    // A `\u` UCN still means a CODE POINT, unchanged — the two spellings coexist
    // and must not be routed through one another.
    if ((unsigned short)u'\u00E9' != 0x00E9u) return 7;
    puts("units");
    return 42;
}
