// D-CSUBSET-ATTRIBUTE-LEADING-WITH-STORAGE-CLASS (TF-C77) witness, CU A —
// MODE 2: a GNU attribute in the MID position, between the type head and the
// declarator list. This is the `declAttrRun` slot.
//
//     static int __attribute__((cold)) sf(int x) { … }   /* the sqlite shape */
//     int __attribute__((weak)) wd = 100;
//     int __attribute__((aligned(64))) av = 7;
//
// The function-definition form is the sqlite MODE-2 spelling —
// `static void SQLITE_NOINLINE walIndexPage(…)`, 20 of its 119 SQLITE_NOINLINE
// sites (16 × `static T SQLITE_NOINLINE f(` + 4 × `T SQLITE_NOINLINE f(`).
//
// ★ STATE THE CENSUS SCOPE WITH THE NUMBER, because quoting a bare count is what
// invites a false disagreement. 119 counts sqlite's `.c` files ONLY, and splits
// 90/9/16/4 = mode 3 (`static SQLITE_NOINLINE T f(`) / leading / the two mode-2
// shapes above; including the 3 `.h` prototype sites gives 122 = 90/10/16/6,
// i.e. 22 in mode 2. The figure 99 is NOT this slot's — it is MODE 3 (90) plus
// the 9 leading sites. Mode 3 is the dominant sqlite position; MODE 2 is the one
// this example demonstrates, and the two figures must not be swapped again.
//
// MEASURED at the pre-change HEAD, every line above was a hard parse
// error:
//     error[P0009] expected 'Identifier', 'ParenOpen', 'StarOp', 'EndStatement',
//                  'BracketOpen' or 'BlockOpen' — got '__attribute__'
//
// ★★ THE GRAMMAR AND THE ENGINE CHANGE ARE ONE UNIT, and this file is the
// witness for both halves independently. The slot is a SIBLING of the type head,
// never a child of it (a child would let `resolveTypeNodeImpl`'s
// first-child-that-resolves-wins token arm try the attribute identifier as a
// TYPE — the silent hijack `D-CSUBSET-TYPEDEF-HEAD-DECORATION-TYPE-HIJACK`
// exists to prevent). Two separate keys carry it to two separate sinks:
//   • `declarationAttrSlotRules` → the SEMANTIC attribute scan  → `aligned`
//   • `linkagePrefixRoots`'s slot roots → the HIR LINKAGE fold  → `weak`
// Either one missing is a SILENT drop, and the two break DIFFERENT checks, which
// is why both are witnessed here rather than trusting one to cover the other.
//
// ★★ NON-VACUITY, PROVEN PER CHECK, AGAINST THE FINAL BUILD. Each check isolated
// into its own program and built four ways — as written; with
// `declarationAttrSlotRules` removed from the topLevelDecl row (H1); with
// `linkagePrefixRoots` reverted to prefix-only (H2); and with the row indices
// left at their pre-TF-C77 values (H3). MEASURED:
//
//   #  check                       ON   H1 slot-off      H2 engine-off    H3 stale idx
//   1  &av % 64 == 0  (aligned)    42   1  SILENT        42               COMPILE-ERR
//   2  wd == 5        (weak)       42   LINK dup sym     LINK dup sym     COMPILE-ERR
//   3  sf(41) == 42   (sqlite fn)  42   42               42               COMPILE-ERR
//
// Read the columns, not just the reds. H1 and H2 break DIFFERENT rows — that is
// the point of running both: they are genuinely independent halves, and a single
// whole-file revert would have proven only that "something" was needed. Row 1
// under H1 is the important cell: the program still COMPILES AND LINKS with ZERO
// diagnostics of any code and simply returns the wrong number. Row 3 is a
// COMPILE-gate witness (`cold` is an ABI-neutral hint with no runtime effect) and
// this file says so rather than pretending otherwise; it is here because the
// regression it guards IS a compile failure — that is exactly how the P0009
// above presented, and it is the position sqlite actually uses.
//
// ★★ THE OFFSET BREAKER `data_pad` IS LOAD-BEARING. DO NOT DELETE IT, AND DO NOT
// ADD AN OVER-ALIGNED GLOBAL WITHOUT ONE. The first over-aligned global in a
// section lands at the section start, and every object format page-aligns those
// — so `&av % 64 == 0` would hold whether or not the sink ran. It must be the
// SAME SECTION CLASS as the object it precedes: this was MEASURED the hard way
// during authoring, where an UNINITIALIZED `char pad;` (.bss) in front of an
// INITIALIZED `av` (.data) left check 1 returning 42 under H1, i.e. VACUOUS. An
// initialized `data_pad = 1` (.data, like `av`) makes the natural placement
// differ from the aligned placement, and RE-MEASURED the check then reads
// 42 → 1 under H1. A check that cannot fail is worse than no check.
//
// ★ `wd` IS A DECLARATION-LEVEL BINDING, NOT A PER-DECLARATOR ONE, and that is C
// semantics rather than a convention: the attribute sits in the DECL-SPECIFIER
// region, so it applies to EVERY declarator. GROUND TRUTH IS REAL CLANG —
// MEASURED with `clang -c` + `nm -m`, `int __attribute__((weak)) ma = 1, mb = 2;`
// emits BOTH as "weak external". Contrast the AFTER-DECLARATOR run, where only
// the declarator it follows is weak (pinned in
// examples/c/gnu_attribute_linkage_positions_crosscu/). Two positions,
// two different scopes of application, both correct.
//
// ★★ VALID C, VERIFIED, NOT ASSUMED. `clang -fsyntax-only -Wall -Wextra
// -isysroot $(xcrun --show-sdk-path)` over BOTH CUs: ZERO errors, ZERO warnings;
// and the clang-linked two-CU binary independently EXITS 42, so the expected
// exit code is ground truth from a real toolchain rather than DSS agreeing with
// itself. Re-run both checks if you touch either file.
//
// Front-end feature (attribute position → alignment + linkage sinks) carried
// through HIR→MIR→link, target/format-agnostic, baseline AND the shipped
// `release` pipeline — the optimized arm is mandatory here, since the point is
// that a real optimizer PRESERVES both the alignment and the weak binding rather
// than re-laying-out the globals or inlining through the weak symbol.

