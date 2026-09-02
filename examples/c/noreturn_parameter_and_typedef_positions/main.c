// D-CSUBSET-NORETURN-KEYWORD-PARAMETER-AND-TYPEDEF-POSITIONS (C11 6.7.4p2):
// the `_Noreturn` KEYWORD written in the two grammar positions DSS used to
// refuse outright with P_NoAlternativeMatched — a PARAMETER's specifiers and a
// TYPEDEF head. MEASURED 2026-09-01, each reference probed SEPARATELY at
// -std=c11 and -std=c2x: gcc 13.3.0 ACCEPTS both (rc=0, "parameter 'p'
// declared '_Noreturn'" / "typedef 'tfn' declared '_Noreturn'") and IGNORES
// the specifier; clang 18.1.3 and MSVC 19.51 reject both. The disjunction
// decides ACCEPTANCE, so DSS parses both and reproduces gcc's meaning: the
// specifier is DROPPED.
//
// ** WHAT THE EXIT CODE PROVES, AND WHY IT NEEDED AN EXECUTING ARTIFACT.
// `bump_tally` is declared THROUGH the decorated alias `tfn`. If DSS had
// honored the keyword on the typedef head, a call to `bump_tally` would lower
// to `Block{ ExprStmt(call), Unreachable }` and EVERY statement after the call
// would be dead code — `tally` would never be added and the program would not
// return 42 (it would not return at all). gcc does NOT honor it there either:
// with `-Wreturn-type`, `typedef _Noreturn void tfn(void); tfn f; int
// probe(void){ f(); }` warns "control reaches end of non-void function"
// EXACTLY as the undecorated control does, while the all-references-accept
// spelling `typedef void tfn(void); _Noreturn tfn f;` warns not at all. So
// exit 42 is the two compilers agreeing on what this program MEANS.
//
// The parameter half is proved the same way: `take` must RETURN so that its
// caller keeps running, which it only does because `p`'s `_Noreturn` was
// dropped rather than bound to anything.
//
// The loop + the inlinable `bump` exist for the release arm:
// mustDifferFromBaseline needs a body the pipeline can actually transform.

typedef _Noreturn void tfn(void);   // the TYPEDEF HEAD position

static int tally = 0;

tfn bump_tally;                     // declared through the decorated alias
void bump_tally(void) { tally = tally + 1; }

// the PARAMETER position, on a pointer-to-function parameter
static void take(_Noreturn void (*p)(void)) {
    p();
    tally = tally + 1;
}

static int bump(int v) { return v + tally; }

int main(void) {
    int acc = 0;
    for (int i = 0; i < 40; i = i + 1) {
        acc = acc + 1;
    }
    // If the keyword had been honored in EITHER position, control would not
    // reach the return: the call through `p` and the call to `bump_tally`
    // would each terminate the block.
    take(bump_tally);
    return bump(acc);
}
