#pragma once

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_attrs.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// `UnitAttribute<T>` — the CompilationUnit-scoped analog of NodeAttribute<T>.
// It maps a NodeId (drawn from ANY tree in the CU) to a T, spanning every tree
// the CU owns. This is the shape the semantic phase (#8) consumes for its
// symbol table: `UnitAttribute<SymbolId>` bound to one CU.
//
// Internally it holds one NodeAttribute<T> per tree and routes each NodeId to
// the right one via NodeId.arenaTag (the SH3 per-tree provenance). The extra
// guard CU3 adds over NodeAttribute is *membership*: a tagged NodeId whose
// arenaTag belongs to a tree in a DIFFERENT CompilationUnit (a cross-CU leak)
// is not in this CU's routing table and aborts loud — the per-tree guard alone
// can't catch this because TreeIds are globally unique, so a foreign id never
// matches any single bound tree but would otherwise just "miss". (The one
// provenance the guard CANNOT verify is an *untagged* literal NodeId in a
// single-tree CU: with arenaTag==0 there is nothing to check membership
// against, so it routes to the sole tree on trust — see route_.)
//
// This is the binding layer (NodeId -> T), NOT a reverse index: there is no
// `get(SymbolId)`. The semantic phase (#8) builds the SymbolId -> record table
// separately.
//
// Lifetime: like NodeAttribute (and TreeCursor), a UnitAttribute holds raw
// pointers into the CU's trees. The caller MUST keep the bound CU alive for
// the attribute's lifetime. A zero-tree (empty) CU yields a read-only-empty
// attribute: size()/empty()/forEach are valid (and trivially empty), but any
// NodeId-keyed mutation/lookup aborts in route_ (no tree to route to).

namespace dss {

namespace detail::unit_attr {

[[noreturn]] inline void unitAttrFatal(char const* what) {
    std::fputs("dss::UnitAttribute fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Cross-CU variant — emits the bound CompilationUnitId and the offending
// NodeId's source TreeId so a death-test regex can pin both numbers.
[[noreturn]] inline void crossUnitFatal(std::uint32_t boundCuId, std::uint32_t idTreeTag) {
    std::fputs("dss::UnitAttribute fatal: ", stderr);
    std::fprintf(stderr,
                 "UnitAttribute for CompilationUnitId=%u got NodeId from TreeId=%u (not in this unit)",
                 boundCuId, idTreeTag);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace detail::unit_attr

template <typename T>
class UnitAttribute {
public:
    static_assert(std::is_move_constructible_v<T>,
                  "UnitAttribute<T>: T must be move-constructible");

    using value_type = T;

    explicit UnitAttribute(CompilationUnit const& cu) : cuId_(cu.id()) {
        auto const trees = cu.trees();
        perTree_.reserve(trees.size());
        for (std::size_t index = 0; index < trees.size(); ++index) {
            perTree_.emplace_back(trees[index]);
            // A real CU's trees carry globally-unique TreeIds (TreeBuilder's
            // monotonic counter), so a collision here means corrupted input.
            if (!tagToIndex_.emplace(trees[index].id().v, index).second) {
                detail::unit_attr::unitAttrFatal(
                    "duplicate TreeId among the CompilationUnit's trees");
            }
        }
    }

    UnitAttribute(UnitAttribute const&)            = delete;
    UnitAttribute& operator=(UnitAttribute const&) = delete;
    // Defaulted moves are correct here (unlike NodeAttribute's hand-rolled
    // ones): moving the vector + map leaves the source observably empty, so
    // size()/empty()/forEach stay consistent on the moved-from object without
    // a custom reset.
    UnitAttribute(UnitAttribute&&)                 = default;
    UnitAttribute& operator=(UnitAttribute&&)      = default;

    // ── identity / introspection ──

    [[nodiscard]] CompilationUnitId unit() const noexcept { return cuId_; }

    [[nodiscard]] std::size_t size() const noexcept {
        std::size_t total = 0;
        for (auto const& attr : perTree_) total += attr.size();
        return total;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // ── mutators ──

    void set(NodeId id, T value)  { perTree_[route_(id)].set(id, std::move(value)); }
    bool erase(NodeId id)         { return perTree_[route_(id)].erase(id); }

    // ── lookups ──

    [[nodiscard]] bool     has(NodeId id)    const { return perTree_[route_(id)].has(id); }
    [[nodiscard]] T&       get(NodeId id)          { return perTree_[route_(id)].get(id); }
    [[nodiscard]] T const& get(NodeId id)    const { return perTree_[route_(id)].get(id); }
    [[nodiscard]] T*       tryGet(NodeId id)       { return perTree_[route_(id)].tryGet(id); }
    [[nodiscard]] T const* tryGet(NodeId id) const { return perTree_[route_(id)].tryGet(id); }

    // ── iteration ──
    //
    // Unspecified order. The callback receives (TreeId owningTree, NodeId,
    // T const&). Cross-tree aggregate iteration — each per-tree NodeAttribute
    // yields its own tagged ids, so a forwarded id stays distinguishable.
    template <typename F>
    void forEach(F&& visit) const {
        for (auto const& attr : perTree_) {
            for (auto const& [id, value] : attr) {
                visit(attr.tree(), id, value);
            }
        }
    }

private:
    // Map a NodeId to the index of its owning tree's NodeAttribute, enforcing
    // CU membership. An untagged literal (arenaTag == 0) is only routable when
    // the CU has exactly one tree; it is rejected as ambiguous in a multi-tree
    // CU and (since size() != 1) in a zero-tree CU. The per-tree NodeAttribute
    // still applies its own sentinel / bounds / tag checks once routed.
    [[nodiscard]] std::size_t route_(NodeId id) const {
        if (id.arenaTag == 0) {
            if (perTree_.size() == 1) return 0;
            detail::unit_attr::unitAttrFatal(
                "untagged NodeId (arenaTag==0) is ambiguous in a multi-tree "
                "(or empty) CompilationUnit");
        }
        auto const it = tagToIndex_.find(id.arenaTag);
        if (it == tagToIndex_.end()) {
            detail::unit_attr::crossUnitFatal(cuId_.v, id.arenaTag);
        }
        return it->second;
    }

    CompilationUnitId                              cuId_;
    std::vector<NodeAttribute<T>>                  perTree_;     // parallel to cu.trees()
    std::unordered_map<std::uint32_t, std::size_t> tagToIndex_;  // TreeId.v → perTree_ index
};

} // namespace dss
