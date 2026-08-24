/* c11 D-CSUBSET-EAST-CONST-CAST runtime witness.
 *
 * An EAST-const cast — `const` between the base type and the `*`
 * (`(unsigned char const *)`) — the SQLite-pervasive pointer-to-const cast form
 * (50+ in the sqlite3.c amalgamation). West-const `(const char *)` and the
 * declaration head `char const *p` already parsed; only the cast/sizeof
 * type-name (`castTypeRef`, which bundles base + stars) lacked the post-base
 * const slot. The cast parses, the deref reads the value → exit 42.
 *
 * RED-ON-DISABLE: remove the post-base `{optional ConstKeyword}` from
 * `castTypeRef` and `(unsigned char const *)` fails to parse (P0009 at the `*`). */
int main(void) {
    unsigned char b = 42;
    unsigned char const *p = (unsigned char const *)&b;   /* east-const cast */
    return (int)*p;
}
