// C11/C23 6.4.3 + 6.4.5: `u8"\U0001F600"` (U+1F600) - an astral universal
// character name under a char8_t element -> the canonical 4-byte UTF-8 sequence
// F0 9F 98 80, then the NUL. Read as unsigned char*, the four bytes gate the exit
// code 42 (each checked explicitly). The release arm runs the same.

int main(void) {
    unsigned char *p = (unsigned char *)u8"\U0001F600";
    return (p[0] == 0xF0 && p[1] == 0x9F && p[2] == 0x98
            && p[3] == 0x80 && p[4] == 0) ? 42 : 1;
}
