// C11/C23 6.4.3 + 6.4.5: `u"\U0001F600"` (😀, U+1F600) — a supplementary-plane
// universal character name under a 16-bit char16_t element encodes as a UTF-16
// SURROGATE PAIR: the high surrogate 0xD83D then the low surrogate 0xDE00 (two
// code units), then the wide NUL. Read as unsigned short*, [0]==0xD83D &&
// [1]==0xDE00 && [2]==0 gates the exit code 42; a truncation regression (one wrong
// unit) flips it. The release arm runs the same through the shipped optimizer.

int main(void) {
    unsigned short *p = (unsigned short *)u"\U0001F600";
    return (p[0] == 0xD83D && p[1] == 0xDE00 && p[2] == 0) ? 42 : 1;
}
