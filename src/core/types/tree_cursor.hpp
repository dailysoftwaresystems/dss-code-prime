#pragma once

#include "core/export.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree_node.hpp"

#include <cstdint>
#include <string_view>

namespace dss {

class Tree;     // tree.hpp — full definition needed at call sites; here forward-decl

// Two ways to walk a Tree:
//
//   Cst — visits every node, including whitespace/comment leaves. Used by
//         formatters, IDE tooling, and anything that wants the original
//         source byte-for-byte.
//   Ast — skips nodes flagged NodeFlags::EmptySpace. Used by semantic
//         passes and IR generation. Critically, AST mode does NOT skip
//         Missing or Synthetic nodes — those are part of the parse result
//         and downstream phases need to see them (e.g., to report "you
//         didn't write the `)`").
enum class CursorMode : std::uint8_t {
    Cst,
    Ast,
};

// Stateful walker over a Tree. Cheap to copy (small POD-ish state); meant
// to be short-lived in practice.
//
// Copy-able by design — unlike `TreeBuilder::OpenScope` which is move-only
// because it owns a mutable frame. A `TreeCursor` is read-only state; two
// cursors over the same node are independent observers.
//
// Mode is immutable per cursor. To switch CST↔AST mid-traversal, construct
// a new cursor at the desired position via `Tree::cursor()` / `astCursor()`,
// or copy the current cursor and re-construct.
//
// Thread safety: thread_compatible. Multiple cursors over the same Tree
// across threads are safe (Tree is immutable post-finish); concurrent
// access to a single cursor instance requires external synchronization.
//
// Lifetime: a TreeCursor must NOT outlive its Tree. The cursor holds a
// raw pointer; if the Tree is destroyed first, every cursor method is UB.
// Bookmarks additionally validate via TreeId, which catches the case where
// a Tree was destroyed and a fresh one happened to land at the same heap
// address (ABA).
class DSS_EXPORT TreeCursor {
public:
    TreeCursor(Tree const& tree, NodeId start, CursorMode mode = CursorMode::Cst) noexcept;

    // ── position ──
    [[nodiscard]] NodeId      current() const noexcept { return current_; }
    [[nodiscard]] NodeKind    kind()    const noexcept;
    [[nodiscard]] NodeFlags   flags()   const noexcept;
    [[nodiscard]] CursorMode  mode()    const noexcept { return mode_; }
    [[nodiscard]] Tree const& tree()    const noexcept { return *tree_; }

    // ── convenience forwarders (save the c.tree().X(c.current()) boilerplate) ──
    // Each forwards to the corresponding Tree method; same discriminant-
    // assertion / bounds-check behavior. Returns sentinel/empty for an
    // invalid cursor position rather than aborting.
    [[nodiscard]] std::string_view text()      const noexcept;
    [[nodiscard]] SourceSpan       span()      const noexcept;
    [[nodiscard]] RuleId           rule()      const;          // requires kind == Internal
    [[nodiscard]] SchemaTokenId    tokenKind() const;          // requires kind == Token

    // ── movement — return false if no such node exists ──
    //
    // All five are `[[nodiscard]]`: discarding the return is almost always
    // a bug (the caller would silently lose track of whether the cursor
    // moved).
    [[nodiscard]] bool gotoFirstChild()  noexcept;
    [[nodiscard]] bool gotoLastChild()   noexcept;
    [[nodiscard]] bool gotoNextSibling() noexcept;
    [[nodiscard]] bool gotoPrevSibling() noexcept;
    [[nodiscard]] bool gotoParent()      noexcept;

    // ── bookmark / restore ──
    //
    // Opaque token capturing (tree_, current_, tree.id()). Restore aborts
    // (release-fatal) on three distinct failure modes:
    //   - Bookmark default-constructed / never initialized.
    //   - Bookmark taken on a different Tree instance.
    //   - Bookmark taken on a different Tree at the same heap address
    //     (ABA — caught via TreeId).
    // Each emits a distinct stderr message before aborting, so triage is
    // unambiguous.
    class DSS_EXPORT Bookmark {
    public:
        constexpr Bookmark() noexcept = default;
        [[nodiscard]] constexpr bool         valid() const noexcept { return tree_ != nullptr && id_.valid(); }
        [[nodiscard]] constexpr NodeId       id()    const noexcept { return id_; }
        [[nodiscard]] constexpr Tree const*  tree()  const noexcept { return tree_; }
        [[nodiscard]] constexpr TreeId       treeId() const noexcept { return treeId_; }
        constexpr bool operator==(Bookmark const&) const noexcept = default;

    private:
        friend class TreeCursor;
        constexpr Bookmark(Tree const* t, NodeId i, TreeId tid) noexcept
            : tree_(t), id_(i), treeId_(tid) {}

        Tree const* tree_ = nullptr;
        NodeId      id_;
        TreeId      treeId_;
    };

    [[nodiscard]] Bookmark mark()    const noexcept;
    void                   restore(Bookmark saved) noexcept;

    // ── convenience ──
    //
    // In CST mode: true iff the node has no children at all.
    // In AST mode: true iff no visible (non-EmptySpace) child exists.
    // Returns true (i.e. "is leaf-like") when the cursor is at an invalid
    // position; movement would also return false, so this is consistent.
    [[nodiscard]] bool isAtLeaf() const noexcept;

    // Depth from root (0 = root, 1 = root's child, ...). O(depth) parent
    // walk; intentionally not cached — typical depths are small. Capped by
    // tree.nodeCount() iterations so a corrupt parent-cycle returns -1
    // rather than looping forever.
    [[nodiscard]] int  depth() const noexcept;

private:
    // True iff `id` is visible under the current cursor mode.
    [[nodiscard]] bool isVisible_(NodeId id) const noexcept;

    Tree const* tree_;        // non-null after construction; never reassigned across this cursor's lifetime
    NodeId      current_;
    CursorMode  mode_;
};

} // namespace dss
