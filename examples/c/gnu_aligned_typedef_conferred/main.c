// GNU `__attribute__((aligned(N)))` ON A TYPEDEF must be ACCEPTED **and CONFERRED**
// (cycle P66, lane `al`). The alias's alignment has to reach three places, and this
// file gates the exit code on all three so a partial fix cannot read as a complete
// one:
//   (1) `_Alignof(A8)`               — the compile-time answer;
//   (2) an OBJECT of the alias type   — static storage AND a frame slot;
//   (3) a STRUCT MEMBER of that type — the composite's own layout.
//
// ★★ THE REFERENCES DO NOT MERELY TOLERATE IT, THEY CONFER IT. ✔MEASURED 2026-09-09,
// each reference probed SEPARATELY on its own translation unit, rc read DIRECTLY and
// never through a pipeline:
//   * gcc 13.3.0 `-std=c17 -Wall -Wextra`: rc 0, stderr EMPTY, and
//     `_Static_assert(_Alignof(A8) == 8)` PASSES. The `== 4` twin FAILS. An EXECUTED
//     binary reports a static and an automatic object of the type both 8-aligned at
//     -O0 and -O2.
//   * clang 18.1.3, same flags: identical on every arm.
//   * mingw-w64 gcc 13.2.0: identical on the arms it was given.
//   * MSVC 19.51.36257 ABSTAINS ON THE SPELLING ONLY — it has no `__attribute__`
//     syntax at all (C2143). Its vote on the CONSTRUCT is cast through the spelling
//     it does implement, and it is the SAME vote: `typedef __declspec(align(8)) int
//     A8;` compiles rc 0 with `int p[(__alignof(A8)==8)?1:-1]`, while the `==4` twin
//     fails C2118. So an over-aligned type ALIAS is conferred by all three
//     references, not two — recorded as a measurement, never as agreement-by-silence.
//
// ★ SIZE IS NOT AFFECTED, AND THAT IS MEASURED TOO, because it is the half a
// "just make the type bigger" fix would get wrong: `sizeof(A8)` stays 4 on gcc and
// clang while `_Alignof(A8)` is 8. Both references consequently REFUSE an ARRAY of
// the alias ("alignment of array elements is greater than element size"), so no
// array of A8 appears below.
//
// ★★ IT IS THE **OPPOSITE** ANSWER FROM THE ISO SPELLING, AND BOTH ARE RIGHT.
// `typedef _Alignas(8) int A8;` is REFUSED by gcc, clang AND MSVC (C7704), so DSS
// refuses it too — [[D-CSUBSET-ALIGNAS-TYPEDEF-PARAM-PARSE]], re-measured this cycle
// and still holding. One construct, two spellings, opposite verdicts: a fix keyed on
// the CONTEXT would have admitted both and put DSS ABOVE the union in the same
// stroke. This file exists on the GNU side of that line only.
//
// RED-ON-DISABLE, and the two halves are DIFFERENT questions that must red
// DIFFERENT pins:
//   * ACCEPTANCE — if the typedef sink goes back to refusing, this file does not
//     COMPILE at all (error[S_AlignasInvalidContext]) and stops exiting 42.
//   * CONFERRAL — if the declaration is admitted but the alignment is dropped before
//     layout, the file still COMPILES and returns 7, 8 or 9 naming which of the three
//     observations failed. That is the silent-wrong-answer direction, and it is the
//     one this file was written to make loud.

typedef unsigned long long uptr;

// ⓘ WHY THERE IS ARITHMETIC IN A FILE ABOUT ALIGNMENT. The `release` arm declares
// `"mustDifferFromBaseline": true`, so the example must give the shipped optimizer
// something to transform — a first draft of this file was pure address checks and
// compile-time assertions, and its optimized image came out BYTE-IDENTICAL to the
// baseline (3072 bytes, same fnv1a), which the runner correctly refuses: an arm that
// compares x to x cannot fail. The helper below is inlinable and the loop is
// foldable, so the two images genuinely differ, while the value they compute is
// still read out of an object of the ALIAS type and so still depends on the
// alignment being conferred.

// The construct under test. Written in the POST-specifier order; the PRE order
// (`typedef __attribute__((aligned(8))) int A8;`) is pinned at the semantic tier by
// SemanticAnalyzerC.GnuAlignedOnATypedefInLeadingPositionConfersToo — both orders are
// accepted and conferred by gcc and clang alike (✔MEASURED).
typedef int __attribute__((aligned(8))) A8;

// (3) the composite half: a member of the alias type must be placed on the alias's
// boundary, which is what makes `c` and `v` sixteen bytes apart rather than eight.
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3: sizeof == 16, _Alignof == 8, offsetof(v)
// == 8, against an undecorated `int v`'s 8 / 4 / 4.
struct S { char c; A8 v; };

static int scaled(A8 v, int k) { return (int)v * k; }

// (2a) static storage.
static A8 g_static;
static struct S g_struct;

int main(void) {
    // (1) the compile-time answer. A `_Static_assert` would refuse the file rather
    // than return a code, so the runtime arm below is what distinguishes
    // "conferred" from "dropped" in the exit status; the static assertion is kept
    // as well because it fails EARLIER and names the defect more precisely.
    _Static_assert(_Alignof(A8) == 8, "the alias must confer 8");
    _Static_assert(sizeof(A8) == 4, "the alias must NOT change the size");
    _Static_assert(sizeof(struct S) == 16, "the member must be placed at 8");

    // (2b) an automatic object. `&l` forces a real frame slot, which is the only way
    // a local's alignment is observable at all.
    A8 l;
    if (((uptr)(void *)&g_static % 8u) != 0u) return 7;   // static storage dropped it
    if (((uptr)(void *)&l % 8u) != 0u) return 8;          // the frame slot dropped it
    if (((uptr)(void *)&g_struct.v % 8u) != 0u) return 9; // the member dropped it

    // The optimizer's work, and it is not decoration: `l` and `g_struct.v` are
    // objects of the ALIAS type, so the accumulator below is loaded THROUGH the
    // alignment this file is about. The loop and the inlinable `scaled` are what
    // make the release image differ from the baseline.
    l = 3;
    g_struct.v = 4;
    int acc = 0;
    for (int i = 0; i < 6; ++i) acc += scaled(l, i) - scaled(g_struct.v, i);
    // acc = sum over i<6 of (3i - 4i) = -sum(i) = -15, so 42 = 27 - (-15).
    if (acc != -15) return 10;   // the alias-typed loads did not read what was stored
    return 27 - acc;
}
