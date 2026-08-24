// c20 (D-CSUBSET-ADJACENT-STRING-CONCAT) — THE byte-level pin. C 5.1.1.2 phase 6
// concatenation is at the DECODED-byte level: phase 5 (escape decode) runs PER
// SEGMENT, BEFORE phase 6 joins the bytes. So `"\x41" "1"` decodes the first
// body's `\x41` to 'A' (0x41), then appends the second body's '1' → "A1" (two
// bytes 0x41 0x31).
//
// The trap this guards: a naive RAW-TOKEN merge would concatenate the bodies
// FIRST (`\x41` + `1` = `\x411`) and only then decode — and `\x411` is a hex
// escape over THREE digits, which decodes to the single byte 0x11 (an
// out-of-range \x would instead fail loud). Either way the output would be wrong.
//
// Runtime witness: `puts("\x41" "1")` prints exactly "A1" to captured stdout and
// main exits 0. A raw-merge regression prints a control byte (0x11) instead of
// "A1" — the harness's byte-for-byte stdout assertion catches it immediately.

extern int puts(const char* s);

int main() {
    puts("\x41" "1");
    return 0;
}
