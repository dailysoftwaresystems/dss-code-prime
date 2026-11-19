// THE ONE HOME OF A FRESH SymbolId (P69 lane `cs`: fold f26d -- review-csf26c2 N9; fold f26e -- review-csf26d B1,
// its home audit v2): the counting end `SymbolIdEnd`, reachable only through its door types, and the index rule,
// reachable only through `IndexSymbolIds` (core/types/symbol_id_cursor.hpp).
//
// THE UNIVERSE: per FORM, a door past nothing, past a held id, past a held id at the last values (the seed a `+ 1`
// wrapped onto 0); a take after a take; a raise that would lower; the writer-reserved value stepped over; exhaustion
// -- fatal by name (ABORT), reported and nothing written (OPTIONAL), reported once with the output withheld by the
// seal (REPORTED), and the REPORTED door destroyed unsettled; the index rule over its bounds (0, 1, the first reserved
// value and the one before it, the last value). And the STRUCTURE (A3): the cursor's take is no member a source
// that states no form can call.
//
// RED-ON-DISABLE: compute the next as `h + 1` unsaturated and the highest-at-the-last-value cells take an id; drop
// the reserved step and a door hands out the PE `_tls_index` value; drop the index rule's reserved bound and the
// first-reserved cell is a position; let the OPTIONAL door write its trial on a refusal and the end moves; make the
// seal ignore the refusal and the refused output is engaged; make `take` public and the static_assert fails to build.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/object_format_kind.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_id_cursor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();

// ★ A3, STRUCTURAL: no source that states no form can take from an end or compute an index id. Asked through a
// concept, an inaccessible member is an unsatisfied constraint (outside a template it would be a hard error); the
// index rule has no free function at all.
template <class End>
concept TakesWithoutADoor = requires(End& e) { e.take(); };
static_assert(!TakesWithoutADoor<dss::SymbolIdEnd>, "SymbolIdEnd::take must be reachable only through a door type");

[[nodiscard]] std::size_t countOf(dss::DiagnosticReporter const& r, dss::DiagnosticCode code) {
    std::size_t n = 0;
    for (auto const& d : r.all()) if (d.code == code) ++n;
    return n;
}

[[nodiscard]] dss::SymbolIdRefusalSink linkSink(dss::DiagnosticReporter& r) {
    return dss::SymbolIdRefusalSink{&r, dss::DiagnosticCode::K_SymbolIdSpaceExhausted};
}

} // namespace

TEST(SymbolIdCursor, ACursorHandsOutTheIdPastTheHighestHeldAndMovesPastIt) {
    dss::SymbolIdEnd end;
    EXPECT_EQ(end.value(), 1u) << "a space holding nothing starts at 1, past the invalid id";
    dss::AbortingSymbolIds ids{end, "the test space"};
    EXPECT_EQ(ids.take("test").v, 1u);
    EXPECT_EQ(ids.take("test").v, 2u) << "a take after a take is the next id";
    EXPECT_EQ(end.value(), 3u) << "the door advanced the end it is a view of";
    dss::SymbolIdEnd past;
    past.keepClearOfHighest(41u);
    EXPECT_EQ((dss::AbortingSymbolIds{past, "the test space"}.take("test").v), 42u);
    past.keepClearOfHighest(7u);
    EXPECT_EQ(past.value(), 43u) << "a raise below the end lowers nothing";
    past.keepClearOf(dss::SymbolId{99u});
    EXPECT_EQ((dss::AbortingSymbolIds{past, "the test space"}.take("test").v), 100u);
    dss::SymbolIdEnd other;
    other.keepClearOf(past);
    EXPECT_EQ(other.value(), past.value()) << "an end raised to another end of the space continues it";
}

TEST(SymbolIdCursor, TheWriterReservedValueIsSteppedOver) {
    dss::SymbolIdEnd end;
    end.keepClearOfHighest(dss::kTlsIndexReservedSymbolIdValue - 1u);
    ASSERT_TRUE(dss::isWriterReservedSymbolIdValue(dss::kTlsIndexReservedSymbolIdValue));
    dss::DiagnosticReporter r;
    EXPECT_EQ(dss::OptionalSymbolIds(end, linkSink(r)).take("test"),
              std::optional<dss::SymbolId>{dss::SymbolId{dss::kTlsIndexReservedSymbolIdValue + 1u}})
        << "the PE writer's `_tls_index` value is never a module's";
}

