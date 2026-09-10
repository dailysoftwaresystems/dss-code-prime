// D-CSUBSET-GNU-ATTRIBUTE (TF-C72/C73) witness, CU A: LINKAGE-bearing GNU
// attributes in BOTH positions — LEADING and AFTER-DECLARATOR — carried across
// a real CROSS-TU LINK. The observable is what the LOADED PROGRAM sees, not
// that the declaration parsed.
//
// ★ WHY THIS EXAMPLE EXISTS. An adversarial audit named two shapes as the
// hardest regressions in this area, and BOTH were REPRODUCED as hard compile
// failures against the tree as it stood at the start of this cycle:
//
//     extern int gg(void) __attribute__((__nothrow__, __leaf__));
//         error[H000C] got '__attribute__' is not a recognized linkage specifier
//     int f(void) __attribute__((weak));   /* definition in a sibling TU */
//         error[H0009] HIR Ref to unbound symbol 75
//
// The first is the LITERAL glibc idiom — `<stdio.h>` spells `remove`, `rename`
// and most of the rest exactly that way, so every real C header hit it. Both
// now compile and run; this file is the ratchet that keeps them that way.
//
// ★★ NON-VACUITY, PROVEN PER CHECK, WITH THE HONEST SPLIT STATED UP FRONT.
// Each check was isolated into its OWN two-CU program carrying ONLY the
// declarations that check needs — not a shared header, because a shared header
// makes one broken declaration abort the whole compile and tells you nothing
// about any particular check. Each isolated program was then built three ways:
// as written; with EXACTLY that check's attribute deleted from the source; and
// with EXACTLY the config row that attribute rides removed from a PATCHED
// `DSS_CONFIG_ROOT` tree (the live checkout untouched). MEASURED:
//
//   #  check                              ON   attr deleted        config row removed
//   1  gg()       == 10  (nothrow,leaf)   42   42  ← see below     COMPILE-ERR H000C
//   2  wp()       ==  8  (weak proto)     42   42  ← see below     COMPILE-ERR H000C
//   3  wfun()     == 12  (weak, leading)  42   LINK-FAIL dup sym   COMPILE-ERR H000C
//   4  wd_lead    ==  5  (weak, leading)  42   LINK-FAIL dup sym   COMPILE-ERR H000C
//   5  wd_tail    ==  5  (weak, trailing) 42   LINK-FAIL dup sym   COMPILE-ERR H000C
//   6  vis_lead() ==  3  (vis, leading)   42   42  ← see below     COMPILE-ERR H000C
//   7  vis_tail() ==  4  (vis, trailing)  42   42  ← see below     COMPILE-ERR H000C
//   8  wmerge()   ==  9  (weak on PROTO)  42   LINK-FAIL dup sym   COMPILE-ERR H000C
//
// ★★ ROW 8 IS THE CROSS-DECLARATION CASE
// (D-C-DECLARED-LINKAGE-FACET-NOT-MERGED-ACROSS-A-REDECLARATION) and it is
// EXIT-CODE DISCRIMINATING, which rows 6/7 cannot be. The `weak` sits on
// `wmerge`'s PROTOTYPE and the definition BESIDE it carries no attribute, so the
// binding reaches the emitted symbol only if the compiler folds a declared
// linkage facet across an entity's several declarations. It did not before this
// cycle: BOTH definitions came out strong and DSS refused the link
// (`K_SymbolRedefinedAcrossUnits`) — the same failure the "attr deleted" column
// records, reached without deleting anything. The visibility half of that same
// fold has no runtime witness for the reason stated below, and is pinned
// structurally instead (`ElfHiddenVisibility.
// CalledHiddenFunctionKeepsGlobalBindingOnBothTiers` reads `vis_tail`'s
// `STV_HIDDEN` off this example's own emitted `.symtab`, on both tiers).
//
// Rows 3/4/5 and 8 are EXIT-CODE discriminating in the strongest sense available: the
// sibling CU defines each of those symbols STRONGLY, so a `weak` that is parsed
// but whose binding never reaches the symbol resolves to THIS CU's body and the
// program exits 3/4/5 instead of 42 — and a `weak` deleted outright is a
// duplicate-symbol failure in DSS (`K_SymbolRedefinedAcrossUnits`) and in clang
// (`ld: duplicate symbol`) alike. That is a real strong-over-weak resolution
// being witnessed, not a parse.
//
// Rows 1/2/6/7 are COMPILE-GATE witnesses and this file says so rather than
// pretending otherwise. `__nothrow__`/`__leaf__` are ABI-neutral hints and
// `visibility("hidden")` does not change what a statically linked call returns,
// so DELETING them cannot move an exit code — the honest disable for them is
// disconnecting the config row they ride, which turns each into a loud
// H000C compile error and moves NOTHING else (removing `nothrow`/`leaf` from
// `linkageSpecifierIgnoredNames` breaks only row 1; dropping the `weak` key from
// `linkageSpecifiers` breaks 2/3/4/5; dropping `visibility:hidden` breaks only
// 6/7). Those rows are here because the regression they guard IS a compile
// failure — that is exactly how both audit findings above presented.
//
// ★ WHAT IS DELIBERATELY *NOT* CHECKED HERE. `local_b` is the second declarator
// of `int wd_tail __attribute__((weak)) = 100, local_b = 2;` and it exists to
// witness the PER-DECLARATOR shape: the attribute must attach to `wd_tail` only.
// The half that IS observable — that it attaches to `wd_tail` — is row 5. The
// half that is NOT — that it did NOT leak onto `local_b` — has NO runtime
// witness in a linkable program: a lone definition behaves identically whether
// it is strong or weak, and giving the sibling CU a competing strong `local_b`
// would make the CORRECT behavior a duplicate-symbol error, i.e. an example that
// only passes when the compiler is broken. `local_b` is therefore USED (it
// carries the exit code) but never gated by an `if`. The leak direction is
// pinned at the tier that CAN see it —
// `HirLoweringC.AfterDeclaratorAttributeIsPerDeclaratorNotPerDeclaration`
// (tests/hir/test_hir_lowering_c.cpp) reads the lowered linkage
// side-table directly and asserts `EXPECT_FALSE(bWeak)`, which no running binary
// can. A check that cannot fail is worse than an acknowledged gap.
//
// ★ `deprecated("msg")` IS DECLARED IN BOTH POSITIONS AND NEVER CALLED, on
// purpose. Calling it makes clang emit `-Wdeprecated-declarations`, and this
// corpus requires `-Wall -Wextra` CLEAN. Its only observable in DSS is likewise
// a WARNING (`S_DeprecatedSymbolUsed`), which the runner cannot assert — it
// compares exit codes and stdout. So both positions are witnessed here as
// PARSING and LOWERING with a string argument, and the warning-emission
// semantics belong in a unit pin.
//
// ★★ VALID C, VERIFIED, NOT ASSUMED. `clang -fsyntax-only -Wall -Wextra
// -isysroot $(xcrun --show-sdk-path)` over BOTH CUs: ZERO errors, ZERO warnings;
// and the clang-linked two-CU binary independently EXITS 42, so the expected
// exit code is ground truth from a real toolchain rather than DSS agreeing with
// itself. Re-run both checks if you touch either file.
//
// Front-end feature (attribute → linkage sink) carried through HIR→MIR→link,
// target/format-agnostic, baseline AND the shipped `release` pipeline — the
// optimized arm is mandatory, since the point is that a real optimizer PRESERVES
// weak binding rather than inlining through it (the `weak_inline_crosscu`
// precedent) or DCE-ing a hidden symbol that is still called.

