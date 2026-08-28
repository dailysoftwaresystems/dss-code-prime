// C11/C23 6.4.5 (FF3, D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE): `u"\xC3\xA9"`
// uses \x byte escapes in a char16_t string. A byte escape names a raw code-unit
// VALUE, not a code point; the old path SILENTLY collapsed the two intended units
// into one (0x00E9). Cycle C FAILS LOUD with H_WideByteEscapeUnsupported
// (unsuppressable). The narrow "\xC3\xA9" is UNCHANGED (byte-producing).
int main(void) {
    unsigned short *p = (unsigned short *)u"\xC3\xA9";
    return p[0];
}
