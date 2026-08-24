// D-PP-STRINGIZE-EXPANDED-ARG-SLICES-WRONG-BYTES — the `#` (stringize) runtime
// witness for an argument that arrived through a MACRO EXPANSION.
//
// C 6.10.3.2 / C23 6.10.5.2: `#` replaces its operand with a string literal
// spelling that operand's PREPROCESSING TOKENS — each token's own spelling, with
// one space wherever white space separated two tokens and none where it did not.
//
// ★★ WHY THIS EXAMPLE EXISTS. DSS used to build that literal by slicing ONE
// contiguous source byte range, `[first token's start, last token's end)`. That is
// only exact while the operand really is unbroken call-site text. The two-level
// `XSTR(...)` idiom — the standard way every real project stringifies something
// AFTER expanding it (version macros, assertion macros) — breaks the premise: the
// inner macro's argument has already been expanded, so its tokens come from the
// `#define` line, the call site and the product buffer INTERLEAVED, and a range
// spanning them reads whatever lies between two unrelated positions. Measured
// symptoms, all with exit code 0 and no diagnostic: the `#define` line's own text
// (`"g(a, b)"` for what must be `"g(1, 2)"`), a COMMENT copied into the literal,
// and — for `x P(1,2)` — the ENTIRE REMAINDER OF THE FILE inside one string. A
// silent wrong-value miscompile, which is why the fix is a runtime witness and not
// only a unit test.
//
// Every expectation below was ✔MEASURED on four reference preprocessors that
// agreed exactly: clang-18 `-std=c23`, clang-19 `-std=c23`, gcc-13 `-std=c2x`,
// and cl 19.51.36252 `/std:clatest /Zc:preprocessor`. The printed lines ARE the
// assertion — a wrong spelling changes stdout and reds the arm.
//
// What the printed lines pin, in order:
//
//  (1) THE CORE DIFFERENTIAL — `XSTR(PLAIN(1,2))` is `"g(1, 2)"`. The single-level
//      control on the line before it is `"PLAIN(1,2)"`, because a `#` operand is
//      never pre-expanded: the two together prove the fix did not simply start
//      expanding everything.
//
//  (2) ASYMMETRIC SPACING IS THE CORRECT ANSWER — `XSTR(VA(1,2,3))` is
//      `"k(1, 2,3)"`. The `, ` comes from VA's replacement list and the `2,3` from
//      the call site, where the source wrote no space. A blind space-join between
//      tokens would say `"k(1, 2, 3)"`, so this line refutes that shortcut too.
//
//  (3) ★ WHITE SPACE INSIDE A TOKEN'S SPELLING SURVIVES VERBATIM. 6.10.5.2p2
//      collapses white space BETWEEN preprocessing tokens; the two spaces in
//      `"a  b"` are inside ONE token and are not between anything.
//      `f("a  b" ,   "c  d")` is the case that pins both rules at once — collapse
//      between, preserve inside, in a single product — and it is only expressible
//      per-token, which is the second reason the byte-range slice had to go.
//
//  (4) A COMMENT IS WHITE SPACE (translation phase 3), so `S(a/*x*/b)` is `"a b"`.
//      The slice used to copy the comment's characters into the literal.
//
//  (5) ADJACENCY SURVIVES EVERY BOUNDARY A TOKEN CROSSES — a substitution taking a
//      macro name's place (`a PLAIN(1,2)` vs `a+PLAIN(1,2)`) and a `##` product
//      standing where its left operand stood (`x P(1,2)` vs `x+P(1,2)`).
//
// Fold-resistance: the runtime checks compare the stringize products with
// `strcmp`-style walks over `expect`, and the arithmetic that produces the exit
// code is seeded through `ident`, an opaque pass-through, so the baseline (`debug`)
// arm keeps live runtime work. The `release` arm re-runs the whole program through
// the shipped optimizer pipeline — a preprocessor that spelled a literal
// differently would diverge from 42 in both arms, and a constant-folder that
// mangled string data would diverge in the release one.
//
// Exit 42 on success; each check has its own distinct failure code so a red arm
// names which rule broke.

