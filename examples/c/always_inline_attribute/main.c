// TF-C81 (D-CSUBSET-ALWAYSINLINE) corpus witness: GNU
// `__attribute__((always_inline))` is HONORED end to end, and the honoring
// COMPOSES with the SHIPPED release optimizer rather than merely surviving a
// baseline build.
//
// ★ WHAT THIS EXAMPLE CAN AND CANNOT WITNESS — STATED PLAINLY, AND IT IS THE
// SAME HONEST SPLIT `noinline_attribute` USES. Inlining is SEMANTICS-PRESERVING
// by construction, so NO exit code can distinguish "the callee was inlined" from
// "it was not". Every runtime probe that appears to (comparing function
// addresses, measuring stack depth) either invokes undefined behavior or TAKES
// THE CALLEE'S ADDRESS — which makes the inliner refuse under §2.9 rule 4 for an
// unrelated reason and renders the check vacuous. Inventing one here would be a
// check that cannot fail.
//
// So the division of labor is deliberate:
//   * THIS example witnesses the COMPOSITION — all five attribute spellings
//     parse, reach the sink, and the resulting SPLICE still produces a CORRECT,
//     RUNNABLE program under the real shipped `release` pipeline on every
//     target. `wide()` is deliberately far OVER the release `inlineThreshold`
//     (50), so the release arm genuinely exercises the cost-model BYPASS and not
//     an inline that would have happened anyway. A bypass that mis-threaded an
//     argument, cloned a block wrongly, or desynchronized the call graph lands
//     here as a wrong exit code on four targets at once.
//   * THE APPLIED FACT — that the Call instruction is actually GONE from the
//     optimized module — is asserted where it is observable, at MIR tier:
//     `MirLoweringCLinkage.AlwaysInlineBypassesThresholdInShippedRelease`
//     (which loads release.pipeline.json BY NAME) and its non-vacuous twin
//     `OverThresholdCalleeWithoutAlwaysInlineKeepsItsCall` (which proves the
//     callee is genuinely over-threshold), plus
//     `Inlining.AlwaysInlineCalleeBypassesCostThreshold` and
//     `MirRebuildHelper.RebuildFunctionPreservesAlwaysInline` for the
//     propagation.
//
// ★★ WHAT THE ATTRIBUTE DOES NOT PROMISE, AND WHY THE `release` ARM IS
// MANDATORY RATHER THAN DECORATION. `always_inline` in DSS EXEMPTS THE CALLEE
// FROM THE INLINER'S SIZE THRESHOLD — nothing more. Every correctness refusal
// still wins (Weak binding, recursion, address-escape, no returning path), and a
// pipeline with NO inliner has no threshold to bypass: under the shipped `debug`
// pipeline (`Identity` only) this attribute is VACUOUS and every call below
// stays out-of-line. GCC and clang honour `always_inline` at -O0 because their
// inliner always runs; DSS's does not. That asymmetry is stated in the
// `c.lang.json` effects row and pinned by
// `MirLoweringCLinkage.AlwaysInlineIsInertUnderShippedDebugPipeline`. A
// baseline-only example would leave the one interaction that matters — the
// optimizer x feature composition — completely unwitnessed, which is why this
// example carries a `{"shippedPipeline": "release"}` arm.
//
// The five spellings, one per declaration:
//   1. mode-2, after the type:    `static int __attribute__((always_inline)) f()`
//   2. mode-3, specifier prefix:  `static __attribute__((always_inline)) int f()`
//   3. dunder form:               `__attribute__((__always_inline__))`
//   4. ★ THE SQLITE SHAPE:        `__attribute__((always_inline)) inline` —
//      exactly what `SQLITE_INLINE` expands to, and sqlite's ONE use site
//      (btree.c:1846, `static SQLITE_INLINE int allocateSpace(...)`) spells it
//      this way. The attribute and the C99 `inline` specifier (TF-C79) must
//      compose.
//   5. PROTOTYPE-only, definition bare — reaches MIR solely via the proto/def
//      OR-merge in the post-Pass-1.5 `mergedFnDecls` sweep.
//
// ★ ALL ARITHMETIC IN `wide()` IS UNSIGNED ON PURPOSE. The body deliberately
// overflows to stay large and un-foldable; signed overflow would be undefined
// behavior, so the accumulator is `unsigned` where wraparound is defined.
//
// add_five(0)=5 -> times_two=10 -> minus_three=7 -> declared_first=8, and
// wide(8) is a fixed nonzero constant, so the program exits 42.