/* The LITERAL glibc prototype idiom. `extern` + an after-declarator attribute
   run: the exact shape that failed H000C before this cycle. */
extern int gg(void) __attribute__((__nothrow__, __leaf__));

/* A `weak` PROTOTYPE with no body here — the definition lives in cu_b.c and is
   resolved at LINK. The shape that failed H0009 before this cycle. */
int wp(void) __attribute__((weak));

/* weak FUNCTION definition, LEADING position. cu_b.c defines `wfun` strongly,
   so strong-over-weak must make the call return 12, never this 100. */
__attribute__((weak)) int wfun(void) { return 100; }

/* weak DATA definition, LEADING position. cu_b.c defines it strongly as 5. */
__attribute__((weak)) int wd_lead = 100;

/* weak DATA, AFTER-DECLARATOR position, and PER-DECLARATOR: `wd_tail` is weak
   (cu_b.c's strong 5 supersedes this 100), `local_b` is NOT and stays 2. */
int wd_tail __attribute__((weak)) = 100, local_b = 2;

/* `deprecated("msg")` in BOTH positions — declared, never called (see header). */
__attribute__((deprecated("superseded"))) int dep_lead(void);
int dep_tail(void) __attribute__((deprecated("superseded")));

/* `visibility("hidden")` in BOTH positions, on functions that ARE called, so a
   visibility sink that wrongly made them DCE-food would fail the link.
   ★ `vis_tail` is ALSO the CROSS-DECLARATION case (row 8's sibling): its
   attribute rides the PROTOTYPE and the definition carries none of its own. It
   has no RUNTIME witness — see the header — and is pinned structurally by
   `ElfHiddenVisibility.CalledHiddenFunctionKeepsGlobalBindingOnBothTiers`,
   which reads `STV_HIDDEN` off THIS example's emitted `.symtab`. */
