// P53 (D-C-EXTERN-MUST-LEAD-THE-DECLARATION-SPECIFIERS), CU A — `extern` written
// AFTER the other declaration specifiers, which is what this row is about.
//
// C 6.7.1 makes the declaration specifiers an UNORDERED SET. Until P53 DSS
// accepted `extern inline` / `extern __inline` / `extern _Noreturn` /
// `extern _Thread_local` and REFUSED all five mirror images with
// `error[P_NoAlternativeMatched]`, because `extern` was the HEAD of its own
// file-scope declaration rule. gcc 13.3.0 (`-std=c2x`), clang 18.1.3
// (`-std=c23`) and MSVC 19.51.36252 (`/std:clatest`), probed SEPARATELY on
// 2026-09-02, accept every ordering.
//
// ★★ THIS FILE PINS MEANING, NOT PARSING, AND THAT DISTINCTION IS THE WHOLE
// POINT. A parse-only assertion cannot see the failure this row exists to
// prevent. C99 6.7.4p7 says an `inline` definition WITHOUT `extern` provides no
// external definition, and one WITH `extern` does — so the two orderings must
// not merely both compile, they must both EMIT the body. ✔MEASURED with `nm`,
// gcc 13.3.0 and clang 18.1.3 on this exact shape: `extern inline int p(int)
// {…}` and `inline extern int p(int){…}` each yield `T p`, while the bare
// `inline int p(int){…}` yields `U p` and FAILS TO LINK against a sibling TU
// that calls it. Identical symbol state ⇒ the two orders MEAN the same thing.
//
// ★ AND THE FAILURE IS SILENT WITHOUT THE WEAK FALLBACKS IN CU B. If either
// definition here were wrongly suppressed as a 6.7.4p7 inline definition, the
// program would still compile, still link, and emit ZERO diagnostics — cu_b's
// weak bodies would quietly win. The arithmetic names which half was lost:
// 33 + 2 + 7 = 42 both correct, 10 payload_a lost, 41 payload_b lost, 9 both
// lost. (The `inline_c99_extern_gnu_spellings` 42/3/41/2 pattern, re-based on a
// cross-TU object so the extern OBJECT half is in the same exit code.)
//
// ★★ `sharedCounter` IS THE NON-DEFINING HALF, and it is the one that was a
// SILENT MISCOMPILE in P53's first pass. Merging the two declaration rules makes
// `extern` an ordinary `singleDeclSpecifier`; with the grammar edit ALONE
// `dsscp` reported `warning[H_UnknownLinkageSpecifier]: 'extern' is not a
// recognized linkage specifier` and EXITED 0 — the extern-ness silently dropped,
// so every extern OBJECT declaration became a TENTATIVE DEFINITION emitting
// storage in its own TU. A `{nonDefining:true}` entry on the merged row's
// `linkageSpecifiers` map is what states the fact instead, and binding this
// object across the TU boundary is what proves it: an extern that had become a
// definition would not be reading cu_b's 7.
//
// ★ `die` pins the TWO-LEADING-SPECIFIER form. gcc, clang AND MSVC all accept
// `_Noreturn inline extern void die(int);`, so any repair that admitted only ONE
// specifier before `extern` would still have been below the reference union —
// which is why the fix admits an arbitrary specifier prefix rather than an
// enumerated two-token lead. The call is guarded by a condition that is false at
// run time, so the declaration must still LINK without the branch being taken.

inline   extern int payload_a(void) { return 33; }
__inline extern int payload_b(void) { return 2; }

_Noreturn inline extern void die(int c);

extern int sharedCounter;

int main(void) {
    if (sharedCounter == 999) { die(1); }
    return payload_a() + payload_b() + sharedCounter;
}
