#pragma once

// ── The per-function OLD→NEW id remap (D-PERF-OPT-REBUILD-REMAP-IS-A-HASH-MAP) ──
//
// Every MIR-tier pass rebuilds the module by walking a read-only `Mir` and
// re-emitting it into a fresh `MirBuilder`, translating each OLD arena id to the
// NEW one the builder just minted. That translation used to be a
// `std::unordered_map<std::uint32_t, IdT>` per function, and ✔MEASURED on the
// merged 103-TU SQLite module it was the dominant cost of the rebuild half:
// ~700 000 instructions, roughly FOUR hash operations each (one emplace for the
// result, one lookup per operand), at ~120-200 ns per instruction on a rebuild
// that moves a couple of POD words.
//
// ★★★ THE MAP WAS NEVER NEEDED, BECAUSE THE KEYS ARE A DENSE CONTIGUOUS RANGE.
// `MirBuilder::addFunction` calls `closeFunction_()` before opening the next
// function, so a function's blocks occupy one contiguous ascending slot range
// and its instructions occupy one contiguous slot range. A hash table keyed on a
// dense integer range is a vector wearing a costume: this type is that vector,
// indexed by `oldId.v - base`, with `IdT{}` (the arena's slot-0 sentinel, which
// `valid()` reports false) as the ABSENT marker.
//
// ★★ THE SIZING RULE IS LOAD-BEARING AND HAS A SCAR. `reset` sizes to THE
// FUNCTION, never to the module. The predecessor of this type was a map that
// reserved `src_.instCount()` — the whole module — once per function, which made
// the rebuild quadratic in the module and cost seconds
// (D-OPT-REBUILD-REWRITE-MAP-RESERVES-THE-WHOLE-MODULE). A `reset` sized to the
// module would re-introduce that defect in a form no allocator profile would
// name, because a `vector::assign` over megabytes looks like honest work.
//
// ★ AND WHY OUT-OF-RANGE IS NOT SYMMETRIC. A QUERY for an id outside this
// function's range is legitimate and answers ABSENT — it is exactly what the
// hash map answered, and passes rely on it (Dce asks whether a phi predecessor
// survived; Licm's loop forest can name a self-looping block belonging to
// ANOTHER function, per D-OPT-LICM-NATURAL-LOOPS-MODULE-WIDE-SCAN, and must get
// "no" rather than a crash). A WRITE outside the range is a substrate-contract
// violation — it means the rebuild believes a foreign id belongs to the function
// it is emitting — so `put` fails loud rather than silently growing. The hash
// map could not tell those two cases apart; this type must, and does.
//
// AGNOSTIC: pure id arithmetic. No language, target, or object-format state.

#include "core/export.hpp"
#include "core/substrate/arena_tag.hpp"
#include "core/types/strong_ids.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace dss::opt::passes {

// Fails loud on a WRITE outside the function's slot range. Out-of-line so the
// header stays free of <format> and the hot paths below stay inlineable.
[[noreturn]] DSS_EXPORT void
mirIdRemapOutOfRange(std::string_view which, std::uint32_t oldV,
                     std::uint32_t base, std::uint32_t extent);

// Fails loud on a checked read of an ABSENT slot (the hash map's throwing
// `.at()`, with the map named).
[[noreturn]] DSS_EXPORT void
mirIdRemapAbsent(std::string_view which, std::uint32_t oldV);

template <substrate::ArenaId IdT>
class MirIdRemap {
public:
    // Point the remap at OLD slot range [base, base + extent) and clear every
    // slot to ABSENT. O(extent) — the FUNCTION's size, never the module's.
    void reset(std::uint32_t base, std::uint32_t extent, std::string_view which) {
        base_   = base;
        which_  = which;
        live_   = 0;
        slots_.assign(extent, IdT{});
    }

    // Record OLD slot `oldV` → NEW id. Fails loud outside the range.
    void put(std::uint32_t oldV, IdT newId) {
        std::uint32_t const idx = oldV - base_;
        if (oldV < base_ || idx >= slots_.size()) {
            mirIdRemapOutOfRange(which_, oldV, base_,
                                 static_cast<std::uint32_t>(slots_.size()));
        }
        if (!slots_[idx].valid()) ++live_;
        slots_[idx] = newId;
    }

    // ABSENT → nullptr, for an in-range-but-unmapped slot AND for any id outside
    // the range (see the header note — a query is not a contract violation).
    [[nodiscard]] IdT const* find(std::uint32_t oldV) const noexcept {
        std::uint32_t const idx = oldV - base_;
        if (oldV < base_ || idx >= slots_.size()) return nullptr;
        return slots_[idx].valid() ? &slots_[idx] : nullptr;
    }

    [[nodiscard]] bool contains(std::uint32_t oldV) const noexcept {
        return find(oldV) != nullptr;
    }

    // The checked read. Fails loud on ABSENT, which is what the hash map's
    // `.at()` did by throwing — except this names the map and the slot, and
    // these call sites are inside a rebuild that must never continue past a
    // missing translation (D-OPT2-REWRITE-MAP-COMPLETENESS).
    [[nodiscard]] IdT at(std::uint32_t oldV) const {
        if (IdT const* p = find(oldV)) return *p;
        mirIdRemapAbsent(which_, oldV);
    }

    // Live entries — the hash map's `size()`, preserved for the pass counters
    // that read it.
    [[nodiscard]] std::size_t size() const noexcept { return live_; }

private:
    std::uint32_t    base_  = 0;
    std::size_t      live_  = 0;
    std::vector<IdT> slots_;
    // Names the map in the fail-loud text ("rewrite" / "blockMap" / …). A fatal
    // is the one message a reader cannot follow up interactively, so it carries
    // its subject (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS).
    std::string_view which_ = "<unnamed>";
};

// The two instantiations the rebuild substrate uses. Named so the policy hook
// signatures read as intent rather than as a container choice.
using MirInstRemap  = MirIdRemap<MirInstId>;
using MirBlockRemap = MirIdRemap<MirBlockId>;

} // namespace dss::opt::passes