__attribute__((visibility("hidden"))) int vis_lead(void) { return 3; }
int vis_tail(void) __attribute__((visibility("hidden")));
int vis_tail(void) { return 4; }

/* ★★ THE CROSS-DECLARATION BINDING CASE, AND IT IS EXIT-CODE DISCRIMINATING
   (D-C-DECLARED-LINKAGE-FACET-NOT-MERGED-ACROSS-A-REDECLARATION). The `weak`
   is on the PROTOTYPE and this definition carries no attribute at all, so the
   binding reaches it only if the compiler folds a declared linkage facet across
   an entity's several declarations. cu_b.c defines `wmerge` STRONGLY as 9: fold
   it and strong-over-weak returns 9; drop it and BOTH definitions are strong
   and the LINK fails on a duplicate symbol. MEASURED — that duplicate-symbol
   failure is exactly what DSS produced before the fold landed.
   ⚠ THE CALL RETURNS 9; THE PROGRAM EXITS 42 — the two are different facts and
   an earlier revision of this comment conflated them, claiming the binary
   exited 9. ✔RE-MEASURED 2026-09-05 on Ubuntu 24.04, gcc 13.3.0 and clang
   18.1.3 probed SEPARATELY, `-Wall -Wextra` CLEAN, -O0 AND -O2: every arm
   builds, the linked two-CU binary EXITS 42, and `wmerge` resolves to
   cu_b.c's `FUNC GLOBAL DEFAULT` — which is the strong-over-weak resolution
   this row is here to witness. `main` returns 42 whenever every `if` below
   passes, so 9 could only ever be an early-return code, and none is 9. */
int wmerge(void) __attribute__((weak));
int wmerge(void) { return 100; }

int main(void) {
    if (gg()       != 10) return 1;   /* glibc __nothrow__,__leaf__ idiom      */
    if (wp()       !=  8) return 2;   /* weak prototype, sibling-TU definition */
    if (wfun()     != 12) return 3;   /* weak fn  def, LEADING   -> strong wins */
    if (wd_lead    !=  5) return 4;   /* weak data def, LEADING  -> strong wins */
    if (wd_tail    !=  5) return 5;   /* weak data def, TRAILING -> strong wins */
    if (vis_lead() !=  3) return 6;   /* visibility("hidden"), LEADING          */
    if (vis_tail() !=  4) return 7;   /* visibility("hidden"), TRAILING         */
    if (wmerge()   !=  9) return 8;   /* weak on the PROTOTYPE -> strong wins   */
    return local_b + 40;              /* 42 — the non-weak sibling declarator   */
}
