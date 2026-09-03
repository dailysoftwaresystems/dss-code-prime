// P55 D-SEMANTIC-DEPTH-CAPS-TRUNCATE-INTO-TWO-WRONG-ANSWERS (positive, RUNNABLE):
// legal declarators PAST the depths the qualifier walk used to stop at must be
// ACCEPTED, and the program built from them must run and give the right answer.
//
// This is the OVER-REFUSAL control for the negative twin
// (`deep_declarator_qualifier_redecl_error`), and it is the direction that
// matters most: a redeclaration check fails in two opposite ways, and only the
// pair separates a fix that landed from one that over-reached. Every declaration
// below is written TWICE and IDENTICALLY — one legal declaration spelled twice,
// which C23 6.7.6.3p15 requires an implementation to accept — at depths past both
// removed caps:
//
//   * `lvl5`  — FIVE levels of nested function-pointer parameters (past the old
//               `nestDepth >= 4`), and it is CALLED through the whole tower, so
//               the artifact witnesses the deep shape end to end and not merely
//               the redeclaration oracle.
//   * `deep_pp` — TWENTY nested parenthesized declarators (past the old
//               `depth > 16`; ISO C23 5.2.4.1 obliges 63), declared, defined and
//               DEREFERENCED twice over.
//
// ✔All four references accept every declaration here, probed SEPARATELY: gcc
// 13.3.0 (`-std=c2x -pedantic-errors`), clang 18.1.3 (`-std=c23
// -pedantic-errors`), mingw-w64 gcc 13.2.0 and MSVC 19.51.36252
// (`/std:clatest`). DSS must too — the same-shaped refusal was MEASURED on the
// sibling half of this row, where a 17-level pointer chain against a
// shipped-library descriptor row was rejected outright.
//
// Exit arithmetic: the tower delivers "dss" to `lvl0`, which adds 'd' (100) to
// `g_hits`; `**deep_pp` reads back 'k' (107). 100 + 107 = 207 and main returns 0
// only for exactly that. A dropped tower level, a mis-ordered qualifier spine or
// a broken deep declarator cannot produce it.

static char const *g_msg  = "dss";
static int         g_hits = 0;

static void lvl0(const char *s) { g_hits += (int)(unsigned char)s[0]; }
static void lvl1(void (*f)(const char *)) { f(g_msg); }
static void lvl2(void (*f)(void (*)(const char *))) { f(lvl0); }
static void lvl3(void (*f)(void (*)(void (*)(const char *)))) { f(lvl1); }
static void lvl4(void (*f)(void (*)(void (*)(void (*)(const char *))))) { f(lvl2); }

// THE SUBJECT: five levels of nested function-pointer parameters, declared twice
// identically. Before P55 the nested-parameter claim gave up at four levels, so
// the qualifier axis of THIS declaration was never judged at all.
void lvl5(void (*)(void (*)(void (*)(void (*)(void (*)(const char *))))));
void lvl5(void (*)(void (*)(void (*)(void (*)(void (*)(const char *))))));

void lvl5(void (*f)(void (*)(void (*)(void (*)(void (*)(const char *)))))) {
    f(lvl3);
}

// THE SECOND SUBJECT: twenty nested parenthesized declarators around the inner
// `*`. The declared type is a plain `char **` — the parentheses are redundant and
// legal, and the point is that reading the qualifier spine through them no longer
// runs out of depth.
static char  deep_buf[1] = { 'k' };
static char *deep_row    = deep_buf;
extern char *((((((((((((((((((((*deep_pp))))))))))))))))))));
char *((((((((((((((((((((*deep_pp)))))))))))))))))))) = &deep_row;

int main(void) {
    lvl5(lvl4);
    return (g_hits == 100 && **deep_pp == 'k') ? 0 : 1;
}
