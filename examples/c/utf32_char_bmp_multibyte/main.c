// C11/C23 6.4.4.4 UTF-8-decode witness: U'€' (raw source bytes E2 82 AC,
// U+20AC EURO SIGN) must decode those THREE source bytes to the SINGLE code point
// 0x20AC — not pass the bytes through. char32_t holds the full scalar, so the
// value == 0x20AC gates the exit code 42. The release arm runs the same.

int main(void) {
    return U'€' == 0x20AC ? 42 : 1;
}
