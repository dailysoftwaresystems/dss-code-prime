// D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS (TF-C77) witness, CU A —
// MODE 1: a GNU attribute written AFTER the `extern` keyword and BEFORE the type.
//
//     extern __attribute__((__noreturn__)) void die(int);
//     extern __attribute__((weak)) int wfun(void) { return 100; }
//
// This is the Tcl/glibc after-keyword spelling (`EXTERN TCL_NORETURN void
// Tcl_Panic(...)`). MEASURED at the pre-change HEAD it did not compile at all:
//
//     extern __attribute__((__noreturn__)) void die(int);
//         error[P0009] expected 'Identifier', 'VoidKeyword', 'IntKeyword', …
//                      — got '__attribute__'
//
// ★ WHY THE GRAMMAR PUTS IT *INSIDE* THE HEAD WRAPPER AND NOT BEFORE `extern`.
// `externSpecifiers` (which owns the keyword) grew the `attrSpec` alt, so
// FIRST(externDecl) stays {ExternKeyword}. Moving the decoration ahead of the
// keyword would put AttributeKeyword into FIRST(externDecl), colliding with
// `topLevelDecl`'s specifier prefix. That is not a theory: MEASURED against a
// throwaway patched config tree, the loader REFUSES it —
//     error[C_AmbiguousAlternatives] at /shapes/topLevel:
//         alt branches share FIRST token 'ExternKeyword'
//     error[D_SchemaLoadFailed] …
// at LOAD time, exit 1. The wall is real and loud, so the fully-leading order is
// its own anchor rather than a variation of this one.
//
// ★★ EVERY CHECK IS AN APPLIED FACT, PROVEN NON-VACUOUS. Isolated per check and
// built three ways — as written; with THAT check's attribute deleted; and with
// the config row it rides removed from a patched `DSS_CONFIG_ROOT` tree (the
// live checkout untouched). MEASURED:
//
//   #  check                          ON   attr deleted          config row off
//   1  wfun()  == 12  (weak)          42   LINK-FAIL dup sym     COMPILE-ERR H000C
//   2  ev      ==  7  (visibility)    42   42  ← see below       COMPILE-ERR H000C
//   3  gg()    == 10  (nothrow,leaf)  42   42  ← see below       COMPILE-ERR H000C
//   4  pick(0) reached  (noreturn)    42   COMPILE-ERR H0003     COMPILE-ERR H0003
//
// Row 1 is exit-code discriminating in the strongest sense: cu_b.c defines `wfun`
// STRONGLY, so a `weak` that parses but never reaches the symbol resolves to
// THIS CU's body and the program exits 1 instead of 42 — and a `weak` deleted
// outright is a duplicate-symbol failure in DSS (K_SymbolRedefinedAcrossUnits)
// and in clang (`ld: duplicate symbol`) alike.
//
// ★ ROW 4 IS THE ONE THIS EXAMPLE EXISTS FOR, and it is a COMPILE gate on
// purpose, because that is where `noreturn` is observable at all. `pick` is
// non-void and its last path ends in `die(...)` with NO return. If the
// after-keyword `__noreturn__` is honored, that path structurally terminates and
// the program builds; if the attribute is parsed and its meaning dropped, HIR
// verification refuses the function —
//     error[H0003] non-void function #N may fall through without returning a value
// MEASURED, both directions, on this exact program: with the attribute → exit 0
// and a running binary; with `extern void die(int);` instead → H0003, no binary.
// A runtime check could not witness this: the failure mode is a REFUSAL, not a
// wrong number.
//
// Rows 2 and 3 are COMPILE-GATE witnesses and this file says so rather than
// pretending otherwise: `__nothrow__`/`__leaf__` are ABI-neutral hints and
// `visibility("hidden")` does not change what a statically linked read returns,
// so deleting them cannot move an exit code. Their honest disable is
// disconnecting the config row they ride, which turns each into a loud H000C.
// They are here because the regression they guard IS a compile failure — that is
// exactly how the P0009 above presented.
//
// ★ `_Thread_local` × attribute IN BOTH ORDERS is declared and read, because the
// slot is a REPEAT over an alt and the linkage merge is last-wins — an
// order-sensitive fold is a real hazard. Both spellings must give the same answer.
//
// ★★ VALID C, VERIFIED, NOT ASSUMED. `clang -fsyntax-only -Wall -Wextra
// -isysroot $(xcrun --show-sdk-path)` over BOTH CUs: ZERO errors, ZERO warnings;
// and the clang-linked two-CU binary independently EXITS 42, so the expected
// exit code is ground truth from a real toolchain rather than DSS agreeing with
// itself. Re-run both checks if you touch either file.
//
// Front-end feature (attribute position → linkage + noreturn sinks) carried
// through HIR→MIR→link, target/format-agnostic, baseline AND the shipped
// `release` pipeline — the optimized arm is mandatory here, since the point is
// that a real optimizer PRESERVES weak binding rather than inlining through it.

/* MODE 1, `noreturn`. cu_b.c defines `die` as a real non-returning function. */
extern __attribute__((__noreturn__)) void die(int);

/* MODE 1, `weak` on an extern FUNCTION DEFINITION (C 6.9.1 — the Tcl
   `EXTERN int Sqlite3_Init(...){...}` shape). cu_b.c defines `wfun` strongly,
   so strong-over-weak must make the call return 12, never this 100. */
extern __attribute__((weak)) int wfun(void) { return 100; }

/* MODE 1, the COMPOSITE `visibility("hidden")` key — the attribute's string
   argument has to survive the scan as a token for the `<name>:<body>` pairing. */
extern __attribute__((visibility("hidden"))) int ev;

/* MODE 1, MULTI-CLAUSE — the literal glibc `__THROW` expansion. */
extern __attribute__((__nothrow__, __leaf__)) int gg(void);

/* MODE 1 × thread storage, BOTH ORDERS. */
extern _Thread_local __attribute__((visibility("hidden"))) int t_first;
extern __attribute__((visibility("hidden"))) _Thread_local int t_second;

/* Non-void, and its fall-through path ends in a call to the noreturn `die`.
   This function COMPILES ONLY IF the after-keyword `__noreturn__` was honored. */
int pick(int c) {
    if (c) return wfun();
    die(2);
}

int main(void) {
    if (gg()      != 10) return 3;   /* glibc __nothrow__,__leaf__ idiom      */
    if (ev        !=  7) return 2;   /* visibility("hidden"), after-keyword   */
    if (t_first   !=  5) return 5;   /* thread_local AFTER the attribute      */
    if (t_second  !=  6) return 6;   /* thread_local BEFORE the attribute     */
    if (pick(1)   != 12) return 1;   /* weak extern fn def -> strong wins     */
    return t_first + t_second + 31;  /* 42 */
}
