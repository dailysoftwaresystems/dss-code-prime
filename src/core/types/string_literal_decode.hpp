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
[[nodiscard]] inline std::optional<std::string>
decodeAdjacentStringBodies(Tree const& tree, NodeId node, SchemaTokenId bodyTokenKind) {
    std::string out;
    for (NodeId c : tree.children(node)) {
        if (tree.kind(c) != NodeKind::Token) continue;
        if (tree.tokenKind(c).v != bodyTokenKind.v) continue;
        auto seg = decodeStringLiteralBody(tree.text(c));   // phase 5 per segment
        if (!seg) return std::nullopt;                       // malformed escape — fail loud
        out += *seg;                                         // phase 6 byte-level join
    }
    return out;
}

} // namespace dss
