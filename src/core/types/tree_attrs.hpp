#pragma once

#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// `NodeAttribute<T>` — a typed side-table mapping NodeId → T, bound to one
// Tree. This is the ONLY way later passes (semantic analysis, IR gen)
// annotate the tree; the Tree itself stays immutable after construction.
//
// Storage auto-promotes from sparse (unordered_map) to dense
// (vector<optional<T>> indexed by NodeId.v) once a coverage threshold is
// crossed. The choice is internal; the public API is identical in either
// mode.
//
// Lifetime: a NodeAttribute holds a raw `Tree const*` for nodeCount-based
// bounds checks, so the caller MUST NOT let the bound Tree outlive the
// attribute. Same rule as TreeCursor.

namespace dss {

namespace detail::attr {

[[noreturn]] inline void attrFatal(char const* what) {
    std::fputs("dss::NodeAttribute fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Cross-tree variant — emits both TreeIds so death-test regex can pin
// both numbers without parsing pointer addresses.
[[noreturn]] inline void crossTreeFatal(std::uint32_t boundTreeId, std::uint32_t idTreeTag) {
    std::fputs("dss::NodeAttribute fatal: ", stderr);
    std::fprintf(stderr,
                 "NodeAttribute bound to TreeId=%u got NodeId from TreeId=%u",
                 boundTreeId, idTreeTag);
    std::fputc('\n', stderr);
    std::abort();
}

// Sparse → dense once coverage reaches kPromoteCoverageNumer /
// kPromoteCoverageDenom (50%). Below kPromoteFloor nodes, stay sparse —
// dense's per-slot std::optional<T> overhead dominates on tiny trees.
inline constexpr std::size_t kPromoteCoverageNumer = 1;
inline constexpr std::size_t kPromoteCoverageDenom = 2;
inline constexpr std::size_t kPromoteFloor         = 16;

} // namespace detail::attr

template <typename T>
class NodeAttribute {
public:
    static_assert(std::is_move_constructible_v<T>,
                  "NodeAttribute<T>: T must be move-constructible");

    using value_type = T;

    explicit NodeAttribute(Tree const& tree) noexcept
        : tree_(&tree), treeId_(tree.id()) {}

    NodeAttribute(NodeAttribute const&)            = delete;
    NodeAttribute& operator=(NodeAttribute const&) = delete;

    // Custom move ops reset the source's `denseCount_` AND its variant back
    // to an empty SparseMap_ so the moved-from object's observable state
    // (size(), empty(), isDense(), iteration) is internally consistent
    // rather than relying on the std-library's "valid but unspecified"
    // contract for the underlying map / vector.
    NodeAttribute(NodeAttribute&& other) noexcept
        : tree_(other.tree_)
        , treeId_(other.treeId_)
        , storage_(std::move(other.storage_))
        , denseCount_(other.denseCount_) {
        other.storage_.template emplace<SparseMap_>();
        other.denseCount_ = 0;
    }

    NodeAttribute& operator=(NodeAttribute&& other) noexcept {
        if (this == &other) return *this;
        tree_       = other.tree_;
        treeId_     = other.treeId_;
        storage_    = std::move(other.storage_);
        denseCount_ = other.denseCount_;
        other.storage_.template emplace<SparseMap_>();
        other.denseCount_ = 0;
        return *this;
    }

    // ── identity / introspection ──

    [[nodiscard]] TreeId      tree()    const noexcept { return treeId_; }
    [[nodiscard]] bool        isDense() const noexcept { return storage_.index() == 1; }
    [[nodiscard]] std::size_t size()    const noexcept {
        return isDense()
            ? denseCount_
            : std::get<SparseMap_>(storage_).size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // ── mutators ──

    void set(NodeId id, T value) {
        validateId_(id);
        if (isDense()) {
            auto& slot = std::get<DenseVec_>(storage_)[id.v];
            if (!slot.has_value()) ++denseCount_;
            slot = std::move(value);
            return;
        }
        auto& sparse = std::get<SparseMap_>(storage_);
        sparse.insert_or_assign(id, std::move(value));
        maybePromote_();
    }

    bool erase(NodeId id) {
        validateId_(id);
        if (isDense()) {
            auto& slot = std::get<DenseVec_>(storage_)[id.v];
            if (!slot.has_value()) return false;
            slot.reset();
            --denseCount_;
            return true;
        }
        return std::get<SparseMap_>(storage_).erase(id) > 0;
    }

    // Reset to sparse mode. Coverage after clear is unknown; starting dense
    // would waste memory if the next workload happens to be sparse.
    //
    // `noexcept` here is a fail-loud contract, not a standard guarantee:
    // emplacing an empty unordered_map can theoretically throw under OOM,
    // in which case `std::terminate` is the intended outcome (same posture
    // as `attrFatal` / `treeFatal`). The move ctor / move assign above
    // carry the same caveat (they reset the source via the same emplace).
    void clear() noexcept {
        storage_.template emplace<SparseMap_>();
        denseCount_ = 0;
    }

    void reserve(std::size_t n) {
        if (!isDense()) {
            std::get<SparseMap_>(storage_).reserve(n);
        }
        // Dense is already sized to nodeCount(); the hint is a no-op.
    }

    // ── lookups ──

    [[nodiscard]] bool has(NodeId id) const {
        validateId_(id);
        if (isDense()) return std::get<DenseVec_>(storage_)[id.v].has_value();
        return std::get<SparseMap_>(storage_).contains(id);
    }

    [[nodiscard]] T&       get(NodeId id)       { validateId_(id); return getImpl_(*this, id); }
    [[nodiscard]] T const& get(NodeId id) const { validateId_(id); return getImpl_(*this, id); }

    [[nodiscard]] T*       tryGet(NodeId id)       { validateId_(id); return tryGetImpl_(*this, id); }
    [[nodiscard]] T const* tryGet(NodeId id) const { validateId_(id); return tryGetImpl_(*this, id); }

    // ── iteration (unspecified order) ──

    template <bool IsConst>
    class Iterator_;
    using iterator       = Iterator_<false>;
    using const_iterator = Iterator_<true>;

    [[nodiscard]] iterator       begin();
    [[nodiscard]] iterator       end();
    [[nodiscard]] const_iterator begin()  const;
    [[nodiscard]] const_iterator end()    const;
    [[nodiscard]] const_iterator cbegin() const { return begin(); }
    [[nodiscard]] const_iterator cend()   const { return end(); }

private:
    using SparseMap_ = std::unordered_map<NodeId, T>;
    using DenseVec_  = std::vector<std::optional<T>>;

    void validateId_(NodeId id) const {
        if (!id.valid())
            detail::attr::attrFatal("invalid NodeId (NodeId{} sentinel)");
        if (id.v >= tree_->nodeCount())
            detail::attr::attrFatal("NodeId out of bounds for bound Tree");
        // Untagged NodeId (treeTag == 0) passes — preserves the existing
        // test ergonomics of literal `NodeId{N}`. A non-zero tag that
        // differs from the bound tree's id means the caller pulled this
        // NodeId from a different Tree; fail loud with both ids.
        if (id.treeTag != 0 && id.treeTag != treeId_.v)
            detail::attr::crossTreeFatal(treeId_.v, id.treeTag);
    }

    template <typename Self>
    static auto& getImpl_(Self& self, NodeId id) {
        if (self.isDense()) {
            auto& slot = std::get<DenseVec_>(self.storage_)[id.v];
            if (!slot.has_value())
                detail::attr::attrFatal("get: no value for NodeId");
            return *slot;
        }
        auto& sparse = std::get<SparseMap_>(self.storage_);
        auto it = sparse.find(id);
        if (it == sparse.end())
            detail::attr::attrFatal("get: no value for NodeId");
        return it->second;
    }

    template <typename Self>
    static auto* tryGetImpl_(Self& self, NodeId id) {
        if (self.isDense()) {
            auto& slot = std::get<DenseVec_>(self.storage_)[id.v];
            return slot.has_value() ? &*slot : nullptr;
        }
        auto& sparse = std::get<SparseMap_>(self.storage_);
        auto it = sparse.find(id);
        return it == sparse.end() ? nullptr : &it->second;
    }

    void maybePromote_() {
        const std::size_t nc = tree_->nodeCount();
        if (nc < detail::attr::kPromoteFloor) return;

        auto& sparse = std::get<SparseMap_>(storage_);
        if (sparse.size() * detail::attr::kPromoteCoverageDenom
            < nc * detail::attr::kPromoteCoverageNumer) return;

        const std::size_t count = sparse.size();
        DenseVec_ dense(nc);
        for (auto& [id, val] : sparse) {
            dense[id.v] = std::move(val);
        }
        storage_.template emplace<DenseVec_>(std::move(dense));
        denseCount_ = count;
    }

    Tree const*                          tree_;
    TreeId                               treeId_;
    std::variant<SparseMap_, DenseVec_>  storage_{};
    std::size_t                          denseCount_ = 0;
};

// ── Iterator definition ──────────────────────────────────────────────────

template <typename T>
template <bool IsConst>
class NodeAttribute<T>::Iterator_ {
public:
    using element_type      = std::conditional_t<IsConst, T const, T>;
    // value_type is by-value but its second member is a reference into live
    // storage. Consequence: `for (auto kv : attr) { kv.second.mutate(); }`
    // mutates the container — `kv` is a copy of the pair, but `kv.second`
    // aliases the underlying T. Structured bindings (`auto& [id, val]`)
    // behave as expected.
    using value_type        = std::pair<NodeId, element_type&>;
    using reference         = value_type;
    using pointer           = void;
    using difference_type   = std::ptrdiff_t;
    using iterator_category = std::input_iterator_tag;

    // Default-constructed iterators are only well-defined for comparison
    // against other default-constructed iterators (not against a live
    // begin/end). Provided to satisfy the std iterator concept.
    Iterator_() = default;

    [[nodiscard]] value_type operator*() const {
        if (std::holds_alternative<MapIter_>(state_)) {
            auto& it = std::get<MapIter_>(state_);
            return value_type{it->first, it->second};
        }
        auto const& [vec, idx] = std::get<DensePos_>(state_);
        // Tag the synthesized NodeId with the bound tree's id so cross-
        // tree validators trip when an iterator-produced id is handed
        // off to a foreign attribute. The sparse path already returns
        // whatever the user inserted, so equivalent fidelity falls out
        // of the storage in that mode.
        return value_type{NodeId{static_cast<std::uint32_t>(idx), treeTag_},
                          *(*vec)[idx]};
    }

    Iterator_& operator++() {
        if (std::holds_alternative<MapIter_>(state_)) {
            ++std::get<MapIter_>(state_);
            return *this;
        }
        auto& [vec, idx] = std::get<DensePos_>(state_);
        ++idx;
        while (idx < vec->size() && !(*vec)[idx].has_value()) ++idx;
        return *this;
    }

    Iterator_ operator++(int) { auto copy = *this; ++(*this); return copy; }

    bool operator==(Iterator_ const& other) const noexcept { return state_ == other.state_; }
    bool operator!=(Iterator_ const& other) const noexcept { return !(*this == other); }

private:
    friend class NodeAttribute<T>;

    using MapIter_ = std::conditional_t<IsConst,
        typename SparseMap_::const_iterator,
        typename SparseMap_::iterator>;
    using DenseVecPtr_ = std::conditional_t<IsConst, DenseVec_ const*, DenseVec_*>;
    using DensePos_    = std::pair<DenseVecPtr_, std::size_t>;

    explicit Iterator_(MapIter_ it) noexcept : state_(it) {}
    Iterator_(DenseVecPtr_ vec, std::size_t idx, std::uint32_t treeTag) noexcept
        : state_(DensePos_{vec, idx}), treeTag_(treeTag) {}

    std::variant<MapIter_, DensePos_> state_;
    std::uint32_t                     treeTag_ = 0;
};

template <typename T>
auto NodeAttribute<T>::begin() -> iterator {
    if (isDense()) {
        auto& vec = std::get<DenseVec_>(storage_);
        std::size_t idx = 0;
        while (idx < vec.size() && !vec[idx].has_value()) ++idx;
        return iterator{&vec, idx, treeId_.v};
    }
    return iterator{std::get<SparseMap_>(storage_).begin()};
}

template <typename T>
auto NodeAttribute<T>::end() -> iterator {
    if (isDense()) {
        auto& vec = std::get<DenseVec_>(storage_);
        return iterator{&vec, vec.size(), treeId_.v};
    }
    return iterator{std::get<SparseMap_>(storage_).end()};
}

template <typename T>
auto NodeAttribute<T>::begin() const -> const_iterator {
    if (isDense()) {
        auto const& vec = std::get<DenseVec_>(storage_);
        std::size_t idx = 0;
        while (idx < vec.size() && !vec[idx].has_value()) ++idx;
        return const_iterator{&vec, idx, treeId_.v};
    }
    return const_iterator{std::get<SparseMap_>(storage_).begin()};
}

template <typename T>
auto NodeAttribute<T>::end() const -> const_iterator {
    if (isDense()) {
        auto const& vec = std::get<DenseVec_>(storage_);
        return const_iterator{&vec, vec.size(), treeId_.v};
    }
    return const_iterator{std::get<SparseMap_>(storage_).end()};
}

} // namespace dss
