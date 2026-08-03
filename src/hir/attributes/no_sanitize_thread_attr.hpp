#pragma once

// TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): declaration thread-sanitizer-EXCLUSION
// side-table value. Attached per-node via `HirAttribute<NoSanitizeThreadAttr>` to
// a NATIVE FUNCTION declaration HIR node whose bound symbol carried a source-level
// `no_sanitize_thread` attribute — GNU `__attribute__((no_sanitize_thread))` /
// `[[gnu::no_sanitize_thread]]` (there is no C11/C23 standard spelling). Populated
// by CST→HIR lowering from the bound symbol's `SymbolRecord.isNoSanitizeThread`
// (which the semantic phase folded from the language's `attributeSemantics.effects`
// `noSanitizeThread` verb, gated on the declared type being a FnSig and OR-merged
// across the proto/definition pair); read at HIR→MIR lowering and stamped onto
// `MirFunc.noSanitizeThread`, where `mir_text`'s `appendFuncAttrs` prints it as the
// `nosanitizethread` function attribute.
//
// The exact `NoInlineAttr` / `AlwaysInlineAttr` discipline, a FIFTH axis over: its
// own side-table because "do not instrument this for data races" is neither linkage
// nor either inlining axis nor the pragma-borne optimizer opt-out. It rides the
// SAME declaration node as `LinkageAttr`/`NoInlineAttr`/`AlwaysInlineAttr`/
// `NoOptimizeAttr` so HIR→MIR reads all five at one place, and it stays sparse
// (only annotated declarations are recorded).
//
// ★★ WHAT THIS SINK IS, SAID PLAINLY, BECAUSE THE HONEST ANSWER IS NARROWER THAN
// ITS FOUR NEIGHBOURS' AND MUST NOT BE INFERRED FROM THEM. `NoInlineAttr` reaches
// a pass that REFUSES a splice; `AlwaysInlineAttr` reaches a pass that WAIVES a
// threshold; `NoOptimizeAttr` reaches a rebuilder that swaps in an identity policy.
// This one reaches NOBODY YET — MEASURED, `grep -rni sanitiz src/` has ZERO hits:
// DSS ships no sanitizer, no instrumentation pass, and no `-fsanitize` surface at
// all. The sink is that the FACT IS STORED AND OBSERVABLE: it survives every tier
// and is queryable in the `.dssir` text a user can dump. That is a real, testable
// property (the MIR-text pins assert exactly it), and it is deliberately not
// dressed up as a suppression of something that does not exist.
//
// ★ WHY THAT IS STILL WORTH FIVE HOPS RATHER THAN AN IGNORE-LIST ENTRY. The cheap
// alternative was to list `no_sanitize_thread` among the ABI-neutral hint names the
// linkage scan skips and let the semantic tier see nothing. That is precisely the
// class [[D-TEST-IGNORE-LIST-IS-A-LICENSE-TO-DROP]] (registry:545) exists to refuse:
// an ignore list may only ever say "this is not a linkage specifier", never be the
// ONLY place a name is mentioned. Storing the exclusion means the day an
// instrumentation pass lands it has a per-function input already threaded through
// every module copy; dropping it would mean re-deriving from source what the
// front-end had already parsed and thrown away.
//
// ★ DIRECTION OF A DROPPED FLAG: `AlwaysInlineAttr`'s, not `NoInlineAttr`'s. With
// no sanitizer to disarm, a lost `true` cannot produce a wrong program — only an
// unrecorded directive, invisible in the emitted binary because nothing reads it
// there either. It is threaded through every `MirFunc` creation, copy, rebuild and
// serialization path regardless, and the argument is TF-C81's verbatim: a
// half-landed flag and no flag are indistinguishable downstream, so the propagation
// hops need their OWN assertions rather than an end-to-end witness. There is no
// end-to-end behavioural witness available for this axis AT ALL (nothing observable
// changes in codegen), which makes the MIR-text and rebuild pins not merely the best
// evidence but the only evidence.
//
// ★ COMPOSES WITH EVERYTHING; CONTRADICTS NOTHING. `noinline`/`always_inline` are a
// mutually exclusive pair guarded by a loud `S_ConflictingInlineAttributes`. This
// axis has no partner to contradict — a function may be `no_sanitize_thread` and
// `noinline`, or `no_sanitize_thread` and `always_inline`, or inside a
// `#pragma optimize("", off)` region as well — so no contradiction gate exists and
// none is missing.

namespace dss {

struct NoSanitizeThreadAttr {
    bool isNoSanitizeThread = false;
};

} // namespace dss
