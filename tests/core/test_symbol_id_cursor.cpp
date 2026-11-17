// THE ONE MINTING RULE OF EVERY SYMBOL-ID SPACE (P69 lane `cs`, fold f26d -- review-csf26c2 N9): the counting cursor
// `SymbolIdEnd` and the index rule `symbolIdOfIndex` (core/types/symbol_id_cursor.hpp), which every space's door asks.
//
// THE UNIVERSE: a cursor past nothing, past a held id, past a held id at the last values (the seed a `+ 1` wrapped
// onto 0); a take after a take; a raise that would lower; the writer-reserved value stepped over; exhaustion; the
// index rule over its bounds (0, 1, the first reserved value and the one before it, the last value).
//
// RED-ON-DISABLE: compute the next as `h + 1` unsaturated and the highest-at-the-last-value cells take an id; drop
// the reserved step and the cursor hands out the PE `_tls_index` value; drop the index rule's reserved bound and
// the first-reserved cell is a valid id.

#include "core/types/object_format_kind.hpp"
#include "core/types/symbol_id_cursor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();

} // namespace

TEST(SymbolIdCursor, ACursorHandsOutTheIdPastTheHighestHeldAndMovesPastIt) {
    dss::SymbolIdEnd end;
    EXPECT_EQ(end.value(), 1u) << "a space holding nothing starts at 1, past the invalid id";
    EXPECT_EQ(end.take().v, 1u);
    EXPECT_EQ(end.take().v, 2u) << "a take after a take is the next id";
    dss::SymbolIdEnd past;
    past.keepClearOfHighest(41u);
    EXPECT_EQ(past.take().v, 42u);
    past.keepClearOfHighest(7u);
    EXPECT_EQ(past.value(), 43u) << "a raise below the end lowers nothing";
    past.keepClearOf(dss::SymbolId{99u});
    EXPECT_EQ(past.take().v, 100u);
    dss::SymbolIdEnd other;
    other.keepClearOf(past);
    EXPECT_EQ(other.value(), past.value()) << "an end raised to another end of the space continues it";
}

TEST(SymbolIdCursor, TheWriterReservedValueIsSteppedOver) {
    dss::SymbolIdEnd end;
    end.keepClearOfHighest(dss::kTlsIndexReservedSymbolIdValue - 1u);
    ASSERT_TRUE(dss::isWriterReservedSymbolIdValue(dss::kTlsIndexReservedSymbolIdValue));
    EXPECT_EQ(end.take().v, dss::kTlsIndexReservedSymbolIdValue + 1u)
        << "the PE writer's `_tls_index` value is never a module's";
}

TEST(SymbolIdCursor, ExhaustionIsTheInvalidIdNeverAWrappedOne) {
    {
        dss::SymbolIdEnd end;
        end.keepClearOfHighest(kLast);
        EXPECT_FALSE(end.take().valid()) << "a space whose highest id is the last value has no next";
        EXPECT_FALSE(end.take().valid()) << "and stays exhausted";
    }
    {
        dss::SymbolIdEnd end;
        end.keepClearOfHighest(kLast - 1u);
        EXPECT_FALSE(end.take().valid()) << "the last value is never handed out: the end would have nowhere to go";
    }
    {
        dss::SymbolIdEnd end;
        end.keepClearOfHighest(kLast - 2u);
        EXPECT_EQ(end.take().v, kLast - 1u);
        EXPECT_FALSE(end.take().valid());
    }
}

TEST(SymbolIdCursor, AnIndexSpaceEndsAtTheFirstWriterReservedValue) {
    EXPECT_FALSE(dss::symbolIdOfIndex(0u).valid()) << "position 0 is the invalid id";
    EXPECT_EQ(dss::symbolIdOfIndex(1u).v, 1u);
    EXPECT_EQ(dss::symbolIdOfIndex(dss::kFirstWriterReservedSymbolIdValue - 1u).v,
              dss::kFirstWriterReservedSymbolIdValue - 1u);
    EXPECT_FALSE(dss::symbolIdOfIndex(dss::kFirstWriterReservedSymbolIdValue).valid())
        << "an index space does not step over a reserved value (an id IS its position): it ends there";
    EXPECT_FALSE(dss::symbolIdOfIndex(kLast).valid());
    EXPECT_FALSE(dss::symbolIdOfIndex(std::size_t{kLast} + 1u).valid());
    EXPECT_LE(dss::kFirstWriterReservedSymbolIdValue, dss::kTlsIndexReservedSymbolIdValue)
        << "the first reserved value is the lowest of the reserved set";
}
