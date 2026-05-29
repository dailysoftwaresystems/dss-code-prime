#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

// Shared monotonic-id minter for the family of DSS strong-id types
// (`SchemaId`, `TargetSchemaId`, `HirModuleId`, `MirModuleId`,
// `LirModuleId`, `CompilationUnitId`, `BufferId`, `TreeId`, ...).
//
// Each strong-id type that follows the `DSS_STRONG_ID(Name)` pattern
// (uint32 underlying field `v`, `v == 0` reserved as `Invalid<Name>`
// sentinel) shares the same minting discipline: a process-wide
// monotonic counter starting at 1, atomically incremented per emit,
// with an abort-on-overflow guard so a wrapped counter cannot mint
// `StrongId{0}` and silently appear as an Invalid sentinel.
//
// `DssStrongId` concept locks the contract at the type-system level:
// arena-element id types (which carry a second `arenaTag` field) are
// EXPLICITLY rejected — they have a different lifecycle (arena owns
// their counter) and minting one through this substrate would leave
// `arenaTag = 0` and quietly produce an "untagged" id that the cross-
// arena guard treats as a wildcard match.

namespace dss::substrate {

// True iff `T` looks like a `DSS_STRONG_ID(Name)` instantiation:
//   - sole 32-bit `v` field (sizeof == 4)
//   - default-constructible to the Invalid sentinel
//   - constructible from a single `std::uint32_t`
//   - has a `valid()` predicate (every DSS_STRONG_ID does)
// Arena-id types (`DSS_ARENA_ID`) have `sizeof == 8` (arenaTag adds a
// second uint32) and so are rejected by the size invariant.
template <class T>
concept DssStrongId =
    std::is_default_constructible_v<T> &&
    std::is_trivially_copyable_v<T> &&
    requires(T t, std::uint32_t v) {
        T{v};
        { t.v } -> std::same_as<std::uint32_t&>;
        { t.valid() } -> std::same_as<bool>;
    } &&
    sizeof(T) == sizeof(std::uint32_t);

// Mint a fresh strong id of type `StrongId`. Process-global; thread-
// safe via relaxed atomic increment (the ordering only needs to be
// monotonic per counter — strong ids are not memory-fence sync points).
//
// Aborts loud (writes to stderr + `std::abort`) on uint32 overflow so
// the wrap never re-emits the `Invalid` sentinel as a "fresh" id.
// Overflow is genuinely unreachable in production (every id type would
// need 4 billion mints in a single process); the guard exists so an
// unbounded test loop or a fuzzer can't silently corrupt cross-arena
// tagging.
template <DssStrongId StrongId>
[[nodiscard]] StrongId mintMonotonicId() noexcept {
    static std::atomic<std::uint32_t> counter{0};
    std::uint32_t const v = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    if (v == 0) [[unlikely]] {
        std::fputs(
            "dss: mintMonotonicId<StrongId> counter exhausted "
            "(uint32 overflow) — aborting before a wrapped id "
            "collides with the Invalid sentinel\n",
            stderr);
        std::abort();
    }
    return StrongId{v};
}

} // namespace dss::substrate
