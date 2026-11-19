#pragma once

#include "core/export.hpp"                    // DSS_EXPORT
#include "core/types/object_format_kind.hpp"  // isWriterReservedSymbolIdValue, kFirstWriterReservedSymbolIdValue
#include "core/types/strong_ids.hpp"          // SymbolId

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// ★★★ THE ONE HOME OF A FRESH `SymbolId` (P69, lane `cs`: fold f26d -- review-csf26c2 N9; fold f26e -- review-csf26d
// B1, its home audit v2 and main's M1/M2). A `SymbolId` the program MINTS -- a new id, not a copy or a mapping of an
// existing one -- is unique in its id space, and WHETHER AN OUTPUT MAY CARRY IT IS DECIDED BY THE DOOR THAT TOOK IT: a
// door that cannot hand out an id reports one named, delivery-guaranteed refusal at the take and never hands out the
// invalid id unreported; an output exists only through the door's SEAL (`ReportedSymbolIds`), a test of the take's
// VALUE (`OptionalSymbolIds`, `IndexSymbolIds::next`), or not at all (`AbortingSymbolIds`,
// `IndexSymbolIds::nextOrAbort`).
//
// ★ THE FORM IS A TYPE. The cursor's take (`SymbolIdEnd::take`) is PRIVATE and the index rule lives only inside
// `IndexSymbolIds`: a source of fresh ids that states no form does not compile (tests/core/test_symbol_id_cursor.cpp
// pins it with a `static_assert` over a requires-expression). Each door's owner duty is in its signature:
//   * `AbortingSymbolIds::take(who) -> SymbolId` -- a space that cannot run out at a size the engine accepts: its
//     exhaustion is an engine defect, fatal BY NAME (`refuseExhaustedSymbolIds`);
//   * `OptionalSymbolIds::take(who) -> std::optional<SymbolId>` (and its all-or-none `take(count, who)`) -- the
//     refusal REPORTED inside, nothing written, nullopt; the owner tests the value at the take;
//   * `ReportedSymbolIds` -- a NEW space whose ids pass through many hands before they reach the output: the refusal
//     reported once inside, the invalid id handed out from then on, and the output leaves ONLY through
//     `seal(out) -> std::optional<Out>` (nullopt once refused) or `withhold()`. A door that handed out an id and is
//     destroyed neither sealed nor withheld is fatal BY NAME, so every run of its owner exercises the duty;
//   * `IndexSymbolIds` -- an INDEX space, where an id IS a position: `nextOrAbort` / `next` mint the next position in
//     the two forms above; `idAt` READS a position the space already holds, and refuses one it does not.
// Every refusal is stated through ONE emitter per form (below), the OPTIONAL and REPORTED ones under the TIER's code
// (`SymbolIdRefusalSink::code`, by the K_/L_/H_ scheme), so no tier writes its own sentence.
//
// TWO KINDS OF SPACE, ONE SOURCE OF THE RULE:
//   * a COUNTING space holds a `SymbolIdEnd`: one past the highest id the space holds or has handed out, GROW-ONLY. A
//     take steps over a value a format WRITER reserves (`isWriterReservedSymbolIdValue`) and refuses the last value
//     (minting it would leave the end nowhere to go, and a wrapped id would be 0, the invalid id). Two doors over one
//     end are two views of ONE space, so the second continues the first.
//   * an INDEX space (the front end's table, an object's records, the HIR text's handles), where an id IS a position:
//     stepping over a reserved value would break id == position, so the space ENDS at the first one.
namespace dss {

class DiagnosticReporter;
enum class DiagnosticCode : std::uint16_t;

// The refusal every hop between an OPTIONAL door and its output carries AS A VALUE (`std::expected<T,
// SymbolIdsRefused>`, or the hop's own nullopt / null / false where that answer means nothing else): the door has
// reported it; a hop only passes it on.
struct SymbolIdsRefused {};

// Where an OPTIONAL or REPORTED door states its refusal: the reporter, and the TIER's code by the K_/L_/H_ scheme
// (the link tier's K_SymbolIdSpaceExhausted, MIR→LIR's L_SymbolIdSpaceExhausted, HIR→MIR's
// H_SymbolIdSpaceExhausted). Every such code is an unsuppressable member (`kWhySymbolIdSpace`), and the emitter
// delivers it guaranteed.
struct SymbolIdRefusalSink {
    DiagnosticReporter* reporter;
    DiagnosticCode      code;
};

// What ran out: a counting space's END, or an index space's POSITION.
enum class SymbolIdSpaceKind : std::uint8_t { CountingEnd, IndexPosition };

// THE ABORT FORM'S ONE FATAL LINE: `who` names what was minting, `space` the space it minted in.
[[noreturn]] DSS_EXPORT void refuseExhaustedSymbolIds(std::string_view who, std::string_view space,
                                                      SymbolIdSpaceKind kind, std::uint64_t at);
// THE OPTIONAL AND REPORTED FORMS' ONE ERROR, delivered guaranteed under the tier's code; `who` names the space and
// what was being minted.
DSS_EXPORT void reportExhaustedSymbolIds(SymbolIdRefusalSink sink, std::string_view who, SymbolIdSpaceKind kind,
                                         std::uint64_t at);

class AbortingSymbolIds;
class OptionalSymbolIds;
class ReportedSymbolIds;

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
    // An end a holder carries as a plain value, opened. An end below 1 (nothing) is 1.
    [[nodiscard]] static constexpr SymbolIdEnd startingAt(std::uint32_t end) noexcept {
        SymbolIdEnd e;
        if (end > e.end_) e.end_ = end;
        return e;
    }
    // One past the highest id the space holds or has handed out (1: nothing).
    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return end_; }

