#pragma once

#include "analysis/semantic/semantic_model.hpp"
#include "core/export.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "lsp/protocol.hpp"

// Position/offset/node plumbing shared by the LSP semantic request
// handlers (hover / definition / references / rename / completion /
// signatureHelp). Pure functions over a Tree + SourceBuffer + the
// SemanticModel — no nlohmann, no transport. The handlers in
// lsp_server.cpp compose these with the model's query surface.

namespace dss::lsp {

// Map an LSP Position {line, character (UTF-16)} to a UTF-8 ByteOffset in
// `buffer`. Out-of-range lines clamp to the buffer end; a character past
// the line end clamps to the line's last byte. Walks the buffer once to
// find the requested line start, then hands off to the shared
// `lineByteRangeFor` + `utf16ColumnToByteOffset` helpers (in
// `utf16_column.hpp`) so `\r`/`\n`/EOB clamping matches every other
// LSP line-resolution path.
[[nodiscard]] DSS_EXPORT dss::ByteOffset positionToByteOffset(
    dss::SourceBuffer const& buffer, Position pos) noexcept;

// Inverse: a ByteOffset → an LSP Position (0-based line + UTF-16 column).
[[nodiscard]] DSS_EXPORT Position byteOffsetToPosition(
    dss::SourceBuffer const& buffer, dss::ByteOffset offset);

// A SourceSpan → an LSP Range (both endpoints converted via
// byteOffsetToPosition).
[[nodiscard]] DSS_EXPORT Range spanToRange(
    dss::SourceBuffer const& buffer, dss::SourceSpan span);

// The deepest AST node (CursorMode::Ast, skipping EmptySpace) whose span
// contains `offset`. Returns InvalidNode when the tree is empty or no
// node contains the offset. Ties (a zero-width or boundary-touching
// node) resolve to the deepest containing node.
[[nodiscard]] DSS_EXPORT NodeId nodeAtOffset(
    dss::Tree const& tree, dss::ByteOffset offset);

// The deepest scope whose anchor-subtree span contains `offset`, walking
// `model.scopes()`. Returns the tree's root scope (or the nearest
// containing scope) — never InvalidScope for a non-empty model. Used by
// completion to collect in-scope bindings.
[[nodiscard]] DSS_EXPORT ScopeId scopeAtOffset(
    SemanticModel const& model, dss::Tree const& tree, dss::ByteOffset offset);

} // namespace dss::lsp
