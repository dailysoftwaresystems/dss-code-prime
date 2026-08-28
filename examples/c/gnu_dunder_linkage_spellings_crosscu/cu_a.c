// D-C-LINKAGE-SPECIFIER-LOOKUP-IS-POSITION-BLIND-AND-NOT-DUNDER-NORMALIZED
// (P42 lane V) witness: the GNU `__`-wrapped spellings of the two LINKAGE
// attributes DSS actually models - `__weak__` and `__visibility__` - carried
// across a real cross-TU LINK. The observable is what the LOADED PROGRAM sees,
// not that the declaration parsed.
//
// * WHY THIS EXAMPLE EXISTS. At the start of this cycle every dunder spelling
// was a hard compile error, in every position, on objects and on functions:
//
//     __attribute__((__weak__)) int gv;
//         error[H000C] '__weak__' is not a recognized linkage specifier
//     __attribute__((__visibility__("hidden"))) int gv;
//         error[H000C] '__visibility__:hidden' is not a recognized linkage specifier
//
// while the bare spellings compiled clean. The cause was that `linkageFrom`'s
// strict lookup ran on RAW TOKEN TEXT while `linkageSpecifierIgnoredNames`, on
// the SAME tier, was `stripDunder`-normalized - two lookups over one vocabulary
// disagreeing about normalization. These are the spellings glibc and the macOS
// SDK write, so the refusal was reachable from ordinary headers.
//
// ** NON-VACUITY: EVERY WEAK CHECK HERE IS EXIT-CODE DISCRIMINATING IN BOTH
// DIRECTIONS, which is what makes this file worth more than "it compiled".
//   * If the dunder spelling is REFUSED (the pre-change behaviour), the program
//     does not build at all and the runner reports a compile failure.
//   * If the dunder spelling is ACCEPTED and then SILENTLY DISCARDED - the
//     failure mode this project ranks BELOW a loud refusal - then `wfun`,
//     `wd_lead` and `wd_tail` are STRONG here, cu_b.c defines each of them
//     strongly too, and the link fails with a duplicate symbol
//     (`K_SymbolRedefinedAcrossUnits` in DSS, `ld: duplicate symbol` in clang).
//     Nothing in this file can pass by accident.
// Rows 3/4/5 below are therefore real strong-over-weak resolution being
// witnessed at link time, in the dunder spelling.
//
// (i) THE HONEST SPLIT, stated rather than papered over: the three
// `__visibility__` rows are COMPILE-GATE witnesses. Hidden visibility does not
// change what a statically linked call returns, so deleting it cannot move an
// exit code - its disable is disconnecting the normalization, which turns it
// back into a loud H000C. That IS the regression this file guards, so the row
// belongs here; the side-table half is pinned where it can be READ, in
// `HirLoweringC.GnuDunderLinkageSpellingsResolveLikeThePlainOnes` and
// `HirLoweringC.GnuDunderLinkageSpellingsApplyInObjectAndExternPositions`
// (tests/hir/test_hir_lowering_c.cpp), which compare the dunder form's folded
// `LinkageAttr` against the plain form's. Neither file is sufficient alone.
//
// ** VALID C, VERIFIED, NOT ASSUMED. Both references probed SEPARATELY with
// per-compiler std flags (gcc REJECTS `-std=c23`): gcc 13.3.0 `-std=c2x` and
// clang 18.1.3 `-std=c23`, each with `-Wall -Wextra`, over BOTH CUs - zero
// errors, zero warnings - and each linked binary independently EXITS 42, so the
// expected exit code is ground truth from a real toolchain rather than DSS
// agreeing with itself. Re-run both checks if you touch either file.
//
// Front-end feature (attribute-name normalization -> linkage sink) carried
// through HIR->MIR->link, target/format-agnostic, baseline AND the shipped
// `release` pipeline - the optimized arm is mandatory for the same reason as the
// bare-spelling sibling `gnu_attribute_linkage_positions_crosscu`: the point is
// that a real optimizer PRESERVES weak binding rather than inlining through it.

/* (3) weak FUNCTION definition, DUNDER spelling, LEADING position. cu_b.c
   defines `wfun` strongly, so strong-over-weak must make the call return 12,
   never this 100. */
__attribute__((__weak__)) int wfun(void) { return 100; }

/* (4) weak DATA definition, DUNDER spelling, LEADING position. cu_b.c defines
   it strongly as 5. */
__attribute__((__weak__)) int wd_lead = 100;

/* (5) weak DATA, DUNDER spelling, AFTER-DECLARATOR position, and PER-DECLARATOR:
   `wd_tail` is weak (cu_b.c's strong 5 supersedes this 100), `local_b` is NOT
   and stays 2. */
int wd_tail __attribute__((__weak__)) = 100, local_b = 2;

/* (2) A weak PROTOTYPE in the dunder spelling, no body here - the definition
   lives in cu_b.c and is resolved at LINK. */
int wp(void) __attribute__((__weak__));

/* (9) The after-`extern` position - the shape glibc writes and the one tcl.h
   reaches through `#define EXTERN extern TCL_STORAGE_CLASS`. This is a DIFFERENT
   scan root (`externDecl`) with its own linkage map, so it is not covered by any
   of the rows above. */
extern int e_weak __attribute__((__weak__));

/* (6)(7) `__visibility__("hidden")` in BOTH positions, on functions that ARE
   called, so a visibility sink that wrongly made them DCE-food would fail the
   link. */
__attribute__((__visibility__("hidden"))) int vis_lead(void) { return 3; }
int vis_tail(void) __attribute__((__visibility__("hidden")));
int vis_tail(void) { return 4; }

/* (8) `__visibility__("default")` - the composite twin, and the exact spelling
   the inverted unit pin used to refuse by name. */
__attribute__((__visibility__("default"))) int vis_dflt(void) { return 6; }

int main(void) {
    if (wp()       !=  8) return 2;   /* weak proto,  dunder, sibling-TU body  */
    if (wfun()     != 12) return 3;   /* weak fn def, dunder  -> strong wins   */
    if (wd_lead    !=  5) return 4;   /* weak data,   dunder, LEADING          */
    if (wd_tail    !=  5) return 5;   /* weak data,   dunder, TRAILING         */
    if (vis_lead() !=  3) return 6;   /* __visibility__("hidden"), LEADING     */
    if (vis_tail() !=  4) return 7;   /* __visibility__("hidden"), TRAILING    */
    if (vis_dflt() !=  6) return 8;   /* __visibility__("default")             */
    if (e_weak     !=  9) return 9;   /* weak extern DECLARATION, dunder       */
    return local_b + 40;              /* 42 - the non-weak sibling declarator  */
}
