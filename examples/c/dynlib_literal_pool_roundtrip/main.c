// The consumer half of `dynlib_literal_pool_roundtrip`. It imports the four
// entry points `dsslib.c` exports and scores each one, so the exit code says
// WHICH of the synthesized definition kinds came back wrong rather than only
// that something did.
//
// Deliberately libc-free: `streq` is written out so the example asserts the
// LIBRARY's literals and not a runtime's string routines, and so the exec arm
// needs no import beyond the library under test.

extern const char *dss_greet(void);
extern const char *dss_where(void);
extern int         dss_scaled_times_1000(int x);
extern int         dss_table_matches_greet(void);
extern int         dss_table_matches_private(void);

static int streq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return *a == *b ? 1 : 0;
}

int main(void) {
    int score = 0;
    // 8 — the string-literal pool object survived the image boundary intact.
    if (streq(dss_greet(), "hello, world")) score += 8;
    // 16 — `__func__` reaches the same pool through a different producer, and
    // its bytes are the DEFINING function's name, not the caller's.
    if (streq(dss_where(), "dss_where")) score += 16;
    // 16 — the promoted floating constants: -(2.0) * 1.5 * 1000.0 = -3000.
    if (dss_scaled_times_1000(2) == -3000) score += 16;
    // 1 — the static initializer's entry for an EXPORTED definition agrees with
    // the library's own address-take of it (C 6.2.2p2, inside one image).
    if (dss_table_matches_greet()) score += 1;
    // 1 — and the `static` control still resolves to its own body, which is
    // what stops the line above from being true for the wrong reason.
    if (dss_table_matches_private()) score += 1;
    return score;   // 8 + 16 + 16 + 1 + 1 = 42
}
