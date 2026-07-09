#pragma once

// THE chokepoint for a (possibly adjacent-concatenated) string-literal EXPR.
//
// C 5.1.1.2 phase 6 (D-CSUBSET-ADJACENT-STRING-CONCAT): adjacent string
// literals concatenate — `"a" "b"` ≡ `"ab"`. The grammar rule
// `stringLiteralExpr` admits ONE required `StringStart StringLiteral` pair
// plus a zero-or-more repeat of further pairs (flat children); a lone string
// is exactly the pre-c20 two-child shape, so this function reduces to a single
// `decodeStringLiteralBody` call for it (byte-identical behavior).
//
// Concatenation is at the DECODED-byte level: phase 5 (escape decode) runs
// per body BEFORE phase 6 joins the bytes. So `"\x41" "1"` decodes to 'A'
// then '1' → "A1", NOT a raw-token merge `\x411` (which would decode the hex
// escape across the segment boundary). EVERY consumer that turns a
// `stringLiteralExpr` node into decoded bytes (HIR lowering, the extern
// asm-label / library-override path, the semantic Array<Char,N> typer) MUST
// route through here — a consumer that reads only the FIRST body child
// silently drops the rest = a SILENT MISCOMPILE. Coverage-by-construction:
// this is the ONLY path from a stringLiteralExpr node to decoded bytes.
//
// The attribute-argument path (`__attribute__((visibility("hidden")))`) is
// the one body consumer that does NOT route here: a string ARGUMENT cannot
// concatenate in that grammar position the way a primary-expression string
// can, so it fails loud on an unexpected second opener rather than calling
// this (see cst_to_hir's linkage-specifier scan).

#include "core/types/char_decode.hpp"   // decodeStringLiteralBody (per-segment phase-5 decode)
#include "core/types/strong_ids.hpp"    // SchemaTokenId, NodeId
#include "core/types/tree.hpp"          // Tree (children / kind / tokenKind / text)
#include "core/types/tree_node.hpp"     // NodeKind

#include <optional>
#include <string>

namespace dss {

// Decode and concatenate every `bodyTokenKind` body child of `node` (a
// `stringLiteralExpr` rule node) in source order. Each body is decoded
// INDEPENDENTLY via `decodeStringLiteralBody` (C-family `\`-escapes), then the
// decoded byte sequences are appended. The result is NOT NUL-terminated (the
// trailing NUL is implied by the literal's `Array<Char, N+1>` type).
//
// Returns std::nullopt and stops on the FIRST malformed escape (caller fails
// loud — never a guessed/partial size). Returns "" only for a node with no
// matching body child (the lone-empty-string `""` case still has a body token
// whose text is empty → decodes to ""). EmptySpace children are skipped; a
// non-token or wrong-kind child is ignored, so this is robust to interleaved
// trivia between adjacent pieces.
//
// `outcome` (when non-null) receives the AGGREGATE result: the specific error of
// the first failing segment (so a caller renders H_InvalidUniversalCharacterName
// vs. a generic escape error) and `usedByteEscape` ORed across every decoded
// segment (so the wide/UTF path can fail loud on a `\x`/octal escape anywhere in
// an adjacent-concatenated literal — D-CSUBSET-WIDE-HEX-OCTAL-ESCAPE-VALUE).
[[nodiscard]] inline std::optional<std::string>
decodeAdjacentStringBodies(Tree const& tree, NodeId node, SchemaTokenId bodyTokenKind,
                           EscapeDecodeOutcome* outcome = nullptr) {
    std::string out;
    for (NodeId c : tree.children(node)) {
        if (tree.kind(c) != NodeKind::Token) continue;
        if (tree.tokenKind(c).v != bodyTokenKind.v) continue;
        EscapeDecodeOutcome oc;
        auto seg = decodeStringLiteralBody(tree.text(c), &oc);   // phase 5 per segment
        if (outcome && oc.usedByteEscape) outcome->usedByteEscape = true;
        if (!seg) {
            if (outcome) outcome->error = oc.error;              // first failing segment's reason
            return std::nullopt;                                 // malformed escape — fail loud
        }
        out += *seg;                                             // phase 6 byte-level join
    }
    return out;
}

// SF4 (Cycle D, C11/C23 6.4.5p5): the EFFECTIVE encoding prefix of a run of
// adjacent string literals (`L"a" "b"`). Scans EVERY opener token of the
// `stringLiteralExpr` rule `node` and folds them per 6.4.5p5: a run has at most
// ONE distinct NON-narrow prefix — narrow segments widen to it, POSITION-
// INDEPENDENTLY ("if any of the tokens has an encoding prefix, the resulting
// sequence is treated as having that prefix"). TWO DIFFERENT non-narrow prefixes
// is the case 6.4.5p5 leaves implementation-defined; this implementation REJECTS
// it (`conflict`, matching gcc/clang) rather than silently dropping one prefix's
// element width.
//
// THE shared chokepoint (both the HIR lowering AND the semantic typer route
// through it): the scan is the drift-prone part — a per-tier reimplementation
// re-opens the mistype/miscompile defects Cycle A shipped, where keying the run's
// core on the FIRST opener dropped a later prefix (`"a" L"b"` → narrow; `u"a" U"b"`
// → silently U16).
//
// `isNonNarrowOpener` is a TIER-SUPPLIED classifier over opener TOKEN KINDS — NOT
// resolved element cores. This is load-bearing for target-agnosticism: on pe,
// `u"` and `L"` BOTH resolve to U16, so a core-keyed conflict check would silently
// ACCEPT `u"a" L"b"` on Windows while REJECTING it on Linux (I32 ≠ U16). The token
// kinds `u"` ≠ `L"` differ on EVERY format, so the fold is format-agnostic.
// `narrowFallback` is the opener kind the caller uses when the run is all-narrow
// (no non-narrow prefix present).
//
//   effectiveOpener = the single distinct non-narrow opener token kind, or
//                     `narrowFallback` when the run is all-narrow.
//   conflict        = the run carries ≥2 DISTINCT non-narrow opener token kinds.
//
// On `conflict` the `effectiveOpener` value is unspecified (the first non-narrow
// seen); the caller MUST branch on `conflict` FIRST and fail loud, never consume
// the opener (see the HIR/semantic tiers' explicit early conflict branch).
struct EffectiveStringPrefix {
    SchemaTokenId effectiveOpener;
    bool          conflict;
};

template <typename IsNonNarrowOpener>
[[nodiscard]] inline EffectiveStringPrefix
effectiveStringConcatPrefix(Tree const& tree, NodeId node,
                            IsNonNarrowOpener&& isNonNarrowOpener,
                            SchemaTokenId narrowFallback) {
    SchemaTokenId nonNarrow{};   // first non-narrow opener kind seen (invalid ⇒ none)
    bool conflict = false;
    for (NodeId c : tree.children(node)) {
        if (tree.kind(c) != NodeKind::Token) continue;
        SchemaTokenId const tk = tree.tokenKind(c);
        if (!isNonNarrowOpener(tk)) continue;             // narrow opener / body / trivia
        if (!nonNarrow.valid()) nonNarrow = tk;           // the run's non-narrow prefix
        else if (nonNarrow.v != tk.v) conflict = true;    // a SECOND, DIFFERENT one → reject
    }
    return { nonNarrow.valid() ? nonNarrow : narrowFallback, conflict };
}

} // namespace dss
