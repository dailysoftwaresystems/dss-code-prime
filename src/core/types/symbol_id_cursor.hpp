#pragma once

#include "core/types/object_format_kind.hpp"  // isWriterReservedSymbolIdValue, kFirstWriterReservedSymbolIdValue
#include "core/types/strong_ids.hpp"          // SymbolId

#include <cstddef>
#include <cstdint>
#include <limits>

// ★★ THE ONE PLACE A FRESH `SymbolId` IS MADE (P69, lane `cs`, fold f26d -- review-csf26c2 N9, F10's class). A
// `SymbolId` the program MINTS -- a new id, not a copy or a mapping of an existing one -- is unique in its id space.
// Every space's door asks this file for its next id, and nothing else in the tree constructs a `SymbolId` from
// arithmetic, a counter or a size: tests/core/test_symbol_id_mints_guard.cpp refuses every such form outside this
// file, with no exemption list.
//
// TWO KINDS OF SPACE, ONE SOURCE OF THE RULE:
//   * a COUNTING space (a MIR module and the LIR lowering's continuation of it, the MIR merge, the assembler's text,
//     an assembled module, the link merge's numbering, an object format's fresh records, HIR's synthesized symbols)
//     holds a `SymbolIdEnd`: one past the highest id the space holds or has handed out, GROW-ONLY. A door seeds it
//     with "the highest id held" and the next id is computed here -- one past it, saturating at the last value, so no
//     door spells `+ 1` and no seed wraps onto 0, the invalid id. A take steps over a value a format WRITER reserves
//     (`isWriterReservedSymbolIdValue`) and is the invalid id at the last value, which the door turns into its tier's
//     named refusal. Two cursors of one space are two views of ONE end, so the second continues the first.
//   * an INDEX space (the front end's table, an object's records, the HIR text's handles), where an id IS a
//     position: stepping over a reserved value would break id == index, so the space ENDS at the first one --
//     `symbolIdOfIndex`.
namespace dss {

class SymbolIdEnd {
public:
    constexpr SymbolIdEnd() noexcept = default;

    // Raise the end past `held` -- the id a holder of the space holds. Never lowers it.
    constexpr void keepClearOf(SymbolId held) noexcept { keepClearOfHighest(held.v); }
    // Raise the end past `highestHeld`, the highest id VALUE the space holds (0: none). Never lowers it.
    constexpr void keepClearOfHighest(std::uint32_t highestHeld) noexcept {
        std::uint32_t const past = highestHeld == kLast ? kLast : highestHeld + 1u;
        if (past > end_) end_ = past;
    }
    // Raise the end to another end of the same space (a rebuilt module's, a reader's). Never lowers it.
    constexpr void keepClearOf(SymbolIdEnd other) noexcept {
        if (other.end_ > end_) end_ = other.end_;
    }
    // A cursor over an end a door carries as a plain value (a module's `symbolIdEnd`): opened, asked, and its
    // value written back. An end below 1 (nothing) is 1.
    [[nodiscard]] static constexpr SymbolIdEnd startingAt(std::uint32_t end) noexcept {
        SymbolIdEnd e;
        if (end > e.end_) e.end_ = end;
        return e;
    }
    // One past the highest id the space holds or has handed out (1: nothing).
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return end_; }

    // The next id, the end moved past it: a writer-reserved value is stepped over; the invalid id when the space is
    // exhausted (minting the last value would leave the end nowhere to go). The DOOR names the refusal.
    [[nodiscard]] constexpr SymbolId take() noexcept {
        while (end_ != kLast && isWriterReservedSymbolIdValue(end_)) ++end_;
        if (end_ == kLast) return SymbolId{};
        return SymbolId{end_++};
    }

private:
    static constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t end_ = 1;
};

// THE INDEX RULE: the id of position `index` of an index space, or the invalid id where the space has none -- 0 (the
// invalid id itself) and every position from the first writer-reserved value on. The door names the refusal.
[[nodiscard]] constexpr SymbolId symbolIdOfIndex(std::size_t index) noexcept {
    if (index == 0u || index >= kFirstWriterReservedSymbolIdValue) return SymbolId{};
    return SymbolId{static_cast<std::uint32_t>(index)};
}

} // namespace dss
