// C11/C23 6.4.5 (Cycle C): the RAW astral character `u"😀"` (U+1F600, source
// bytes F0 9F 98 80) - a supplementary-plane code point under a 16-bit char16_t -
// now encodes as a UTF-16 SURROGATE PAIR (high 0xD83D, low 0xDE00) rather than
// failing loud. Read as unsigned short*, [0]==0xD83D && [1]==0xDE00 && [2]==0 gates
// 42. This is the raw-source analog of ucn_surrogate_pair_utf16 (the \U escape
// form). The release arm runs the same.

int main(void) {
    unsigned short *p = (unsigned short *)u"😀";
    return (p[0] == 0xD83D && p[1] == 0xDE00 && p[2] == 0) ? 42 : 1;
}