// EXHAUSTION, THE OPTIONAL FORM: refused, reported under the sink's code, NOTHING written -- every time it is asked;
// never the last value, never a wrapped id.
TEST(SymbolIdCursor, ExhaustionIsTheInvalidIdNeverAWrappedOne) {
    for (std::uint32_t const highest : {kLast, kLast - 1u}) {
        dss::SymbolIdEnd end;
        end.keepClearOfHighest(highest);
        std::uint32_t const before = end.value();
        dss::DiagnosticReporter r;
        dss::OptionalSymbolIds ids{end, linkSink(r)};
        EXPECT_FALSE(ids.take("the test space").has_value())
            << (highest == kLast ? "a space whose highest id is the last value has no next"
                                 : "the last value is never handed out");
        EXPECT_EQ(end.value(), before) << "a refused take wrote the end";
        EXPECT_FALSE(ids.take(3u, "the test space").has_value()) << "all or none";
        EXPECT_EQ(end.value(), before) << "a refused take of several wrote the end";
        EXPECT_EQ(countOf(r, dss::DiagnosticCode::K_SymbolIdSpaceExhausted), 2u) << "one report per refused take";
        EXPECT_EQ(r.errorCount(), 2u);
    }
    dss::SymbolIdEnd end;
    end.keepClearOfHighest(kLast - 2u);
    dss::DiagnosticReporter r;
    dss::OptionalSymbolIds ids{end, linkSink(r)};
    EXPECT_FALSE(ids.take(2u, "the test space").has_value()) << "one id is left, two were asked";
    EXPECT_EQ(end.value(), kLast - 1u) << "ALL OR NONE: the refusal wrote nothing";
    EXPECT_EQ(ids.take("the test space"), std::optional<dss::SymbolId>{dss::SymbolId{kLast - 1u}});
    EXPECT_FALSE(ids.take("the test space").has_value());
}

// The refusal is delivered past every volume gate: a door refusal under `maxPerCode = 0` and no dedup window is ONE
// Error counted AND stored, for each of the tier codes (A5).
TEST(SymbolIdCursor, TheRefusalIsDeliveredUnderEveryTiersCodeWhateverTheVolumeGates) {
    for (dss::DiagnosticCode const code : {dss::DiagnosticCode::K_SymbolIdSpaceExhausted,
                                           dss::DiagnosticCode::L_SymbolIdSpaceExhausted,
                                           dss::DiagnosticCode::H_SymbolIdSpaceExhausted}) {
        dss::DiagnosticReporter::Config cfg;
        cfg.maxPerCode  = 0;
        cfg.dedupWindow = 0;
        dss::DiagnosticReporter r{cfg};
        dss::SymbolIdEnd end;
        end.keepClearOfHighest(kLast);
        EXPECT_FALSE((dss::OptionalSymbolIds{end, dss::SymbolIdRefusalSink{&r, code}}.take("the test space")));
        EXPECT_EQ(r.errorCount(), 1u) << dss::diagnosticCodeName(code);
        EXPECT_EQ(countOf(r, code), 1u) << dss::diagnosticCodeName(code) << " was not delivered past the gates";
    }
}