private:
    friend class AbortingSymbolIds;
    friend class OptionalSymbolIds;
    friend class ReportedSymbolIds;

    // The next id, the end moved past it: a writer-reserved value is stepped over; the invalid id when the space is
    // exhausted. Reachable only through a door, which names the refusal.
    [[nodiscard]] constexpr SymbolId take() noexcept {
        while (end_ != kLast && isWriterReservedSymbolIdValue(end_)) ++end_;
        if (end_ == kLast) return SymbolId{};
        return SymbolId{end_++};
    }

    static constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t end_ = 1;
};

// THE ABORT FORM over a counting space's end: `space` names the space in the fatal line.
class AbortingSymbolIds {
public:
    constexpr AbortingSymbolIds(SymbolIdEnd& end, std::string_view space) noexcept : end_(&end), space_(space) {}
    [[nodiscard]] SymbolId take(std::string_view who) {
        SymbolId const id = end_->take();
        if (!id.valid()) refuseExhaustedSymbolIds(who, space_, SymbolIdSpaceKind::CountingEnd, end_->value());
        return id;
    }

private:
    SymbolIdEnd*     end_;
    std::string_view space_;
};

// THE OPTIONAL FORM over a counting space's end: a refused take writes NOTHING -- not the end, not a reserved value
// it stepped over (f26d's ruling on the take sites) -- and reports, every time it is refused.
class OptionalSymbolIds {
public:
    constexpr OptionalSymbolIds(SymbolIdEnd& end, SymbolIdRefusalSink sink) noexcept : end_(&end), sink_(sink) {}
    [[nodiscard]] std::optional<SymbolId> take(std::string_view who) {
        SymbolIdEnd    trial = *end_;
        SymbolId const id    = trial.take();
        if (!id.valid()) {
            reportExhaustedSymbolIds(sink_, who, SymbolIdSpaceKind::CountingEnd, trial.value());
            return std::nullopt;
        }
        *end_ = trial;
        return id;
    }
    // `count` ids, ALL OR NONE: the space too short for every one of them, the refusal reported once and nothing
    // written.
    [[nodiscard]] std::optional<std::vector<SymbolId>> take(std::size_t count, std::string_view who) {
        SymbolIdEnd           trial = *end_;
        std::vector<SymbolId> ids;
        ids.reserve(count);
        while (ids.size() < count) {
            SymbolId const id = trial.take();
            if (!id.valid()) {
                reportExhaustedSymbolIds(sink_, who, SymbolIdSpaceKind::CountingEnd, trial.value());
                return std::nullopt;
            }
            ids.push_back(id);
        }
        *end_ = trial;
        return ids;
    }

private:
    SymbolIdEnd*        end_;
    SymbolIdRefusalSink sink_;
};

