// D-PP-ELIFDEF-ELIFNDEF (C23 6.10.1) runtime witness -- the `#elifdef`/
// `#elifndef` chain. FEATURE_B is defined; FEATURE_A and FEATURE_C are NOT.
//
//   #ifdef FEATURE_A     -> false (dead)                 pick = 1
//   #elifdef FEATURE_B   -> TRUE, TAKEN                  pick = 42   <-- answer
//   #elifndef FEATURE_C  -> !defined(C) is TRUE, but an earlier arm already
//                           took, so the taken-once latch MUST keep it dead
//                           (it must NOT fire)           pick = 3
//   #else                                                 pick = 3
//
// `pick` is a PRE-DECLARED var REASSIGNED per arm (never redeclared), so if a
// regression let two arms both fire (e.g. the taken-once latch broke and the
// `#elifndef` arm also ran after the `#elifdef` arm) the LAST assignment wins
// -> a distinct exit 3, NOT an `int pick` redeclaration compile error that
// would MASK the miscompile.
//
// THE PRE-FIX SILENT MISCOMPILE: with `#elifdef`/`#elifndef` unrecognized, both
// are silently consumed inside the dead `#ifdef FEATURE_A` group and control
// falls through to `#else` -> pick = 3 -> exit 3, with NO compile/link error.
//
// Fold-resistance: `pick` reaches `identity` as a FUNCTION ARGUMENT, so the
// baseline arm keeps a live runtime value. The release arm runs the shipped
// pipeline over the same source.
//
//   correct -> pick = 42 -> exit 42 ; any wrong fall-through -> pick = 3

#define FEATURE_B

int identity(int v) { return v; }

int main(void) {
    int pick = 9;   /* sentinel: reached only if NO arm fires */
#ifdef FEATURE_A
    pick = 1;
#elifdef FEATURE_B
    pick = 42;
#elifndef FEATURE_C
    pick = 3;
#else
    pick = 3;
#endif
    return identity(pick);
}