extern int puts(const char* s);

int ident(int x) {
    return x;
}

// Compare two NUL-terminated byte strings. Returns 1 iff every byte matches.
int sameBytes(const char* a, const char* b) {
    int i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) {
            return 0;
        }
        i = i + 1;
    }
    return (a[i] == 0 && b[i] == 0) ? 1 : 0;
}

/* The two-level stringize idiom: STR spells its operand, XSTR expands first. */
#define STR(...)   #__VA_ARGS__
#define XSTR(...)  STR(__VA_ARGS__)
#define S(x)       #x

/* Inner macros whose expansions mix `#define`-line and call-site tokens. */
#define PLAIN(a,b) g(a, b)
#define VA(f,...)  k(f, __VA_ARGS__)
#define MIX(a)     q(a, w)
#define TWO(a,b)   a b
#define P(a,b)     a##b

int main(void) {
    /* (1) the core differential, with its single-level control. */
    if (!sameBytes(STR(PLAIN(1,2)), "PLAIN(1,2)")) {
        return 1;
    }
    if (!sameBytes(XSTR(PLAIN(1,2)), "g(1, 2)")) {
        return 2;
    }
    if (!sameBytes(XSTR(PLAIN(PLAIN(1,2),3)), "g(g(1, 2), 3)")) {
        return 3;
    }
    if (!sameBytes(XSTR(MIX(z)), "q(z, w)")) {
        return 4;
    }

    /* (2) asymmetric spacing is correct, not a bug to be smoothed over. */
    if (!sameBytes(XSTR(VA(1,2,3)), "k(1, 2,3)")) {
        return 5;
    }
    /* A replacement list's own space, with call-site values substituted in. */
    if (!sameBytes(XSTR(TWO(1,2)), "1 2")) {
        return 6;
    }

    /* (3) white space INSIDE a token's spelling survives verbatim. */
    if (!sameBytes(S("a  b"), "\"a  b\"")) {
        return 7;
    }
    if (!sameBytes(S('a  b'), "'a  b'")) {
        return 8;
    }
    /* ...and between-token white space still collapses, in the SAME product. */
    if (!sameBytes(S(f("a  b" ,   "c  d")), "f(\"a  b\" , \"c  d\")")) {
        return 9;
    }
    /* ...even when the literal reaches `#` through an expansion. */
    if (!sameBytes(XSTR(PLAIN("a  b",2)), "g(\"a  b\", 2)")) {
        return 10;
    }

    /* (4) a comment is white space and must never reach the literal. */
    if (!sameBytes(S(a/*x*/b), "a b")) {
        return 11;
    }
    if (!sameBytes(S(a/*x*/  b), "a b")) {
        return 12;
    }

    /* (5) adjacency across a substitution boundary... */
    if (!sameBytes(XSTR(a PLAIN(1,2)), "a g(1, 2)")) {
        return 13;
    }
    if (!sameBytes(XSTR(a+PLAIN(1,2)), "a+g(1, 2)")) {
        return 14;
    }
    /* ...and across a `##` product, which stands where its left operand stood. */
    if (!sameBytes(XSTR(x P(1,2)), "x 12")) {
        return 15;
    }
    if (!sameBytes(XSTR(x+P(1,2)), "x+12")) {
        return 16;
    }

    /* Leading/trailing white space of the argument is deleted (6.10.5.2p2). */
    if (!sameBytes(XSTR(   PLAIN(1,2)   ), "g(1, 2)")) {
        return 17;
    }
    /* An empty stringizing argument is the empty literal (6.10.5.2p4). */
    if (!sameBytes(XSTR(), "")) {
        return 18;
    }

    /* The products are also PRINTED, so stdout pins the exact bytes and a
       wrong spelling cannot hide behind a comparison that both sides got wrong. */
    puts("[" XSTR(PLAIN(1,2)) "]");
    puts("[" XSTR(VA(1,2,3)) "]");
    puts("[" S(f("a  b" ,   "c  d")) "]");
    puts("[" XSTR(x P(1,2)) "]");

    return ident(40) + ident(2);
}
