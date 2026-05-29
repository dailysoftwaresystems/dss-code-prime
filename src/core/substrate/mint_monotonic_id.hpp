#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

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
// Prior to this substrate each consumer (`mintLirModuleId`,
// `mintTargetSchemaId`, `HirBuilder::nextModuleId`, `MirBuilder::
// nextModuleId`, `nextBufferIdValue`, `CompilationUnit::nextId`,
// `TreeBuilder::nextTreeId`, `grammar_schema_json` inline) reinvented
// the same `static std::atomic<std::uint32_t>` counter — eight near-
// identical bodies with subtly different overflow behaviour (some
// asserted, most silently wrapped). One template instantiation per
// strong-id type produces one process-wide counter with uniform
// behaviour.

namespace dss::substrate {

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
template <class StrongId>
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
