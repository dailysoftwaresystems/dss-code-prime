// C11/C23 6.4.5: the UTF-8-DECODE witness. `u"€"` written as the raw source
// character EURO SIGN (U+20AC, source bytes E2 82 AC) must decode those THREE
// source bytes to a SINGLE UTF-16 code unit 0x20AC — NOT pass the bytes through
// verbatim. Read as unsigned short*, p[0] == 0x20AC gates the exit code 42; a
// byte-passthrough regression would put 0x82E2 (or similar) in p[0] and return 1.
// The release arm runs the same through the shipped optimizer pipeline.

int main(void) {
    unsigned short *p = (unsigned short *)u"€";
    return p[0] == 0x20AC ? 42 : 1;
}
