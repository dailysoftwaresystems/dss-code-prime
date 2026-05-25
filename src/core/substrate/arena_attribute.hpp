#pragma once

#include "core/substrate/arena_tag.hpp"

#include <cstddef>
#include <iterator>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

// `ArenaAttribute<ArenaT, T>` — a typed side-table mapping an arena's id → T,
// bound to one arena. This is the ONLY way later passes annotate an immutable
// arena (tree, HIR, MIR, …) without mutating it. `NodeAttribute<T>` is the
// `ArenaT = Tree` alias (see tree_attrs.hpp).
//
// Storage auto-promotes from sparse (unordered_map) to dense
// (vector<optional<T>> indexed by id.v) once a coverage threshold is crossed.
// The choice is internal; the public API is identical in either mode.
//
// `ArenaT` requirements (duck-typed): a nested `IdType` (the element id, with
// `.v` / `.treeTag` / `.valid()` and a two-arg `(v, tag)` ctor + std::hash) and
// `TagType` (the arena's identity tag, with `.v`); plus `id() -> TagType` and
// `nodeCount() -> std::size_t`. Fatal-message wording comes from
// `ArenaNames<IdType, TagType>`.
//
// Lifetime: an ArenaAttribute holds a raw `ArenaT const*` for nodeCount-based
// bounds checks, so the caller MUST NOT let the bound arena outlive the
// attribute. Same rule as TreeCursor.

namespace dss::substrate {

namespace detail::attr {

// Sparse → dense once coverage reaches kPromoteCoverageNumer /
// kPromoteCoverageDenom (50%). Below kPromoteFloor nodes, stay sparse —
// dense's per-slot std::optional<T> overhead dominates on tiny arenas.
inline constexpr std::size_t kPromoteCoverageNumer = 1;
inline constexpr std::size_t kPromoteCoverageDenom = 2;
inline constexpr std::size_t kPromoteFloor         = 16;

} // namespace detail::attr

template <class ArenaT, class T>
class ArenaAttribute {
public:
    static_assert(std::is_move_constructible_v<T>,
                  "ArenaAttribute<ArenaT, T>: T must be move-constructible");

    using value_type = T;
    using IdType     = typename ArenaT::IdType;
    using TagType    = typename ArenaT::TagType;

    explicit ArenaAttribute(ArenaT const& arena) noexcept
        : arena_(&arena), tag_(arena.id()) {}

    ArenaAttribute(ArenaAttribute const&)            = delete;
    ArenaAttribute& operator=(ArenaAttribute const&) = delete;

    // Custom move ops reset the source's `denseCount_` AND its variant back
    // to an empty SparseMap_ so the moved-from object's observable state
    // (size(), empty(), isDense(), iteration) is internally consistent
    // rather than relying on the std-library's "valid but unspecified"
    // contract for the underlying map / vector.
    ArenaAttribute(ArenaAttribute&& other) noexcept
        : arena_(other.arena_)
        , tag_(other.tag_)
        , storage_(std::move(other.storage_))
        , denseCount_(other.denseCount_) {
        other.storage_.template emplace<SparseMap_>();
        other.denseCount_ = 0;
    }

    ArenaAttribute& operator=(ArenaAttribute&& other) noexcept {
        if (this == &other) return *this;
        arena_      = other.arena_;
        tag_        = other.tag_;
        storage_    = std::move(other.storage_);
        denseCount_ = other.denseCount_;
        other.storage_.template emplace<SparseMap_>();
        other.denseCount_ = 0;
        return *this;
    }

    // ── identity / introspection ──

