#include "lsp/lsp_semantic_query.hpp"

#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "lsp/utf16_column.hpp"

#include <string_view>

namespace dss::lsp {

dss::ByteOffset positionToByteOffset(dss::SourceBuffer const& buffer,
                                     Position pos) noexcept {
    const auto text = buffer.text();
    if (text.empty()) return dss::ByteOffset{0};

    // Walk to the start byte of line `pos.line` (0-based). Lines are
    // delimited by '\n'; we count newline boundaries. (A pure newline-
    // count walk is the simplest precise way to land on the requested
    // line — the buffer's line-offset table is private; once we have
    // the line's first byte we hand off to the shared
    // `lineByteRangeFor` helper for the line bounds so `\r`/`\n`/EOB
    // handling matches `diagnostic_translator`'s.)
    std::uint32_t line      = 0;
    std::uint32_t lineStart = 0;
    std::size_t   i         = 0;
    while (i < text.size() && line < pos.line) {
        if (text[i] == '\n') {
            ++line;
            lineStart = static_cast<std::uint32_t>(i + 1);
        }
        ++i;
    }
    const auto lr = lineByteRangeFor(buffer, lineStart);
    const std::string_view lineText =
        text.substr(lr.startByte, lr.endByte - lr.startByte);
    const std::uint32_t byteInLine =
        utf16ColumnToByteOffset(lineText, pos.character);
    return dss::ByteOffset{lr.startByte + byteInLine};
}

Position byteOffsetToPosition(dss::SourceBuffer const& buffer,
                              dss::ByteOffset offset) {
    const auto lc = buffer.lineCol(offset);
    const auto lr = lineByteRangeFor(buffer, offset);
    const auto text = buffer.text();
    const std::string_view lineText =
        text.substr(lr.startByte, lr.endByte - lr.startByte);
    const std::uint32_t byteInLine =
        (offset >= lr.startByte) ? (offset - lr.startByte) : 0u;
    Position p;
    p.line      = (lc.line > 0) ? lc.line - 1 : 0u;
    p.character = utf8ByteOffsetToUtf16Column(lineText, byteInLine);
    return p;
}

Range spanToRange(dss::SourceBuffer const& buffer, dss::SourceSpan span) {
    Range r;
    r.start = byteOffsetToPosition(buffer, span.start());
    r.end   = byteOffsetToPosition(buffer, span.end());
    return r;
}

NodeId nodeAtOffset(dss::Tree const& tree, dss::ByteOffset offset) {
    if (!tree.root().valid()) return InvalidNode;
    // Guard the documented contract: if `offset` lies OUTSIDE the root
    // node's span (and isn't the at-end boundary on a non-empty root),
    // return InvalidNode. Without this, the function would silently
    // return the root for any non-empty tree regardless of `offset`,
    // contradicting the header's "no node contains the offset" clause.
    {
        const auto rootSpan = tree.span(tree.root());
        if (!rootSpan.contains(offset)
            && !(offset == rootSpan.end() && !rootSpan.isEmpty())) {
            return InvalidNode;
        }
    }
    // Descend from the root: at each level pick the visible child whose
    // span contains `offset`; stop when no child does. The deepest such
    // node is the answer. A boundary offset (== a node's end) is treated
    // as contained by `containsSpan`-style half-open logic via
    // SourceSpan::contains, except we also accept offset == span.end at a
    // leaf so an at-end cursor still lands on the trailing token.
    NodeId current = tree.root();
    for (;;) {
        NodeId next = InvalidNode;
        for (NodeId child : tree.children(current)) {
            if (isEmptySpace(tree.flags(child))) continue;
            const auto sp = tree.span(child);
            if (sp.contains(offset)
                || (offset == sp.end() && !sp.isEmpty())) {
                next = child;
                break;
            }
        }
        if (!next.valid()) break;
        current = next;
    }
    return current;
}

ScopeId scopeAtOffset(SemanticModel const& model, dss::Tree const& tree,
                      dss::ByteOffset offset) {
    auto const& scopes = model.scopes();
    // The best scope is the deepest one (largest depth from root) whose
    // anchor subtree-span contains `offset` AND belongs to this tree. The
    // root scope (anchor == tree.root()) is the fallback.
    ScopeId best{};
    std::size_t bestDepth = 0;
    for (std::size_t i = 1; i < scopes.size(); ++i) {
        auto const& rec = scopes[i];
        if (rec.tree.v != tree.id().v) continue;
        if (!rec.anchor.valid()) continue;
        const auto sp = tree.span(rec.anchor);
        if (!sp.contains(offset) && !(offset == sp.end() && !sp.isEmpty())) {
            continue;
        }
        // Depth = walk parent chain.
        std::size_t depth = 0;
        ScopeId p = rec.parent;
        while (p.valid() && p.v < scopes.size()) {
            ++depth;
            p = scopes[p.v].parent;
        }
        // Strict greater-than: ties at the same depth resolve to the
        // FIRST-found scope in iteration order (which is also the one
        // declared first by Pass 1). Using `>=` here would silently let
        // the last-iterated scope win, which is a coin-flip dependent
        // on Pass 1's ordering and surprising in user-facing tooling.
        if (!best.valid() || depth > bestDepth) {
            best      = ScopeId{static_cast<std::uint32_t>(i)};
            bestDepth = depth;
        }
    }
    return best;
}

} // namespace dss::lsp
