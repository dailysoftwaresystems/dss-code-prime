/* c44 (D-CSUBSET-SHIPPED-TYPEDEF-CAST-PARSE) — a SHIPPED-typedef cast
 * `(size_t)(parenExpr)` must parse as a CAST, not a function call. `size_t` comes
 * from DSS's shipped <stddef.h> and is injected SEMANTICALLY (post-parse), so the
 * parser's type-name table never saw it and disambiguated `(size_t)((char*)...)`
 * as a call `size_t(...)` → the array dimension was non-constant → an
 * incomplete-array FAM → S001C. This is sqlite's EXACT offsetof shape
 * (`((size_t)((char*)&((ST*)0)->M - (char*)0))`, sqlite3.c:24984 keyinfoSpace) —
 * c43's offsetof fold was correct but never reached it (the c43 corpus used a
 * SOURCE typedef, which the parser DOES track, masking this). The fix (c44) gave the
 * shipped descriptor's typedef NAMES to the parse; since
 * D-PERF-2-TYPEDEF-SEED-DISAMBIGUATION they are seeded into the binder sketch BEFORE
 * the file's one parse (the post-parse oracle and its reparse are retired,
 * D-PP-FC2-ORACLE-RETIRE), so the cast commits on that parse; c43 then folds the
 * offsetof.
 *
 * RED-ON-DISABLE: drop the descriptor typedef-name seed -> `size_t` is no type name
 * at the parse -> `(size_t)(parenExpr)` is not read as a cast -> the program does
 * not compile. */
#include <stddef.h>

struct K { int n; int m; };   /* m at byte offset 4 */

/* the sole-member `struct uKey` shape, sized by a SHIPPED-(size_t) offsetof */
struct uKey { char keyinfoSpace[ ((size_t)((char*)&((struct K*)0)->m - (char*)0)) ]; };

int main(void) {
    /* a bare shipped-(size_t) cast of a PARENTHESIZED operand — the smoking gun
     * (this is what parsed as a call before the fix). */
    size_t v = (size_t)(2 + 3);
    if (v != 5) return 1;

    /* the shipped-(size_t) offsetof folds -> uKey is char[offsetof(K,m)] = char[4]. */
    if (sizeof(struct uKey) != 4) return 2;

    return 42;
}