    // The bound arena's identity tag. `tag()` is the canonical generic name;
    // `tree()` is the original Tree-lineage alias kept for existing callers
    // (UnitAttribute, the NodeAttribute tests).
    [[nodiscard]] TagType     tag()     const noexcept { return tag_; }
    [[nodiscard]] TagType     tree()    const noexcept { return tag_; }
    [[nodiscard]] bool        isDense() const noexcept { return storage_.index() == 1; }
    [[nodiscard]] std::size_t size()    const noexcept {
        return isDense()
            ? denseCount_
            : std::get<SparseMap_>(storage_).size();
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // ── mutators ──

    void set(IdType id, T value) {
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

    bool erase(IdType id) {
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
    // as the *Fatal helpers). The move ctor / move assign above carry the
    // same caveat (they reset the source via the same emplace).
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

    [[nodiscard]] bool has(IdType id) const {
        validateId_(id);
        if (isDense()) return std::get<DenseVec_>(storage_)[id.v].has_value();
        return std::get<SparseMap_>(storage_).contains(id);
    }

    [[nodiscard]] T&       get(IdType id)       { validateId_(id); return getImpl_(*this, id); }
    [[nodiscard]] T const& get(IdType id) const { validateId_(id); return getImpl_(*this, id); }

    [[nodiscard]] T*       tryGet(IdType id)       { validateId_(id); return tryGetImpl_(*this, id); }
    [[nodiscard]] T const* tryGet(IdType id) const { validateId_(id); return tryGetImpl_(*this, id); }

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
    using Names      = ArenaNames<IdType, TagType>;
    using SparseMap_ = std::unordered_map<IdType, T>;
    using DenseVec_  = std::vector<std::optional<T>>;

    void validateId_(IdType id) const {
        if (!id.valid())
            detail::arena::attrInvalidSentinel(Names::attribute, Names::element);
        if (id.v >= arena_->nodeCount())
            detail::arena::attrOutOfBounds(Names::attribute, Names::element);
        // Untagged id (treeTag == 0) passes — preserves the existing test
        // ergonomics of literal `IdType{N}`. A non-zero tag that differs from
        // the bound arena's tag means the caller pulled this id from a
        // different arena; fail loud with both tags.
        if (id.treeTag != 0 && id.treeTag != tag_.v)
            detail::arena::attrCrossArena(Names::attribute, Names::element, Names::tag,
                                          tag_.v, id.treeTag);
    }

    template <typename Self>
    static auto& getImpl_(Self& self, IdType id) {
        if (self.isDense()) {
            auto& slot = std::get<DenseVec_>(self.storage_)[id.v];
            if (!slot.has_value())
                detail::arena::attrNoValue(Names::attribute, Names::element);
            return *slot;
        }
        auto& sparse = std::get<SparseMap_>(self.storage_);
        auto it = sparse.find(id);
        if (it == sparse.end())
            detail::arena::attrNoValue(Names::attribute, Names::element);
        return it->second;
    }

    template <typename Self>
    static auto* tryGetImpl_(Self& self, IdType id) {
        if (self.isDense()) {
            auto& slot = std::get<DenseVec_>(self.storage_)[id.v];
            return slot.has_value() ? &*slot : nullptr;
        }
        auto& sparse = std::get<SparseMap_>(self.storage_);
        auto it = sparse.find(id);
        return it == sparse.end() ? nullptr : &it->second;
    }

    void maybePromote_() {
        const std::size_t nc = arena_->nodeCount();
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

    ArenaT const*                        arena_;
    TagType                              tag_;
    std::variant<SparseMap_, DenseVec_>  storage_{};
    std::size_t                          denseCount_ = 0;
};

// ── Iterator definition ──────────────────────────────────────────────────

template <class ArenaT, class T>
template <bool IsConst>
class ArenaAttribute<ArenaT, T>::Iterator_ {
public:
    using element_type      = std::conditional_t<IsConst, T const, T>;
    // value_type is by-value but its second member is a reference into live
    // storage. Structured bindings (`auto& [id, val]`) behave as expected.
    using value_type        = std::pair<IdType, element_type&>;
    using reference         = value_type;
    using pointer           = void;
    using difference_type   = std::ptrdiff_t;
    using iterator_category = std::input_iterator_tag;

    Iterator_() = default;

    [[nodiscard]] value_type operator*() const {
        if (std::holds_alternative<MapIter_>(state_)) {
            auto& it = std::get<MapIter_>(state_);
            return value_type{it->first, it->second};
        }
        auto const& [vec, idx] = std::get<DensePos_>(state_);
        // Tag the synthesized id with the bound arena's tag so cross-arena
        // validators trip when an iterator-produced id is handed off to a
        // foreign attribute. The sparse path returns whatever the user
        // inserted, so equivalent fidelity falls out of the storage there.
        return value_type{IdType{static_cast<std::uint32_t>(idx), tag_},
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
    friend class ArenaAttribute<ArenaT, T>;

    using MapIter_ = std::conditional_t<IsConst,
        typename SparseMap_::const_iterator,
        typename SparseMap_::iterator>;
    using DenseVecPtr_ = std::conditional_t<IsConst, DenseVec_ const*, DenseVec_*>;
    using DensePos_    = std::pair<DenseVecPtr_, std::size_t>;

    explicit Iterator_(MapIter_ it) noexcept : state_(it) {}
    Iterator_(DenseVecPtr_ vec, std::size_t idx, std::uint32_t tag) noexcept
        : state_(DensePos_{vec, idx}), tag_(tag) {}

    std::variant<MapIter_, DensePos_> state_;
    std::uint32_t                     tag_ = 0;
};

template <class ArenaT, class T>
auto ArenaAttribute<ArenaT, T>::begin() -> iterator {
    if (isDense()) {
        auto& vec = std::get<DenseVec_>(storage_);
        std::size_t idx = 0;
        while (idx < vec.size() && !vec[idx].has_value()) ++idx;
        return iterator{&vec, idx, tag_.v};
    }
    return iterator{std::get<SparseMap_>(storage_).begin()};
}

template <class ArenaT, class T>
auto ArenaAttribute<ArenaT, T>::end() -> iterator {
    if (isDense()) {
        auto& vec = std::get<DenseVec_>(storage_);
        return iterator{&vec, vec.size(), tag_.v};
    }
    return iterator{std::get<SparseMap_>(storage_).end()};
}

template <class ArenaT, class T>
auto ArenaAttribute<ArenaT, T>::begin() const -> const_iterator {
    if (isDense()) {
        auto const& vec = std::get<DenseVec_>(storage_);
        std::size_t idx = 0;
        while (idx < vec.size() && !vec[idx].has_value()) ++idx;
        return const_iterator{&vec, idx, tag_.v};
    }
    return const_iterator{std::get<SparseMap_>(storage_).begin()};
}

template <class ArenaT, class T>
auto ArenaAttribute<ArenaT, T>::end() const -> const_iterator {
    if (isDense()) {
        auto const& vec = std::get<DenseVec_>(storage_);
        return const_iterator{&vec, vec.size(), tag_.v};
    }
    return const_iterator{std::get<SparseMap_>(storage_).end()};
}

} // namespace dss::substrate
