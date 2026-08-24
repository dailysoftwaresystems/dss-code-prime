// c22 (D-PP-LINE-COMMENT-BEFORE-DIRECTIVE): a `//` line comment that SHARES a
// line with code, immediately followed by a `#` directive on the next line,
// must still recognize the directive. The bug consumed the comment's
// terminating newline as a comment-body token, so the directive lost its line
// boundary (firstOnLine saw the code before the comment) and leaked to the
// parser as a malformed `#`-construct. The fix: the `//` mode ends AT but
// EXCLUDES the newline (stringStyle.endsAtExclusive), so the newline survives as
// a Newline token. If the directive below were NOT processed, `ADD` would be an
// undefined identifier and this would fail to compile -> no binary -> test fails.
int main(void) {
    int r = 5;     // trailing comment on a code line, then a directive next line
#define ADD 37
    return r + ADD;   // 5 + 37 = 42, iff the #define after `code // comment` ran
}
