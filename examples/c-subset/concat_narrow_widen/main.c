// Cycle D (C11/C23 6.4.5p5): adjacent-string-concat prefix MIXING. A run of
// adjacent string literals takes the single non-narrow prefix as its effective
// prefix and the NARROW segments widen to it (position-independent). Here the
// trailing narrow "cd" widens to char16_t alongside the leading u"ab".
//
// Runtime witness (modeled on utf16_string_bytes): read the run as unsigned short*
// — p[2] and p[3] are the two UTF-16 code units of the WIDENED narrow "cd", i.e.
// 0x0063 ('c') and 0x0064 ('d'). Their sum is 99 + 100 = 199, the process exit
// code. `u"` is used (not `L"`) so the code-unit width is a fixed 16 bits on ALL
// four targets — the widen MECHANISM is identical, but the exit code is one value.
// A regression that dropped the second piece would read past the wide NUL; one that
// appended "cd" as raw NARROW bytes (un-widened) would mis-align p[2]/p[3] and read
// garbage — either flips the exit code immediately. The release arm runs the same
// through the shipped optimizer pipeline.

int main(void) {
    unsigned short *p = (unsigned short *)(u"ab" "cd");
    return p[2] + p[3];
}
