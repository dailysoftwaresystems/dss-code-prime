#pragma once

#include "core/export.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_id.hpp"
#include "hir/hir_node.hpp"

namespace dss {

class Hir;   // hir.hpp — full definition needed at call sites; here forward-decl

// Stateful walker over a frozen `Hir` module — the HIR analog of `TreeCursor`.
// Unlike TreeCursor there are no CST/AST modes: HIR has no whitespace/comment
// leaves to skip, it is already the abstracted, structured form. Cheap to copy
// (a pointer + an id); a read-only observer, so two cursors over one module are
// independent.
//
// Movement methods are `[[nodiscard]]` — the bool says whether the move
// happened, and discarding it is almost always a bug. Lifetime: a HirCursor
// must NOT outlive its Hir (it holds a raw pointer).
class DSS_EXPORT HirCursor {
public:
    HirCursor(Hir const& hir, HirNodeId start) noexcept;

    // ── position ──
    [[nodiscard]] HirNodeId  current() const noexcept { return current_; }
    [[nodiscard]] Hir const& hir()     const noexcept { return *hir_; }

    // ── convenience forwarders (sentinel/empty at an invalid position) ──
    [[nodiscard]] HirKind  kind()   const noexcept;
    [[nodiscard]] HirFlags flags()  const noexcept;
    [[nodiscard]] TypeId   typeId() const noexcept;

    // ── movement — return false if no such node exists ──
    [[nodiscard]] bool gotoFirstChild()  noexcept;
    [[nodiscard]] bool gotoLastChild()   noexcept;
    [[nodiscard]] bool gotoNextSibling() noexcept;
    [[nodiscard]] bool gotoPrevSibling() noexcept;
    [[nodiscard]] bool gotoParent()      noexcept;

    // ── convenience ──
    // True iff the node has no children (also true at an invalid position, where
    // movement would likewise fail — consistent).
    [[nodiscard]] bool isAtLeaf() const noexcept;
    // Depth from root (0 = root). At an invalid position returns 0 (matches
    // `TreeCursor::depth`). The parent walk is capped by `nodeCount` so a
    // corrupt parent-cycle returns -1 instead of looping forever — `-1` is the
    // corruption indicator a `noexcept` const observer can offer when the
    // alternative is a hang.
    [[nodiscard]] int  depth() const noexcept;

private:
    Hir const* hir_;       // non-null after construction; never reassigned
    HirNodeId  current_;
};

} // namespace dss
