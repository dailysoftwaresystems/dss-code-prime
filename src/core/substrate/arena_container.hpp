#pragma once

#include "core/substrate/arena_tag.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// `ArenaContainer<Pod, Id, Tag>` — an immutable dense arena of POD `Pod`s,
// indexed by strong id `Id`, identified by arena tag `Tag`. This is the engine
// `Tree` proved out, generalized so HIR / MIR / LIR / the CU symbol table reuse
// one arena + one cross-arena guard instead of copy-pasting four.
//
// `ArenaBuilder<Pod, Id, Tag>` is the mutable counterpart: append nodes, get
// tagged ids, then `finish() && → ArenaContainer` (move-only build → frozen
// container, mirroring TreeBuilder → Tree).
//
// Conventions (preserved from Tree):
//   - Slot 0 is the reserved sentinel; real ids start at 1. The builder
//     emplaces it at construction.
//   - Every emitted `Id` carries the arena's `Tag.v` in `Id.arenaTag`; access
//     validates it (untagged `Id{N}` literals pass — test ergonomics).
//   - `Id` must expose `.v` (uint32 index), `.arenaTag` (uint32 provenance,
//     0 == untagged), `.valid()`, and a two-arg `(v, tag)` ctor.
//   - `Tag` must expose `.v` (uint32).
// Fatal-message wording comes from `ArenaNames<Id, Tag>`.

namespace dss::substrate {

template <class Pod, ArenaId Id, ArenaTag Tag>
class ArenaContainer {
public:
    using PodType = Pod;
    using IdType  = Id;
    using TagType = Tag;

    // The empty arena (no nodes, invalid tag) — the transient state before a
    // builder hands over, and a legitimate value in its own right.
    ArenaContainer() noexcept = default;

    ArenaContainer(std::vector<Pod>&& nodes, Tag tag) noexcept
        : nodes_(std::move(nodes)), tag_(tag) {}

    ArenaContainer(ArenaContainer const&)            = delete;
    ArenaContainer& operator=(ArenaContainer const&) = delete;
    ArenaContainer(ArenaContainer&&) noexcept        = default;
    ArenaContainer& operator=(ArenaContainer&&) noexcept = default;

    // The arena's identity tag (NOT an element id) — what every emitted Id's
    // `.arenaTag` is matched against.
    [[nodiscard]] Tag         id()        const noexcept { return tag_; }
    [[nodiscard]] std::size_t nodeCount() const noexcept { return nodes_.size(); }
    [[nodiscard]] bool        empty()     const noexcept { return nodes_.empty(); }

    // Bounds- + cross-arena-checked element access. Aborts (release-fatal) on
    // an out-of-range or foreign-arena id — never UB.
    [[nodiscard]] Pod const& at(Id id) const {
        detail::arena::validateElement<ArenaNames<Id, Tag>>(id, tag_, nodes_.size());
        return nodes_[id.v];
    }

private:
    std::vector<Pod> nodes_;
    Tag              tag_;
};

template <class Pod, ArenaId Id, ArenaTag Tag>
class ArenaBuilder {
public:
    using PodType    = Pod;
    using IdType     = Id;
    using TagType    = Tag;
    using Container  = ArenaContainer<Pod, Id, Tag>;

    // The tag is minted by the caller (preserving domain-specific allocators
    // like TreeBuilder::nextTreeId) and stamped onto every emitted id.
    explicit ArenaBuilder(Tag tag) : tag_(tag) {
        nodes_.emplace_back();  // slot 0 — the reserved sentinel.
    }

    ArenaBuilder(ArenaBuilder const&)            = delete;
    ArenaBuilder& operator=(ArenaBuilder const&) = delete;
    ArenaBuilder(ArenaBuilder&&) noexcept        = default;
    ArenaBuilder& operator=(ArenaBuilder&&) noexcept = default;

    [[nodiscard]] Tag         id()   const noexcept { return tag_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    // Append a node; return its tagged id.
    Id addNode(Pod pod) {
        const auto value = static_cast<std::uint32_t>(nodes_.size());
        nodes_.push_back(std::move(pod));
        return Id{value, tag_.v};
    }

    // Mutable / const element access during the build (e.g. back-patching a
    // parent link or rolling up a span). Same bounds + tag guard as the frozen
    // container.
    [[nodiscard]] Pod& at(Id id) {
        detail::arena::validateElement<ArenaNames<Id, Tag>>(id, tag_, nodes_.size());
        return nodes_[id.v];
    }
    [[nodiscard]] Pod const& at(Id id) const {
        detail::arena::validateElement<ArenaNames<Id, Tag>>(id, tag_, nodes_.size());
        return nodes_[id.v];
    }

    // Truncate the arena back to `n` slots — the zero-copy primitive behind
    // speculative-checkpoint rollback (mirrors the old `nodes_.resize`). `n`
    // must keep the slot-0 sentinel and may only shrink: a truncate that would
    // drop the sentinel or grow the arena (minting untagged default nodes) is a
    // caller bug, not silent.
    void truncateTo(std::size_t n) noexcept {
        if (n < 1 || n > nodes_.size()) {
            detail::arena::truncateOutOfRange(ArenaNames<Id, Tag>::access);
        }
        nodes_.resize(n);
    }

    // Freeze. Single-use; the builder is consumed.
    [[nodiscard]] Container finish() && {
        return Container{std::move(nodes_), tag_};
    }

private:
    std::vector<Pod> nodes_;
    Tag              tag_;
};

} // namespace dss::substrate
