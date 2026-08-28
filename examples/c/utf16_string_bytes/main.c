// C11/C23 6.4.5: a `u"…"` (char16_t) string literal → UTF-16 code units. Read as
// a `unsigned short*`, p[0] and p[1] are the two 16-bit units 0x0041 ('A') and
// 0x0042 ('B'); their sum is 0x41 + 0x42 = 131, the process exit code. A
// regression that emitted the literal as narrow bytes (`41 42`) would put 'A'|'B'
// in one unit and read garbage in p[1] — the exit code would diverge immediately.
// The release arm runs the same through the shipped optimizer pipeline.

int main(void) {
    unsigned short *p = (unsigned short *)u"AB";
    return p[0] + p[1];
}
