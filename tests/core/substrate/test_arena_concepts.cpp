// SP1 follow-up: the arena type concepts (ArenaId / ArenaTag / Arena) and the
// ArenaNames must-specialize tripwire. These are compile-time contracts, so the
// strongest test is a battery of static_asserts — both that the real id/tag/
// arena types SATISFY the concepts and that deliberately mis-shaped types are
// REJECTED (the negative direction is what protects the concepts' discriminating
// power: a future edit that loosened ArenaId would otherwise pass unnoticed).

#include "core/substrate/arena_attribute.hpp"
#include "core/substrate/arena_container.hpp"
#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"                     // ArenaNames<NodeId, TreeId>, Arena<Tree>
#include "core/types/type_lattice/type_id.hpp"     // ArenaNames<TypeId, CompilationUnitId>

#include "substrate/arena_test_types.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace {

using namespace dss::substrate;
using dss::substrate_test::ShapeId;
using dss::substrate_test::ShapePod;
using dss::substrate_test::ShapeTag;
using dss::substrate_test::ValueId;
using dss::substrate_test::ValuePod;
using dss::substrate_test::ValueTag;

using ShapeArena = ArenaContainer<ShapePod, ShapeId, ShapeTag>;
using ValueArena = ArenaContainer<ValuePod, ValueId, ValueTag>;

// ── ArenaId: satisfied by every real and test element id ──
static_assert(ArenaId<dss::NodeId>);
static_assert(ArenaId<dss::TypeId>);
static_assert(ArenaId<ShapeId>);
static_assert(ArenaId<ValueId>);

// ArenaId rejects a plain strong id — it has `.v` but no `.arenaTag` and no
// two-arg `(v, tag)` ctor, so it can't carry provenance and must NOT qualify.
static_assert(!ArenaId<dss::RuleId>);
static_assert(!ArenaId<dss::TreeId>);

// Mis-shaped ids: each drops exactly one requirement.
struct NoArenaTag {                                  // missing `.arenaTag`
    std::uint32_t v = 0;
    constexpr NoArenaTag() noexcept = default;
    constexpr explicit NoArenaTag(std::uint32_t value) noexcept : v(value) {}
    constexpr NoArenaTag(std::uint32_t value, std::uint32_t) noexcept : v(value) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(NoArenaTag const&) const noexcept = default;
};
struct NoTwoArgCtor {                                // missing the (v, tag) ctor
    std::uint32_t v = 0, arenaTag = 0;
    constexpr NoTwoArgCtor() noexcept = default;
    constexpr explicit NoTwoArgCtor(std::uint32_t value) noexcept : v(value) {}
    [[nodiscard]] constexpr bool valid() const noexcept { return v != 0; }
    constexpr bool operator==(NoTwoArgCtor const&) const noexcept = default;
};
static_assert(!ArenaId<NoArenaTag>);
static_assert(!ArenaId<NoTwoArgCtor>);

// ── ArenaTag: just `.v`. The real tags qualify ──
static_assert(ArenaTag<dss::TreeId>);
static_assert(ArenaTag<dss::CompilationUnitId>);
static_assert(ArenaTag<ShapeTag>);
static_assert(ArenaTag<ValueTag>);
// An element id also exposes `.v`, so ArenaTag is intentionally satisfied by it
// too — the concept is a shape floor, not a tag-vs-id discriminator. Pinned so
// the looseness is documented, not discovered.
static_assert(ArenaTag<dss::NodeId>);

// ── Arena: the bindable-arena contract ArenaAttribute consumes ──
static_assert(Arena<dss::Tree>);
static_assert(Arena<ShapeArena>);
static_assert(Arena<ValueArena>);

// An arena missing `id()` / `nodeCount()` must NOT qualify even with the nested
// id/tag aliases present.
struct NotAnArena {
    using IdType  = ShapeId;
    using TagType = ShapeTag;
};
static_assert(!Arena<NotAnArena>);
static_assert(!Arena<dss::NodeId>);   // an id is not an arena

// ── ArenaNames specializations exist and carry the expected wording ──
// A specialization deleted or renamed would fail HERE (a localized test) rather
// than only surfacing if that arena's death path happened to run. The <NodeId,
// TreeId> and <TypeId, CompilationUnitId> wording is otherwise pinned only by
// the Tree / type-interner suites, never by this substrate suite at runtime.
static_assert(std::string_view{ArenaNames<dss::NodeId, dss::TreeId>::element} == "NodeId");
static_assert(std::string_view{ArenaNames<dss::NodeId, dss::TreeId>::tag} == "TreeId");
static_assert(std::string_view{ArenaNames<dss::TypeId, dss::CompilationUnitId>::element} == "TypeId");
static_assert(std::string_view{ArenaNames<dss::TypeId, dss::CompilationUnitId>::tag} == "CompilationUnitId");
static_assert(std::string_view{ArenaNames<ShapeId, ShapeTag>::attribute} == "ShapeAttr");
static_assert(std::string_view{ArenaNames<ValueId, ValueTag>::access} == "ValueArena::at");

} // namespace

// The whole point of this file is the static_asserts above (checked at compile
// time). A trivial runtime case keeps gtest's main happy and proves the TU links.
TEST(ArenaConcepts, CompileTimeContractsHold) {
    SUCCEED() << "arena concept + ArenaNames static_asserts compiled";
}