char data_pad = 1;                          /* offset breaker — SAME section class as `av` */

/* MODE 2, the ALIGNMENT sink. 64 is a genuine over-alignment for `int`
   (natural 4), so a sink that fell back to natural alignment fails the mask. */
int __attribute__((aligned(64))) av = 7;

/* MODE 2, the LINKAGE sink. cu_b.c defines `wd` strongly as 5, so
   strong-over-weak must make this read 5, never this 100. */
int __attribute__((weak)) wd = 100;

/* MODE 2 on a FUNCTION DEFINITION — the literal sqlite shape, and the form that
   moves the row's `kindByChild` index from [2,0] to [3,0]. If that index is left
   behind, this definition is mis-lowered as a variable and the file does not
   compile at all. */
static int __attribute__((cold)) sf(int x) { return x + 1; }

/* A DECLARATOR-LESS declaration in the same TU — the index shape where the
   optional init-declarator list is ABSENT, so the role children are one shorter.
   This is exactly where a hand-moved index goes wrong, so it rides along. */
struct P { int x; };

int main(void) {
    struct P p;
    p.x = 41;
    if (((unsigned long long)(&av) & 63ull) != 0ull) return 1;  /* aligned(64) */
    if (wd != 5)                                     return 2;  /* weak -> strong wins */
    if (sf(p.x) != 42)                               return 3;  /* sqlite-shaped fn def */
    return av + 35;   /* 42 — flows THROUGH the over-aligned global */
}
