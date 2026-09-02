// P51 (D-CSUBSET-INLINE-FUNCTION-SPECIFIER), CU A — the FOURTH baseline
// spelling that row names, `extern __inline`, and the one the TF-C79 corpus
// left unpinned.
//
// That row's closing witness is "the 4 baseline spellings compile AND the
// emitted symbol's LINKAGE is asserted for each": `inline`, `static inline`,
// `extern inline`, `extern __inline`. The first three are witnessed by
// inline_c99_inline_definition_crosscu and inline_c99_storage_classes.
// ✔MEASURED P51 by grep over the tree at this cycle's base commit: the fourth —
// `extern` TOGETHER WITH a GNU spelling — appeared in no example, no test and no
// fixture; its only occurrences anywhere were two prose `$comment`s in
// c.lang.json.
//
// ★ IT IS NOT A CURIOSITY — IT IS THE SPELLING THE REAL HEADER SURFACE USES FOR
// THIS EXACT LINKAGE. glibc writes `extern __inline __attribute__((__gnu_inline__))`
// as `__extern_inline`, and Apple's <sys/cdefs.h> writes the same two spellings
// in arms 2 and 3 of its `__header_inline` ladder. ✔MEASURED via
// `dsscp --dump-predefined-macros` on all four legs: DSS predefines
// `__STDC_VERSION__ = 202311L`, `__GNUC__ = 4` and `__clang__ = 1`, and defines
// neither `__GNUC_GNU_INLINE__` nor `__GNUC_STDC_INLINE__` — so Apple's arm 1
// (guarded `__STDC_VERSION__ >= 199901L && !defined(__GNUC_GNU_INLINE__) &&
// (!defined(__GNUC__) || defined(__clang__))`) is the arm DSS takes today, and
// this file pins the arms a different identity claim would select. That is the
// point of pinning it now rather than when a header trips over it.
//
// ★★ WHY THIS IS NOT A SPELLING TEST. `extern` on an inline definition is what
// C99 6.7.4p7 exempts from the "does not provide an external definition" rule,
// so the question is whether the `__inline` / `__inline__` spellings reach the
// SAME without-extern scan the C99 keyword does — two independent lookups (the
// shared token kind, and `semantics.inline.externSpecifierTokens`). If the scan
// resolves the keyword but tests for `extern` on a narrower footing, these
// definitions stop being emitted — and the program still compiles and still
// links, because cu_b.c's WEAK fallbacks silently win. The exit code slides
// 42 → 2 (both lost), 3 (payload_a lost) or 41 (payload_b lost) with ZERO
// diagnostics: the same silent-halfway shape
// inline_c99_extern_decl_promotes_crosscu exists for, reached through the other
// half of 6.7.4p7.
//
// ✔MEASURED on gcc 13.3.0 and clang 18.1.3, this exact file, `-std=c99` and
// `-std=gnu17`, `-O0` and `-O2`: `nm cu_a.o` shows `T payload_a` and
// `T payload_b` — strong, defined — in every one of the eight combinations, and
// the linked pair exits 42. mingw-w64 gcc 13.2.0 agrees on the `extern __inline`
// half.
//
// ⚠ MSVC 19.51 is NOT a reference for the `__inline__` half: it accepts
// `extern __inline` (its own spelling) and REFUSES `extern __inline__` with
// C2054. gcc and clang accept it, so the disjunction requires DSS to.

extern __inline   int payload_a(void) { return 40; }
extern __inline__ int payload_b(void) { return 2; }

int main(void) { return payload_a() + payload_b(); }
