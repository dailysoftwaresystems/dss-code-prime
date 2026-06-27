// c20 (D-CSUBSET-ADJACENT-STRING-CONCAT) — macro-expanded adjacent concatenation.
// C 5.1.1.2 phase 6 runs AFTER macro expansion (phase 4): an object-like macro
// whose replacement is a string literal concatenates with a syntactically
// adjacent string literal. `#define S "hel"` then `puts(S "lo")` expands to
// `puts("hel" "lo")` → "hello". This is the exact SQLite idiom (sqlite3.c builds
// long messages from a macro prefix + an adjacent literal tail).
//
// What this proves that adjacent_string_concat does not: the two adjacent pieces
// need NOT both be written literally at the call site — one can arrive via macro
// expansion, and the grammar's adjacent-pair repeat still fuses them into ONE
// stringLiteralExpr that the decode chokepoint joins.
//
// Runtime witness: prints "hello" to captured stdout, main exits 42. A dropped
// piece (either the macro body or the trailing literal) flips stdout and the
// harness fails immediately.

extern int puts(const char* s);

#define S "hel"

int main() {
    puts(S "lo");
    return 42;
}
