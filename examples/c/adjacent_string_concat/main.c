// c20 (D-CSUBSET-ADJACENT-STRING-CONCAT): C 5.1.1.2 phase 6 — adjacent string
// literals concatenate. `"hel" "lo"` is exactly `"hello"`; the two source pieces
// are decoded (phase 5) and byte-joined (phase 6) by the SINGLE
// decodeAdjacentStringBodies chokepoint that HIR lowering and the semantic typer
// both route through. A SQLite compile blocker (sqlite3.c splits long string
// literals across adjacent pieces pervasively).
//
// Runtime witness (modeled on hello_puts): the concatenated literal "hello" is
// passed to msvcrt/glibc/libSystem `puts`, prints "hello" to captured stdout,
// and main exits 42 — both asserted byte-for-byte by the examples_runner. A
// regression that dropped the second piece would print "hel" (stdout mismatch);
// a regression that mis-sized the Array<Char,N> would corrupt the NUL terminator
// and `puts` would over-read. The release arm runs the same through the shipped
// optimizer pipeline.

extern int puts(const char* s);

int main() {
    puts("hel" "lo");
    return 42;
}