// THE REPORTED FORM: reported ONCE, the invalid id from then on, and the output refused by the seal.
TEST(SymbolIdCursor, AReportedDoorRefusesItsOutputThroughTheSeal) {
    {
        dss::DiagnosticReporter r;
        dss::ReportedSymbolIds ids{dss::SymbolIdEnd::startingAt(kLast - 1u), linkSink(r), "the test space"};
        EXPECT_EQ(ids.take().v, kLast - 1u);
        EXPECT_FALSE(ids.take().valid());
        EXPECT_FALSE(ids.take().valid()) << "and stays refused";
        EXPECT_EQ(countOf(r, dss::DiagnosticCode::K_SymbolIdSpaceExhausted), 1u) << "reported ONCE";
        EXPECT_FALSE(ids.seal(std::string{"the output"}).has_value()) << "a refused space's output was sealed";
    }
    {
        dss::DiagnosticReporter r;
        dss::ReportedSymbolIds ids{dss::SymbolIdEnd{}, linkSink(r), "the test space"};
        EXPECT_EQ(ids.take().v, 1u);
        EXPECT_EQ(ids.seal(std::string{"the output"}), std::optional<std::string>{"the output"})
            << "CONTROL: an unrefused space's output is sealed whole";
        EXPECT_EQ(r.errorCount(), 0u);
    }
    {
        dss::DiagnosticReporter r;
        dss::ReportedSymbolIds ids{dss::SymbolIdEnd{}, linkSink(r), "the test space"};
        (void)ids.take();
        std::optional<std::string> const withheld = ids.withhold();
        EXPECT_FALSE(withheld.has_value()) << "an owner's own refusal settles the door";
    }
}

TEST(SymbolIdCursorDeathTest, AReportedDoorDestroyedUnsettledIsFatalByName) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            dss::DiagnosticReporter r;
            // Parentheses, not braces: a brace list's commas would split the macro's arguments.
            dss::ReportedSymbolIds ids(dss::SymbolIdEnd{}, linkSink(r), "the unsettled test space");
            (void)ids.take();
        },
        "the unsettled test space handed out ids and was destroyed with its output neither sealed nor withheld");
}

TEST(SymbolIdCursorDeathTest, AnAbortingDoorIsFatalByNameAtExhaustion) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(
        {
            dss::SymbolIdEnd end;
            end.keepClearOfHighest(kLast);
            (void)dss::AbortingSymbolIds(end, "the test space").take("theTestMinter");
        },
        "theTestMinter fatal: the test space's SymbolId space is exhausted");
}

TEST(SymbolIdCursor, AnIndexSpaceEndsAtTheFirstWriterReservedValue) {
    EXPECT_FALSE(dss::IndexSymbolIds::fits(std::size_t{dss::kFirstWriterReservedSymbolIdValue} + 1u))
        << "a space holding the first reserved value as a position does not fit";
    EXPECT_TRUE(dss::IndexSymbolIds::fits(dss::kFirstWriterReservedSymbolIdValue))
        << "positions 0 .. first reserved - 1 fit";
    EXPECT_EQ(dss::IndexSymbolIds{2u}.idAt(1u, "the test table").v, 1u);
    EXPECT_EQ(dss::IndexSymbolIds{5u}.nextOrAbort("test", "the test table").v, 5u);
    dss::DiagnosticReporter r;
    EXPECT_EQ(dss::IndexSymbolIds{dss::kFirstWriterReservedSymbolIdValue - 1u}.next(linkSink(r), "the test table"),
              std::optional<dss::SymbolId>{dss::SymbolId{dss::kFirstWriterReservedSymbolIdValue - 1u}})
        << "the position before the first reserved value is the last one";
    EXPECT_FALSE(dss::IndexSymbolIds{dss::kFirstWriterReservedSymbolIdValue}.next(linkSink(r), "the test table"))
        << "the first reserved value is no position";
    EXPECT_EQ(countOf(r, dss::DiagnosticCode::K_SymbolIdSpaceExhausted), 1u);
}

TEST(SymbolIdCursorDeathTest, AnIndexReadOfAPositionTheSpaceDoesNotHoldIsFatal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ (void)dss::IndexSymbolIds{3u}.idAt(0u, "the test table"); },
                 "the test table's SymbolId space is exhausted: position 0");
    EXPECT_DEATH({ (void)dss::IndexSymbolIds{3u}.idAt(3u, "the test table"); },
                 "the test table's SymbolId space is exhausted: position 3");
    EXPECT_DEATH({ (void)dss::IndexSymbolIds{std::size_t{kLast} + 2u}.nextOrAbort("t", "the test table"); },
                 "the test table's SymbolId space is exhausted");
}
