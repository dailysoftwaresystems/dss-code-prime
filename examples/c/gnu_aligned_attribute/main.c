// D-CSUBSET-GNU-ATTRIBUTE (TF-C73) witness: GNU `__attribute__((aligned(N)))`
// is HONORED — the alignment is APPLIED, not merely parsed — and it is applied
// IDENTICALLY in every declaration position that can carry it. This is the
// runtime half of the positional-symmetry pin
// `AfterDeclaratorAlignedAppliesLikeTheLeadingPosition`
// (tests/hir/test_hir_lowering_c.cpp): that test asserts the APPLIED
// AlignmentAttr on the lowered node, this file asserts the ADDRESS the loaded
// program actually observes, on every target.
//
// ★ WHY THIS EXAMPLE EXISTS AT ALL — it is the discharge of a pinned promise.
// Until TF-C73 `aligned` was deliberately FAIL-LOUD: DSS sourced alignment only
// from `alignasSpec`, so silencing the attribute would have produced a SILENTLY
// UNDER-ALIGNED object — a miscompile, strictly worse than the noise. The
// corpus example `examples/c/gnu_attribute_after_declarator/` carried
// `int v __attribute__((aligned(4))) = 20;` for exactly that reason and had to
// drop it, recording that the case "comes back when the real `aligned` sink
// lands, asserting the ALIGNMENT, not merely the parse". This file IS that
// return, address-checked rather than parse-checked.
//
// ★★ THE OFFSET BREAKERS (`bss_pad`, `data_pad`) ARE LOAD-BEARING. DO NOT
// DELETE THEM, AND DO NOT ADD AN OVER-ALIGNED GLOBAL WITHOUT ONE.
//
// An earlier revision of this file had NO breakers, and an audit measured the
// consequence: three of its five checks COULD NOT FAIL. The first over-aligned
// global in a translation unit lands at the START of its section, and every
// object format page-aligns section starts — so `&g % 32 == 0` and `&h % 64 == 0`
// held whether or not the alignment sink ran at all. MEASURED, each check
// isolated into its own program and built twice (sink ON, and with the `aligned`
// effect row demoted `align`→`none` under a patched `DSS_CONFIG_ROOT`):
//
//     check                         sink ON   demoted   verdict
//     &g  % 32   (leading)             42        42     ★ VACUOUS
//     &h  % 64   (after-declarator)    42        42     ★ VACUOUS
//     &k  %  8   (typedef)             42        42     ★ VACUOUS
//     sizeof(struct M) == 32           42         4     discriminating
//     &m.v % 16                        42         5     discriminating
//
// The whole-file 42→1 that the old file reported as its red-on-disable came
// ENTIRELY from the leading check firing once the other globals shifted around
// it — an accident of ordering, not a property of the check. The position this
// example exists to prove, AFTER-DECLARATOR, had no working witness at all: an
// asymmetric regression that broke only that sink would have passed green.
//
// A one-byte global of the SAME section class placed immediately before each
// over-aligned object fixes it, because the natural placement then is NOT the
// aligned placement. `bss_pad` (uninitialized → .bss) precedes `g`; `data_pad`
// (initialized → .data) precedes `h`. RE-MEASURED with the breakers in place:
//
//     &g % 32   sink ON 42 → demoted 1      discriminating
//     &h % 64   sink ON 42 → demoted 2      discriminating
//
// ★ THE TYPEDEF POSITION HAS NO RUNTIME CHECK HERE, DELIBERATELY, AND THAT IS A
// CORRECTION OF A PREVIOUS CLAIM. The old file carried
// `typedef unsigned long long u8_t __attribute__((aligned(8))); u8_t k;` with a
// `&k % 8` check, disclosed in-file as "a proven no-op". The disclosure was
// honest but the check still did not belong: `unsigned long long` is naturally
// 8-aligned on all four targets, so `aligned(8)` asks for exactly what the
// alias already has and NO arrangement of padding can make that check
// discriminate the sink — it is vacuous by construction, not by placement. A
// witness file whose header claims "every check below fails the run if the
// alignment is dropped" cannot contain one. The typedef position is covered
// where its behavior is actually observable: at COMPILE time, by the fail-loud
// refusal of an alignment it cannot honor —
//     typedef unsigned long long T __attribute__((aligned(32)));
//     error[S002F] ... on a typedef cannot be honored: the alias resolves to
//                  the same type as its aliasee, whose alignment is 8
// (MEASURED) — plus the parse/lowering pins. When typedef OVER-alignment is
// genuinely honored, a real address check belongs here; until then an honest
// absence beats a check that cannot fail.
//
// ★ THE ASSERTION IS AN ADDRESS OR A `sizeof`, NEVER A DIAGNOSTIC COUNT. The
// failure mode of an alignment sink is a WRONG APPLIED VALUE that emits nothing
// at all, so a count-based or compiles-clean witness stays green through the
// exact regression this guards.
//
// ★★ EVERY DECLARATION BELOW IS VALID C THAT REAL CLANG ACCEPTS, verified with
// `clang -fsyntax-only -Wall -Wextra -isysroot $(xcrun --show-sdk-path)`: ZERO
// errors, ZERO warnings; and the clang-built binary independently EXITS 42, so
// the expected exit code is ground truth from a real toolchain rather than from
// DSS agreeing with itself. This is a hard requirement, not a nicety: an invalid
// `format` attribute shipped in this very corpus area for a full cycle and made
// its example prove the OPPOSITE of what it existed to prove. Re-run both checks
// if you touch this file.
//
// The five positions, all in ONE translation unit so the symmetry is witnessed
// by a single run rather than inferred across files:
//   * LEADING            `__attribute__((aligned(32))) int g;`  → &g % 32 == 0
//   * AFTER-DECLARATOR   `int h __attribute__((aligned(64))) = 7;` → &h % 64
//   * COMPOSITE, AFTER-KEYWORD `struct __attribute__((aligned(32))) CK {…};`
//                        → sizeof == 32 AND &ck % 32 == 0
//   * MEMBER             `int v __attribute__((aligned(16)));`
//                        → sizeof(struct M) == 32 AND &m.v % 16 == 0
//   * COMPOSITE, TRAILING `struct CT {…} __attribute__((aligned(32)));`
//                        → sizeof == 32 AND &ct % 32 == 0
//
// The two COMPOSITE rows are new this cycle and close the gap the previous
// revision recorded as "NOT COVERED, because the surface does not exist yet"
// (it was S0031, fail-loud).
//
// ★★ THE NON-VACUITY PROOF FOR THE FILE AS IT NOW STANDS. Every check isolated
// into its OWN program and built three times — sink ON; the `aligned` effects
// row demoted `align`→`none`; and `declarationAttrSlotRules` removed from the
// structSpec row (which disconnects EXACTLY the composite lead surface while
// leaving the program compilable). A check that returns 42 in all three columns
// cannot fail and does not belong. None does:
//
//   #  position                     clang  ON   align→none   slot-off
//   1  &g % 32   leading            42     42   1            42
//   2  &h % 64   after-declarator   42     42   2            42
//   3  sizeof CK composite-lead     42     42   COMPILE-ERR  3
//   4  &ck % 32  composite-lead     42     42   COMPILE-ERR  4
//   5  sizeof M  member             42     42   5            42
//   6  &m.v % 16 member             42     42   6            42
//   7  sizeof CT composite-trail    42     42   COMPILE-ERR  42
//   8  &ct % 32  composite-trail    42     42   COMPILE-ERR  42
//
// Read the columns, not just the reds. `slot-off` moves ONLY rows 3 and 4,
// which is the point of that switch: it isolates the one channel this cycle
// added, and rows 7/8 staying 42 under it is CORRECT — the trailing list is a
// different surface. Rows 3/4/7/8 go COMPILE-ERR rather than wrong-number under
// the demotion because the composite scan REFUSES an attribute it cannot honor
// instead of dropping it. That has a consequence worth stating plainly: under
// that demotion the composite rows abort the compile, so the runtime checks
// never run — which is exactly why this proof isolates each check into its own
// program instead of reading one whole-file exit code. A whole-file red is not
// evidence about any particular check.
//
// ★ THE MEMBER ROW CHECKS BOTH `sizeof` AND THE ADDRESS, on purpose. The address
// alone would pass on a layout that got `v`'s offset right by accident; the
// `sizeof` alone would pass on a layout that padded the tail correctly but put
// `v` in the wrong place. Together they pin the whole layout: `char c` at 0, `v`
// forced to offset 16, tail padded to the composite's raised 16-byte alignment
// ⇒ 32. Real clang produces exactly this layout (same program, exit 42), so the
// numbers are a toolchain's answer and not DSS's own. The same reasoning is why
// each COMPOSITE row checks `sizeof` as well as its address: a `sizeof` is a
// pure layout fact that no section placement can satisfy by accident.
//
// The `h + 35` return keeps the exit code load-bearing on a value that flows
// THROUGH an over-aligned initialized global, so a sink that silently relocated
// or mis-initialized `h` is caught by the exit code too, not only by the mask.
//
// The exit codes are per-position on purpose — 1 = leading `g`, 2 = trailing
// `h`, 3/4 = composite after-keyword (sizeof/address), 5/6 = member layout
// (sizeof/address), 7/8 = composite trailing (sizeof/address) — so whichever
// arm regresses names itself in the exit status instead of collapsing into one
// anonymous failure.
//
// Front-end feature (attribute → alignment sink) carried through HIR→MIR→asm,
// target/format-agnostic: x86_64 (PE + ELF) and arm64 (ELF under qemu, Mach-O
// macos leg), baseline AND the shipped `release` pipeline — the optimized arm is
// mandatory here, since the point is that a real optimizer PRESERVES alignment.

char bss_pad;                                                  /* offset breaker (.bss)  */
__attribute__((aligned(32))) int g;                            /* leading */
char data_pad = 1;                                             /* offset breaker (.data) */
int h __attribute__((aligned(64))) = 7;                        /* after-declarator */
struct __attribute__((aligned(32))) CK { int x; };             /* composite, after-keyword */
struct CK ck;
struct M { char c; int v __attribute__((aligned(16))); };      /* member */
struct M m;
struct CT { int x; } __attribute__((aligned(32)));             /* composite, trailing */
struct CT ct;

int main(void) {
    if (((unsigned long long)(&g) & 31ull) != 0ull) return 1;
    if (((unsigned long long)(&h) & 63ull) != 0ull) return 2;
    if (sizeof(struct CK) != 32u) return 3;
    if (((unsigned long long)(&ck) & 31ull) != 0ull) return 4;
    if (sizeof(struct M) != 32u) return 5;
    if (((unsigned long long)(&m.v) & 15ull) != 0ull) return 6;
    if (sizeof(struct CT) != 32u) return 7;
    if (((unsigned long long)(&ct) & 31ull) != 0ull) return 8;
    return h + 35;   /* 42 */
}
