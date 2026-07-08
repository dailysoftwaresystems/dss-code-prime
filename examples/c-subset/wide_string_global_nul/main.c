// C11/C23 6.4.5 (F1 element-width NUL + F6 char[N] zero-fill): a NAMED global wide
// array `unsigned short w[2] = u"A";` — the .rodata producer must emit the ONE code
// unit 0x0041 THEN an element-WIDE zero terminator, i.e. the 4 bytes 41 00 00 00
// (Array<U16,2>), NOT a single narrow NUL byte. Reading w[0]==0x41 AND w[1]==0
// gates the exit code 42; a narrow-terminator regression would leave w[1] with a
// stray high byte (over-read of adjacent rodata) and return 1. The release arm runs
// the same through the shipped optimizer pipeline.

unsigned short w[2] = u"A";

int main(void) {
    return (w[0] == 0x41 && w[1] == 0) ? 42 : 1;
}
