// FC18a (D-PP-VA-OPT) — the C23 `__VA_OPT__` runtime witness.
//
// C23 6.10.5.1 (Argument substitution) adds the va-opt-replacement
// `__VA_OPT__ ( pp-tokens_opt )`: inside a VARIADIC function-like macro's
// replacement list it yields its content when the variable arguments have a
// NON-EMPTY substitution, and a placemarker (i.e. nothing) otherwise. It is the
// standard's replacement for the GNU `,##__VA_ARGS__` comma-elision idiom, and
// it is the ONLY item the C23 conformance census still listed as MANDATED rather
// than an extension.
//
// Every expectation below was ✔MEASURED on three reference preprocessors that
// agreed exactly — clang-18 `-std=c23`, clang-19 `-std=c23`, gcc-13 `-std=c2x`
// — and, for the expansion arm, on cl 19.51.36252 `/std:clatest
// /Zc:preprocessor` as a fourth.
//
// What each check pins:
//
//  (1) PRESENCE/ABSENCE — `ADD(base, ...)` emits its `+` only when trailing
//      arguments exist. `ADD(40)` = 40, `ADD(40, 2)` = 42.
//
//  (2) ★★ THE `EMPTY`-MACRO DISCRIMINATOR, and it is the reason this example
//      exists. 6.10.5.1p7 keys the choice on "a (hypothetical) substitution of
//      __VA_ARGS__ as neither an operand of # nor ##" — the MACRO-EXPANDED
//      variable arguments, NOT the raw ones. `ADD(42, EMPTY)` passes one RAW
//      argument token whose SUBSTITUTION is empty, so the `+` must vanish. An
//      implementation that tested the raw run would emit `(42 + )`, which does
//      not even parse — so this line is a COMPILE-time tripwire as well as a
//      runtime one. (The standard states the same case as `F(EMP)` in its own
//      EXAMPLE 2.) Note the GNU idiom deliberately answers DIFFERENTLY here —
//      ✔MEASURED `g(1 , )` for the GNU spelling of the identical call — which is
//      exactly why the two are separate features and not two spellings of one.
//
//  (3) `##` INSIDE the content — `__VA_OPT__(a ## b)` pastes normally, building
//      the identifier `val_ok`. (The standard's H2 shape.)
//
//  (4) ★ PLACEMARKER SEMANTICS ACROSS AN OUTSIDE `##` — `base ## __VA_OPT__(_x)`.
//      With no trailing arguments the va-opt is a PLACEMARKER, not "nothing", so
//      the paste finds an operand and collapses to `base` rather than tripping
//      the dangling-`##` constraint (C23 6.10.5.3p3). With arguments it pastes to
//      `base_x`. ✔MEASURED `z` / `zw` for the reduced form on all three oracles.
//
//  (5) `__VA_ARGS__` USED INSIDE the content, under nested parentheses — the
//      closing `)` of a va-opt is found "by skipping intervening pairs of
//      matching left and right parentheses" (6.10.5.1p3), so a call expression
//      inside the content does not terminate it early.
//
//  (6) THE STANDARD'S `SDEF` SHAPE — an optional `= { ... }` initializer, the
//      motivating real-world use.
//
//  (7) STRINGIZING a va-opt (`#__VA_OPT__(...)`, valid because 6.10.5.1p4 makes a
//      va-opt-replacement "treated as if it were a parameter"). The four printed
//      lines pin the C23 6.10.5.2p3 spelling rules, which are NOT a blind
//      space-join: white space between the content's tokens becomes ONE space and
//      where there was NONE none is inserted. ✔MEASURED `""`, `"a+b"`, `"p+p"`,
//      `"a + b"` — note `SPAR(p, 1)` is `"p+p"`, with the parameter substituted
//      and the original adjacency preserved.
//
// Fold-resistance: every arithmetic operand is seeded through `ident`, an opaque
// pass-through, so the baseline (`debug`) arm keeps live runtime arithmetic. The
// `release` arm re-runs the whole program through the shipped optimizer pipeline
// — a va-opt that expanded differently under folding would diverge from 42.
//
// Exit 42 on success; each check has its own distinct failure code so a red arm
// names which rule broke.

extern int puts(const char* s);

int ident(int x) {
    return x;
}

int sum2(int a, int b) {
    return a + b;
}

struct Pair {
    int a;
    int b;
};

/* (1)/(2) presence-absence of an operator */
#define ADD(base, ...) (base __VA_OPT__(+) __VA_ARGS__)
#define EMPTY

/* (3) `##` inside the content */
#define NAMEIF(a, b, ...) __VA_OPT__(a ## b)

/* (4) placemarker across an OUTSIDE `##` */
#define OPT_SUFFIX(...) base ## __VA_OPT__(_x)

/* (5) `__VA_ARGS__` inside the content, under nested parens */
#define WRAP(...) (0 __VA_OPT__(+ sum2(__VA_ARGS__)))

/* (6) the standard's SDEF shape */
#define SDEF(sname, ...) struct Pair sname __VA_OPT__(= { __VA_ARGS__ })

/* (7) stringizing a va-opt */
#define SEMPTY(...) #__VA_OPT__(a+b)
#define STIGHT(...) #__VA_OPT__(a+b)
#define SPAR(X, ...) #__VA_OPT__(X+X)
#define SSPACED(...) #__VA_OPT__(a + b)

int val_ok = 7;
int base   = 5;
int base_x = 9;

int main(void) {
    /* (1) the `+` appears only when trailing arguments do. */
    int oneArg  = ADD(ident(40));
    int twoArgs = ADD(ident(40), ident(2));
    if (oneArg != 40) {
        return 1;
    }
    if (twoArgs != 42) {
        return 2;
    }

    /* (2) ★ an argument that is PRESENT but expands to NOTHING is empty. */
    int viaEmptyMacro = ADD(ident(42), EMPTY);
    if (viaEmptyMacro != 42) {
        return 3;
    }

    /* (3) `##` inside the content builds `val_ok`. */
    int pasted = NAMEIF(val, _ok, 1);
    if (pasted != 7) {
        return 4;
    }

    /* (4) placemarker across an outside `##`: `base` vs `base_x`. */
    int noSuffix = OPT_SUFFIX();
    int suffixed = OPT_SUFFIX(1);
    if (noSuffix != 5) {
        return 5;
    }
    if (suffixed != 9) {
        return 6;
    }

    /* (5) `__VA_ARGS__` inside the content, under nested parens. */
    int wrapNone = WRAP();
    int wrapSome = WRAP(ident(20), ident(22));
    if (wrapNone != 0) {
        return 7;
    }
    if (wrapSome != 42) {
        return 8;
    }

    /* (6) the SDEF shape: with and without the optional initializer. */
    SDEF(pairPlain);
    SDEF(pairInit, 20, 22);
    pairPlain.a = ident(1);
    pairPlain.b = ident(2);
    if (pairPlain.a + pairPlain.b != 3) {
        return 9;
    }
    if (pairInit.a + pairInit.b != 42) {
        return 10;
    }

    /* (7) stringizing a va-opt — the four printed lines ARE the assertion. */
    puts("[" SEMPTY() "]");
    puts("[" STIGHT(1) "]");
    puts("[" SPAR(p, 1) "]");
    puts("[" SSPACED(1) "]");

    return twoArgs;
}
