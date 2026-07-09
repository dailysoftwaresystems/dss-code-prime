// C11/C23 6.4.5: a `U"…"` (char32_t) string literal → UTF-32 code units. Read as
// `unsigned int*`, p[0] and p[1] are the two 32-bit units 0x00000041 ('A') and
// 0x00000042 ('B'); their sum is 131, the exit code. Witnesses the 4-byte LE
// element encoding + the element-width-aware rodata sizing (Array<U32,3> = 12
// bytes). The release arm runs the same through the shipped optimizer pipeline.

int main(void) {
    unsigned int *p = (unsigned int *)U"AB";
    return p[0] + p[1];
}
