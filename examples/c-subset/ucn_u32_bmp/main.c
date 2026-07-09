// C11/C23 6.4.3: `U"\u00e9"` (e-acute, U+00E9) - a BMP universal character
// name under a 32-bit char32_t element decodes (exactly 4 hex digits -> one code
// point) to ONE code unit 0x000000E9. Read as unsigned int*, p[0]==0xE9 gates the
// exit code 42; a byte-passthrough regression would read 0xC3/0xA9. Witnesses the
// \u UCN decode + the 4-byte LE element encode. The release arm runs the same.

int main(void) {
    unsigned int *p = (unsigned int *)U"\u00e9";
    return p[0] == 0xE9 ? 42 : 1;
}
