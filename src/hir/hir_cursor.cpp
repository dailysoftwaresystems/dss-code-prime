#include "hir/hir_cursor.hpp"

#include "hir/hir.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace dss {

namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// The index of `id` among `siblings`, or kNpos if absent.
[[nodiscard]] std::size_t indexOf(std::span<HirNodeId const> siblings, HirNodeId id) noexcept {
    for (std::size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i] == id) return i;
    }
    return kNpos;
}

// A current node whose parent says it's a child but isn't actually in that
// parent's child list is a `parentOf_`/`childPool_` invariant violation —
// structural corruption, not "no next sibling". Fail loud rather than masking
// it as a benign end-of-list return.
[[noreturn]] void siblingInvariantViolated(char const* where, HirNodeId child, HirNodeId parent) noexcept {
    std::fprintf(stderr,
                 "dss::HirCursor fatal: %s: HirNodeId=%u not present in parent "
                 "HirNodeId=%u's child list (parentOf_/childPool_ invariant violated)\n",
                 where, child.v, parent.v);
    std::abort();
}

} // namespace

HirCursor::HirCursor(Hir const& hir, HirNodeId start) noexcept
    : hir_(&hir), current_(start) {}

HirKind HirCursor::kind() const noexcept {
    return current_.valid() ? hir_->kind(current_) : HirKind::Error;
}

HirFlags HirCursor::flags() const noexcept {
    return current_.valid() ? hir_->flags(current_) : HirFlags::None;
}

TypeId HirCursor::typeId() const noexcept {
    return current_.valid() ? hir_->typeId(current_) : InvalidType;
}

bool HirCursor::gotoFirstChild() noexcept {
    if (!current_.valid()) return false;
    auto kids = hir_->children(current_);
    if (kids.empty()) return false;
    current_ = kids.front();
    return true;
}

bool HirCursor::gotoLastChild() noexcept {
    if (!current_.valid()) return false;
    auto kids = hir_->children(current_);
    if (kids.empty()) return false;
    current_ = kids.back();
    return true;
}

bool HirCursor::gotoNextSibling() noexcept {
    if (!current_.valid()) return false;
    HirNodeId const p = hir_->parent(current_);
    if (!p.valid()) return false;                 // root has no siblings
    auto kids = hir_->children(p);
    std::size_t const idx = indexOf(kids, current_);
    if (idx == kNpos) siblingInvariantViolated("gotoNextSibling", current_, p);
    if (idx + 1 >= kids.size()) return false;     // legitimate end-of-list
    current_ = kids[idx + 1];
    return true;
}

bool HirCursor::gotoPrevSibling() noexcept {
    if (!current_.valid()) return false;
    HirNodeId const p = hir_->parent(current_);
    if (!p.valid()) return false;
    auto kids = hir_->children(p);
    std::size_t const idx = indexOf(kids, current_);
    if (idx == kNpos) siblingInvariantViolated("gotoPrevSibling", current_, p);
    if (idx == 0) return false;                   // legitimate start-of-list
    current_ = kids[idx - 1];
    return true;
}

bool HirCursor::gotoParent() noexcept {
    if (!current_.valid()) return false;
    HirNodeId const p = hir_->parent(current_);
    if (!p.valid()) return false;
    current_ = p;
    return true;
}

bool HirCursor::isAtLeaf() const noexcept {
    if (!current_.valid()) return true;
    return hir_->children(current_).empty();
}

int HirCursor::depth() const noexcept {
    // Match TreeCursor: depth at an invalid position is 0 (there's nothing to
    // measure — and -1 is reserved for the corruption signal below).
    if (!current_.valid()) return 0;
    int d = 0;
    HirNodeId cur = current_;
    std::size_t const cap = hir_->nodeCount();
    for (std::size_t steps = 0; steps <= cap; ++steps) {
        HirNodeId const p = hir_->parent(cur);
        if (!p.valid()) return d;
        cur = p;
        ++d;
    }
    return -1;   // parent chain longer than nodeCount — corrupt cycle
}

} // namespace dss