// 1. mode-2: the attribute sits between the type and the declarator.
static int __attribute__((always_inline)) add_five(int k) { return k + 5; }

// 2. mode-3: the attribute sits in the specifier prefix, before the type.
static __attribute__((always_inline)) int times_two(int k) { return k * 2; }

// 3. dunder spelling — one config entry covers both via `stripDunder`.
static int __attribute__((__always_inline__)) minus_three(int k) { return k - 3; }

// 4. THE SQLITE SHAPE: always_inline COMBINED with the C99 `inline` specifier.
static __attribute__((always_inline)) inline int plus_one(int k) { return k + 1; }

// 5. Spelled ONLY on the prototype; the definition below is bare.
static int declared_first(int k) __attribute__((always_inline));
static int declared_first(int k) { return k * 1; }

// ★ THE OVER-THRESHOLD CALLEE. Far larger than the shipped release
// `inlineThreshold` (50 MIR instructions), so WITHOUT the attribute the cost
// model refuses it and the call survives; WITH it the call is spliced away.
// That difference was MEASURED on a real arm64 binary (`bl _wide` present
// without, absent with).
// ⚠ RE-MEASURED 2026-08-17 on real Apple Silicon (macho64-arm64-darwin-exec,
// `--config=release`, Apple `otool -tV`): this annotation used to read
// `bl _sym_<id>` while D-LINK-MACHO-IMAGE-SYMBOL-NAMES-REPLACED-BY-SYNTHETIC-IDS
// was open, because a Mach-O image spelled EVERY function `_sym_<id>` in its
// symbol table and a disassembler symbolicates the branch target from exactly
// that table. Closing that row did not move one instruction byte — only the
// name the disassembler can print for the target it always had.
// ★ THE OLD SPELLING WAS `_sym_75` AND IS NOT REPRODUCIBLE — say so rather
// than leave a number that will not come back. A SymbolId is assigned per
// build, so the id DRIFTS with unrelated front-end changes: the same
// pre-fix probe on 2026-08-17 printed `_sym_110` for `main`, not the `_sym_75`
// this comment carried. The NAME is stable; the id never was, which is a
// second reason the image had no business printing it.
static __attribute__((always_inline)) int wide(int k) {
    unsigned u = (unsigned)k;
    u = u * 3u + 1u; u = u ^ (u >> 2); u = u + 7u;
    u = u * 3u + 2u; u = u ^ (u >> 2); u = u + 14u;
    u = u * 3u + 3u; u = u ^ (u >> 2); u = u + 21u;
    u = u * 3u + 4u; u = u ^ (u >> 2); u = u + 28u;
    u = u * 3u + 5u; u = u ^ (u >> 2); u = u + 35u;
    u = u * 3u + 6u; u = u ^ (u >> 2); u = u + 42u;
    u = u * 3u + 7u; u = u ^ (u >> 2); u = u + 49u;
    u = u * 3u + 8u; u = u ^ (u >> 2); u = u + 56u;
    u = u * 3u + 9u; u = u ^ (u >> 2); u = u + 63u;
    u = u * 3u + 10u; u = u ^ (u >> 2); u = u + 70u;
    u = u * 3u + 11u; u = u ^ (u >> 2); u = u + 77u;
    u = u * 3u + 12u; u = u ^ (u >> 2); u = u + 84u;
    u = u * 3u + 13u; u = u ^ (u >> 2); u = u + 91u;
    u = u * 3u + 14u; u = u ^ (u >> 2); u = u + 98u;
    u = u * 3u + 15u; u = u ^ (u >> 2); u = u + 105u;
    return (int)(u & 0x7fffffffu);
}

int main(void) {
    int v = add_five(0);
    v = times_two(v);
    v = minus_three(v);
    v = plus_one(v);
    v = declared_first(v);
    return wide(v) != 0 ? 42 : 1;
}
