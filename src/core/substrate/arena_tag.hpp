#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>

// Cross-arena guard primitive shared by ArenaContainer (element access) and
// ArenaAttribute (side-table access). Both stamp a per-arena tag onto every id
// and validate it on every access — an id from arena A used against arena B
// aborts loudly. This is the generalization of Tree's SH3 `arenaTag` discipline.
//
// Fatal-message wording is driven by `ArenaNames<Id, Tag>` so each domain
// reproduces its own diagnostics. The primary template is a `static_assert`
// tripwire: every arena MUST specialize it (e.g. `tree.hpp` for <NodeId,
// TreeId>, `type_id.hpp` for <TypeId, CompilationUnitId>) so a new arena that
// forgets to declare its names fails at compile time rather than silently
// emitting generic-named diagnostics.

namespace dss::substrate {

namespace detail::arena {
template <class> inline constexpr bool kArenaNamesUnspecialized = false;
}

template <class Id, class Tag>
struct ArenaNames {
    static_assert(detail::arena::kArenaNamesUnspecialized<Id>,
                  "ArenaNames<Id, Tag> must be specialized for each arena: declare "
                  "its diagnostic entity names {attribute, element, tag, access}. "
                  "See the <NodeId, TreeId> specialization in tree.hpp.");
};

// ── arena type contracts ──
//
// An arena-element id: an index `.v`, a provenance tag `.arenaTag` (0 ==
// untagged sentinel), a `valid()` predicate, default + 1-arg + 2-arg `(v, tag)`
// construction, and `.v`-only equality. This is exactly what DSS_ARENA_ID
// mints; the concept makes a mis-shaped id fail at the instantiation boundary
// rather than deep inside a template.
template <class Id>
concept ArenaId =
    std::regular<Id> &&   // default-constructible + copyable + ==  (the value-type basics)
    requires(Id id, std::uint32_t value) {
        { id.v }        -> std::convertible_to<std::uint32_t>;
        { id.arenaTag } -> std::convertible_to<std::uint32_t>;
        { id.valid() }  -> std::convertible_to<bool>;
        Id{value};
        Id{value, value};
    };

// An arena identity tag: just an index `.v` (what every id's `.arenaTag` is
// matched against).
template <class Tag>
concept ArenaTag = requires(Tag tag) {
    { tag.v } -> std::convertible_to<std::uint32_t>;
};

// An arena an ArenaAttribute can bind to: exposes its element/tag id types, its
// own tag via `id()`, a `nodeCount()`, and a hashable id (the attribute keys a
// sparse map by it).
template <class A>
concept Arena =
    requires { typename A::IdType; typename A::TagType; } &&
    ArenaId<typename A::IdType> &&
    ArenaTag<typename A::TagType> &&
    requires(A const& arena, typename A::IdType id) {
        { arena.id() }        -> std::same_as<typename A::TagType>;
        { arena.nodeCount() } -> std::convertible_to<std::size_t>;
        std::hash<typename A::IdType>{}(id);
    };

namespace detail::arena {

// ── ArenaContainer element-access fatals ──

[[noreturn]] inline void outOfRange(char const* access, char const* element) {
    std::fprintf(stderr, "dss::substrate fatal: %s: %s out of range\n", access, element);
    std::abort();
}

[[noreturn]] inline void crossArenaAccess(char const* access, char const* element, char const* tag,
                                          std::uint32_t idTag, std::uint32_t arenaTag) {
    std::fprintf(stderr, "dss::substrate fatal: %s: %s from %s=%u used on %s=%u\n",
                 access, element, tag, idTag, tag, arenaTag);
    std::abort();
}

// ── ArenaAttribute side-table fatals ──

[[noreturn]] inline void attrInvalidSentinel(char const* attribute, char const* element) {
    std::fprintf(stderr, "dss::%s fatal: invalid %s (%s{} sentinel)\n", attribute, element, element);
    std::abort();
}

[[noreturn]] inline void attrOutOfBounds(char const* attribute, char const* element) {
    std::fprintf(stderr, "dss::%s fatal: %s out of bounds for bound arena\n", attribute, element);
    std::abort();
}

[[noreturn]] inline void attrNoValue(char const* attribute, char const* element) {
    std::fprintf(stderr, "dss::%s fatal: get: no value for %s\n", attribute, element);
    std::abort();
}

// Emits both tags so death-test regexes can pin both numbers.
[[noreturn]] inline void attrCrossArena(char const* attribute, char const* element, char const* tag,
                                        std::uint32_t boundTag, std::uint32_t idTag) {
    std::fprintf(stderr, "dss::%s fatal: %s bound to %s=%u got %s from %s=%u\n",
                 attribute, attribute, tag, boundTag, element, tag, idTag);
    std::abort();
}

[[noreturn]] inline void truncateOutOfRange(char const* access) {
    std::fprintf(stderr, "dss::substrate fatal: %s: truncateTo size out of range\n", access);
    std::abort();
}

// The element-access guard shared by ArenaContainer::at and ArenaBuilder::at:
// bounds first, then the cross-arena tag check (untagged ids — arenaTag == 0 —
// pass, preserving literal-id test ergonomics). `Names` supplies the wording.
template <class Names, class Id, class Tag>
inline void validateElement(Id id, Tag tag, std::size_t count) {
    if (!id.valid() || id.v >= count) {
        outOfRange(Names::access, Names::element);
    }
    if (id.arenaTag != 0 && id.arenaTag != tag.v) {
        crossArenaAccess(Names::access, Names::element, Names::tag, id.arenaTag, tag.v);
    }
}

} // namespace detail::arena

} // namespace dss::substrate
