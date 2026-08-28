// TF-C78 (D-CSUBSET-NOINLINE) corpus witness: GNU `__attribute__((noinline))`
// is HONORED end to end, and the honoring COMPOSES with the SHIPPED release
// optimizer rather than merely surviving a baseline build.
//
// ★ WHAT THIS EXAMPLE CAN AND CANNOT WITNESS — STATED PLAINLY.
// Inlining is SEMANTICS-PRESERVING by construction, so no exit code can
// distinguish "the callee was inlined" from "it was not". Every runtime probe
// that might seem to (comparing function addresses, measuring stack depth)
// either relies on undefined behavior or TAKES THE CALLEE'S ADDRESS, which
// makes the inliner refuse for an unrelated reason (§2.9 rule 4) and turns the
// check vacuous. Inventing one here would be a check that cannot fail.
//
// So the division of labor is deliberate:
//   * THIS example witnesses the COMPOSITION — all four attribute spellings
//     parse, reach the sink, and the resulting refusal still produces a
//     CORRECT, RUNNABLE program under the real shipped `release` pipeline on
//     every target. A refusal that broke codegen, mis-threaded an argument, or
//     desynchronized the call graph would land here as a wrong exit code.
//   * THE APPLIED FACT — that the Call instruction actually SURVIVES the
//     shipped pipeline — is asserted where it is observable, at MIR tier:
//     `MirLoweringCLinkage.NoInlineSurvivesShippedReleasePipeline`
//     (which loads release.pipeline.json BY NAME), plus
//     `Inlining.NoInlineCalleeIsNotInlined` and its flag-clear twin, and
//     `MirRebuildHelper.RebuildFunctionPreservesNoInline` for the propagation.
//
// RED-ON-DISABLE for those pins was MEASURED on a real arm64 binary, two ways:
// deleting `inlining.cpp`'s rule 2b, AND (with rule 2b intact) dropping only
// the `funcNoInline` argument in `mir_rebuild_helper.cpp`. Both collapse this
// program's helpers away and fold `main` to a single `mov x29, #0x2a` — the two
// failure modes are INDISTINGUISHABLE in the output, which is why the
// propagation carries its own pin.
//
// The four spellings, one per declaration:
//   1. mode-2, after the type:      `static int __attribute__((noinline)) f()`
//   2. mode-3, specifier prefix:    `static __attribute__((noinline)) int f()`
//   3. dunder form:                 `__attribute__((__noinline__))`
//   4. PROTOTYPE-only, definition bare — reaches MIR solely via the proto/def
//      OR-merge in the post-Pass-1.5 `mergedFnDecls` sweep.
//
// add_five(0)=5 -> times_two=10 -> minus_three=7 -> declared_first=8;
// 8 * 5 + 2 = 42.

static int __attribute__((noinline)) add_five(int k) { return k + 5; }

static __attribute__((noinline)) int times_two(int k) { return k * 2; }

static int __attribute__((__noinline__)) minus_three(int k) { return k - 3; }

// Spelled ONLY on the prototype; the definition below is bare.
static int declared_first(int k) __attribute__((noinline));
static int declared_first(int k) { return k + 1; }

int main(void) {
    int v = add_five(0);
    v = times_two(v);
    v = minus_three(v);
    v = declared_first(v);
    return v * 5 + 2;
}
