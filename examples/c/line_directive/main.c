// D-CPP-LINE-DIRECTIVE: `#line` (C23 6.10.4) sets the PRESUMED line — and
// optionally the presumed file name — reported by `__LINE__`/`__FILE__` for the
// lines that FOLLOW the directive. Without it, DSS rejected the directive
// outright (`P_PreprocessorUnsupported`), which is why sqlite could only be
// compiled through its AMALGAMATION: `mksqlite3c.tcl` STRIPS `#line` while
// generating `sqlite3.c`, whereas the lemon-generated `parse.c` carries 50 of
// them. Every C-generating tool emits them (lemon, bison, flex, re2c, protoc),
// so this is the gate on compiling generated C at all.
//
// THE SUBTLETIES THIS PINS (each would pass a naive implementation):
//   1. `#line N` numbers the line AFTER the directive N — not the directive's
//      own line. So the function on the next line reports exactly N.
//   2. The file operand is OPTIONAL (6.10.4p3) and when OMITTED the presumed
//      NAME is left UNCHANGED — a later bare `#line N` must NOT revert
//      `__FILE__` to the real file; it keeps the last name that was set.
//   3. Numbering continues to ADVANCE from the directive: two lines after
//      `#line N` reports N+1.
//
// Runtime-observable via exit code, with values no pass can const-fold away
// into a vacuous witness: `__LINE__` is materialized by the PREPROCESSOR, so the
// `release` arm proves the optimizer preserves it end-to-end.
//
// RED-ON-DISABLE: drop the `lineDirective` arm in preprocessor.cpp and the
// directive fails loud again (no binary at all). Break the presumed-line
// arithmetic (`N + physLine - directiveLine - 1`) and check 1 or 3 fails; drop
// the "omitted file leaves the name unchanged" rule and check 2 fails.

#line 100
static int at_100(void) { return __LINE__; }        // presumed line 100
static int at_101(void) { return __LINE__; }        // numbering advances -> 101

#line 500 "virtual_generated.c"
static int at_500(void) { return __LINE__; }        // presumed 500, file renamed

// A BARE `#line` (no file operand): the presumed NAME must persist as
// "virtual_generated.c" — only the numbering changes.
#line 900
static int at_900(void) { return __LINE__; }
static const char* presumed_file(void) { return __FILE__; }

static int str_eq(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return *a == *b;
}

int main(void) {
    if (at_100() != 100) return 11;   // #line N numbers the FOLLOWING line
    if (at_101() != 101) return 12;   // numbering advances from there
    if (at_500() != 500) return 13;   // second directive, with a file operand
    if (at_900() != 900) return 14;   // bare directive still renumbers
    // The bare `#line 900` must NOT have reverted the name to main.c.
    if (!str_eq(presumed_file(), "virtual_generated.c")) return 15;
    return 20;
}
