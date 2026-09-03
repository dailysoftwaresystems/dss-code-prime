/* D-PP-SYNTHBUILDER-PREDEFINED-DEFINEDNESS runtime witness.
 *
 * ONE question -- "is this PREDEFINED macro defined?" -- asked in eight
 * spellings, each gating a quote-`#include`. The include-gating PRE-SCAN
 * (SynthBuilder) decides whether to splice the header long before the
 * authoritative pass runs, and it answers out of its own oracle
 * (`sbNameDefined`). Two spellings of one question giving different answers is
 * a SILENT WRONG BRANCH, not a refusal: the file still compiles and a header
 * simply takes the arm meant for another platform.
 *
 * PART A -- the row's own claim, on `__GNUC__` (an ordinary object-like
 * language predefine, present on EVERY target this example runs on):
 *   #ifdef / #ifndef / #elifdef / #elifndef  must each agree with
 *   #if defined() / #if !defined() / #elif defined() / #elif !defined().
 * The bare-name arm and its `defined()` twin sit next to each other on purpose
 * -- a fixture exercising only one of the two paths cannot see a divergence
 * between them, and a green there would mean nothing.
 *
 * PART B -- `#undef` composition, on `__declspec`. That is the one FUNCTION-LIKE
 * predefine c.lang.json declares, and `sbNameDefined` answers for it out of a
 * walk over the effective predefined list -- a CONFIG list no directive can
 * subtract from -- while the authoritative pass holds it in its ordinary macro
 * table, where `#undef` DOES erase it. The tolerated skew is pre-scan MORE live;
 * `#ifdef` inverts to `#ifndef`, so an over-answering oracle reads the three
 * NEGATIVE spellings DEAD where the real pass reads them LIVE -- the
 * silently-dropped-header direction. Before the fix that was a hard
 * `P_PreprocessorIncludeError` (the authoritative pass refuses rather than drop);
 * mingw-w64 gcc 13.2.0, WSL gcc 13.3.0 and clang 18.1.3 all compile and RUN the
 * same shape (MEASURED 2026-09-03, each probed separately).
 *   `__declspec` is availableObjectFormats:[pe], so the pe leg is where PART B
 *   exercises the `#undef`-of-a-live-predefine path. On elf/macho the name is
 *   absent to begin with, so the SAME guards must reach the SAME verdicts by the
 *   other route -- which is itself worth pinning (a `#undef` of a name that was
 *   never defined is ignored, C 6.10.5.2p2) and keeps ONE exit code on all four
 *   targets.
 *
 * SCORING. Every arm contributes 1 when it matches the expected verdict and 0
 * otherwise, so the arms cannot cancel each other out: the sum is 14 iff ALL
 * FOURTEEN are right. 14 * 3 = 42, and the sum reaches `identity` as a FUNCTION
 * ARGUMENT so the baseline arm keeps a live runtime value rather than folding
 * the exit to one immediate. The `release` arm runs the shipped pipeline over
 * the same source.
 *
 * ⚠ WHAT THE REFERENCES DO AND DO NOT SAY ABOUT THIS FILE'S EXIT CODE.
 * ✔MEASURED 2026-09-03, each probed separately, on THIS source: mingw-w64 gcc
 * 13.2.0, WSL gcc 13.3.0 and clang 18.1.3 all exit 42. MSVC 19.51.36252
 * (/std:clatest /Zc:preprocessor) compiles it cleanly and exits 18 — and that is
 * NOT a preprocessor disagreement. `__GNUC__` is a gcc/clang-family identity
 * macro that c.lang.json declares and MSVC does not, so every PART A arm scores
 * 0 there; PART B agrees with MSVC arm for arm (all six), 6 * 3 = 18, which
 * accounts for the whole difference. MSVC was probed directly and DOES support
 * `#elifdef`. PART A is therefore a pin on DSS's own configured predefine set,
 * not a portable claim about every reference's.
 */

/* ══ PART A ══ `__GNUC__` IS defined on every target here. ═════════════════ */

#undef SPLICED
#ifdef __GNUC__
#include "marker.h"
#endif
#ifdef SPLICED
#define A_IFDEF 1
#else
#define A_IFDEF 0
#endif

#undef SPLICED
#if defined(__GNUC__)
#include "marker.h"
#endif
#ifdef SPLICED
#define A_DEFINED 1
#else
#define A_DEFINED 0
#endif

#undef SPLICED
#ifndef __GNUC__
#include "marker.h"
#endif
#ifdef SPLICED
#define A_IFNDEF 0
#else
#define A_IFNDEF 1
#endif

#undef SPLICED
#if !defined(__GNUC__)
#include "marker.h"
#endif
#ifdef SPLICED
#define A_NOTDEFINED 0
#else
#define A_NOTDEFINED 1
#endif

#undef SPLICED
#if 0
#elifdef __GNUC__
#include "marker.h"
#endif
#ifdef SPLICED
#define A_ELIFDEF 1
#else
#define A_ELIFDEF 0
#endif

#undef SPLICED
#if 0
#elif defined(__GNUC__)
#include "marker.h"
#endif
#ifdef SPLICED
#define A_ELIFDEFINED 1
#else
#define A_ELIFDEFINED 0
#endif

#undef SPLICED
#if 0
#elifndef __GNUC__
#include "marker.h"
#endif
#ifdef SPLICED
#define A_ELIFNDEF 0
#else
#define A_ELIFNDEF 1
#endif

#undef SPLICED
#if 0
#elif !defined(__GNUC__)
#include "marker.h"
#endif
#ifdef SPLICED
#define A_ELIFNOTDEFINED 0
#else
#define A_ELIFNOTDEFINED 1
#endif

/* ══ PART B ══ `#undef` must compose in the PRE-SCAN, not only in the
 * authoritative pass. After this line `__declspec` is undefined on EVERY
 * target -- erased on pe, never present elsewhere. ════════════════════════ */

#undef __declspec

#undef SPLICED
#ifndef __declspec
#include "marker.h"
#endif
#ifdef SPLICED
#define B_IFNDEF 1
#else
#define B_IFNDEF 0
#endif

#undef SPLICED
#if !defined(__declspec)
#include "marker.h"
#endif
#ifdef SPLICED
#define B_NOTDEFINED 1
#else
#define B_NOTDEFINED 0
#endif

#undef SPLICED
#if 0
#elifndef __declspec
#include "marker.h"
#endif
#ifdef SPLICED
#define B_ELIFNDEF 1
#else
#define B_ELIFNDEF 0
#endif

#undef SPLICED
#ifdef __declspec
#include "marker.h"
#endif
#ifdef SPLICED
#define B_IFDEF 0
#else
#define B_IFDEF 1
#endif

#undef SPLICED
#if defined(__declspec)
#include "marker.h"
#endif
#ifdef SPLICED
#define B_DEFINED 0
#else
#define B_DEFINED 1
#endif

#undef SPLICED
#if 0
#elifdef __declspec
#include "marker.h"
#endif
#ifdef SPLICED
#define B_ELIFDEF 0
#else
#define B_ELIFDEF 1
#endif

int identity(int v) { return v; }

int main(void) {
    int partA = A_IFDEF + A_DEFINED + A_IFNDEF + A_NOTDEFINED
              + A_ELIFDEF + A_ELIFDEFINED + A_ELIFNDEF + A_ELIFNOTDEFINED;
    int partB = B_IFNDEF + B_NOTDEFINED + B_ELIFNDEF
              + B_IFDEF + B_DEFINED + B_ELIFDEF;
    /* 8 + 6 == 14 arms, every one of them correct; 14 * 3 == 42. */
    return identity((partA + partB) * 3);
}