// THE REPORTED FORM: a NEW space (it owns its end) whose output leaves only through the seal.
class ReportedSymbolIds {
public:
    ReportedSymbolIds(SymbolIdEnd start, SymbolIdRefusalSink sink, std::string_view who) noexcept
        : end_(start), sink_(sink), who_(who) {}
    ReportedSymbolIds(ReportedSymbolIds const&)            = delete;
    ReportedSymbolIds& operator=(ReportedSymbolIds const&) = delete;
    // Fatal BY NAME when an id was handed out and the output was neither sealed nor withheld.
    DSS_EXPORT ~ReportedSymbolIds();

    [[nodiscard]] SymbolId take() {
        handedOut_ = true;
        if (refused_) return SymbolId{};
        SymbolId const id = end_.take();
        if (!id.valid()) {
            refused_ = true;
            reportExhaustedSymbolIds(sink_, who_, SymbolIdSpaceKind::CountingEnd, end_.value());
        }
        return id;
    }
    // The owner's output, or nullopt once the space was refused. Settles the door.
    template <class Out>
    [[nodiscard]] std::optional<std::remove_cvref_t<Out>> seal(Out&& out) {
        settled_ = true;
        if (refused_) return std::nullopt;
        return std::optional<std::remove_cvref_t<Out>>{std::forward<Out>(out)};
    }
    // The owner refuses its output for a reason of its own (already reported). Settles the door.
    [[nodiscard]] std::nullopt_t withhold() noexcept {
        settled_ = true;
        return std::nullopt;
    }

private:
    SymbolIdEnd         end_;
    SymbolIdRefusalSink sink_;
    std::string_view    who_;
    bool                handedOut_ = false;
    bool                refused_   = false;
    bool                settled_   = false;
};

// THE INDEX RULE as a door: a view of an index space of `positions` positions (slot 0, the invalid id, counted).
class IndexSymbolIds {
public:
    explicit constexpr IndexSymbolIds(std::size_t positions) noexcept : positions_(positions) {}

    // Whether a space of `positions` positions ends below the first writer-reserved value.
    [[nodiscard]] static constexpr bool fits(std::size_t positions) noexcept {
        return positions <= kFirstWriterReservedSymbolIdValue;
    }
    // The id of a position the space HOLDS (0 < position < positions); fatal by name otherwise -- a READ.
    [[nodiscard]] SymbolId idAt(std::size_t position, std::string_view space) const {
        if (position == 0u || position >= positions_ || !fits(position + 1u))
            refuseExhaustedSymbolIds("IndexSymbolIds::idAt", space, SymbolIdSpaceKind::IndexPosition, position);
        return SymbolId{static_cast<std::uint32_t>(position)};
    }
    // The id of the NEXT position (`positions`) -- the ABORT form.
    [[nodiscard]] SymbolId nextOrAbort(std::string_view who, std::string_view space) const {
        if (positions_ == 0u || !fits(positions_ + 1u))
            refuseExhaustedSymbolIds(who, space, SymbolIdSpaceKind::IndexPosition, positions_);
        return SymbolId{static_cast<std::uint32_t>(positions_)};
    }
    // The id of the NEXT position -- the OPTIONAL form.
    [[nodiscard]] std::optional<SymbolId> next(SymbolIdRefusalSink sink, std::string_view who) const {
        if (positions_ == 0u || !fits(positions_ + 1u)) {
            reportExhaustedSymbolIds(sink, who, SymbolIdSpaceKind::IndexPosition, positions_);
            return std::nullopt;
        }
        return SymbolId{static_cast<std::uint32_t>(positions_)};
    }

private:
    std::size_t positions_;
};

} // namespace dss
