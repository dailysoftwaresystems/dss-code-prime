// C11/C23 6.4.5 (D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE): a `\x` escape whose
// VALUE is wider than one code unit of the literal's element FAILS LOUD with
// H_EscapeValueExceedsCodeUnit (unsuppressable), naming the width and the value.
//
// ⏳ THE CLAIM MOVED HERE; IT WAS NOT DROPPED. This example used to pin the
// WHOLESALE refusal of `u"\xC3\xA9"` — every byte escape in a wide literal was
// rejected, because DSS could not express an escape's value as a code unit. That
// refusal is gone: `u"\xC3\xA9"` now assembles to the two units 0xC3 and 0xA9,
// asserted by value in examples/c/wide_string_hex_escape_units. What remains
// fail-loud, and what this example now pins, is an escape that OVERFLOWS the
// element — the one case where the references genuinely split.
//
// ★ THE UNION IS TAKEN OVER WHAT WORKS, WHICH IS WHY THIS REFUSES. ✔MEASURED
// 2026-09-02, each reference compiled and RUN: gcc 13.3.0 and mingw-w64 gcc
// 13.2.0 ACCEPT `u"\x1FFFF"` by TRUNCATING it to 0xFFFF with a warning; clang
// 18.1.3 and MSVC 19.51 REFUSE it. A reference that only accepts by narrowing the
// value away is not a working reference for that literal, so DSS refuses. A
// truncation here would be exactly the silent wrong answer this anchor pair was
// opened for — the same shape as the collapse the example used to guard.
//
// ⓘ 0x1FFFF is a perfectly good `char32_t` unit: `U"\x1FFFF"` compiles, and so
// does `u"\xFFFF"`. The bound is the ELEMENT WIDTH, not Unicode and not a
// blanket ban on byte escapes.

int main(void) {
    unsigned short *p = (unsigned short *)u"\x1FFFF";
    return p[0];
}
