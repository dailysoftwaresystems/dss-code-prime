// ── D-LIR-RETURN-REG-REFUSAL-IS-UNREACHABLE-FROM-THE-TEST-TIER ──────────────
//    plus the LIR half of
//    D-CONFIG-ENUM-KEYED-MAP-DIAGNOSTICS-RETYPE-THEIR-CLOSED-SET.
// ────────────────────────────────────────────────────────────────────────────
//
// THE CLASS. Something decides acceptance, and the sentence beside it states the
// accepted set AGAIN as a literal. Two owners of one fact: the decision keeps
// working while the sentence rots, and the sentence is the half a reader gets.
//
// ★★★ WHY THIS FILE EXISTS. `returnRegisterForClass` was converted to answer BOTH
// questions off one row set — and the conversion asserted NOTHING, because the
// rows and the function lived in `lir_callconv.cpp`'s anonymous namespace and no
// caller in this tree can reach a `None`/`Flags` result. ✔MEASURED before this
// file existed: retyping the accepted set back into the pre-VR literal
// (`"only gpr/fpr results can be returned in registers"`) left `ctest` at rc=0
// with 16 of 16 LIR tests GREEN. A guard that cannot fire asserts nothing, so
// the rows and the lookup moved to `lir_callconv.hpp` and the refusal became
// something a tier can OBSERVE.
//
// ★★ WHAT THE PINS ASSERT, and why no single one of them carries the claim:
//   (A) THE ROWS ARE THE MAP — for EVERY `LirRegClass` enumerator, the class is
//       accepted exactly when a row names it, and the pool handed back is the cc
//       member that row names. Read off `kReturnPoolRows`, never typed here.
//   (B) COMPLETENESS — the refusal for a class with NO row names EVERY spelling
//       the rows own. This is the direction the historical drift was on, and the
//       only one that catches a re-typed literal.
//   (C) HONESTY — the refusal quotes no register-class spelling that has no row.
//       Catches the mirror defect: a sentence WIDER than its check, which tells
//       a reader a class is returnable when the lookup will refuse it.
//   (D) THE LOOKUP FOLLOWS THE ROWS — for each row class the register handed
//       back is the one the SHIPPED cc names in that row's pool, so acceptance
//       and the pool cannot part company either.
//
// ★ THE EXPECTATION IS DRIVEN FROM THE ROWS, NEVER FROM TODAY'S SENTENCE.
// Spelling out the expected message here would be the same retyping one level
// up: both halves of the comparison would be literals nobody re-derives.
//
// ⚠ Runs against BOTH shipped targets. `returnVrs` is declared by arm64 and
// EMPTY on x86_64, so a pin that only ever saw one of them would not know
// whether it was testing the rows or one target's declarations.

#include "core/types/config_key_vocabulary.hpp"   // detail::renderAllowedList
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_reg.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

// Every enumerator of the LIR register class, taken from the ONE table that
// owns the spellings rather than typed out here — so a class added to
// `kTargetRegClassTable` is covered by these pins without an edit.
constexpr auto kAllRegClassNames = allNames(kTargetRegClassTable);

// A context label with no apostrophes in it: the honesty pin reads the quoted
// tokens of the message, and a label that quoted something would be a token the
// pin then has to excuse.
constexpr char const* kLabel = "returnPoolProjection";

// Does the message ADVERTISE this spelling? `renderAllowedList` is the one
// closed-set renderer in this tree and it QUOTES every entry, so "advertised"
// is "appears in quotes" — and asking for the quoted form is what makes the
// check immune to the spelling turning up inside some longer word.
//
// ⚠ DELIBERATELY NOT a copy of `tests/core/vocabulary_projection_probe.hpp`'s
// `quotedTokens`. That header is not on this target's include path (`dss_add_test`
// adds `src` and `tests/test_support`, not `tests/core`), and minting a fifth
// copy of it in the same cycle that consolidated the other three would be the
// defect wearing a different hat. The predicate below needs no tokenizer.
[[nodiscard]] bool advertisesQuoted(std::string const& msg, std::string_view name) {
    return msg.find("'" + std::string{name} + "'") != std::string::npos;
}

[[nodiscard]] bool rowsName(LirRegClass cls) {
    for (auto const& row : kReturnPoolRows) {
        if (row.cls == cls) return true;
    }
    return false;
}

[[nodiscard]] bool advertised(std::string_view name) {
    for (auto const& n : kReturnPoolClassNames) {
        if (n == name) return true;
    }
    return false;
}

[[nodiscard]] std::string summarize(DiagnosticReporter const& r) {
    std::string s;
    for (auto const& d : r.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"<no diagnostics>"} : s;
}

// The shipped targets, and the cc index every MIR→LIR boundary verb uses.
constexpr char const* kTargets[] = {"x86_64", "arm64"};

} // namespace

// ── (A) THE ROWS ARE THE MAP ────────────────────────────────────────────────
//
// The complement is DEFINED, never enumerated: "has a pool" is "a row names it",
// so a class added to one side and not the other fails here instead of quietly
// becoming a refusal (or, worse, quietly acquiring somebody else's pool).
TEST(LirReturnPoolProjection, EveryRegisterClassIsAcceptedExactlyWhenARowNamesIt) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        for (std::size_t i = 0; i < kAllRegClassNames.size(); ++i) {
            auto const cls = static_cast<LirRegClass>(i);
            SCOPED_TRACE(std::string{lirRegClassName(cls)});
            EXPECT_EQ(returnRegisterPool(*cc, cls) != nullptr, rowsName(cls))
                << "acceptance and the row set disagree about this class — the "
                   "pool lookup and the sentence that advertises it are one "
                   "walk, and this is the walk";
            // …and the ADVERTISED set is exactly the accepted set. Both come
            // off `kReturnPoolRows`; a second array of names would be the
            // defect this pin exists for, one level down.
            EXPECT_EQ(advertised(lirRegClassName(cls)), rowsName(cls));
        }
    }
}

// ── (D) THE LOOKUP FOLLOWS THE ROWS ─────────────────────────────────────────
//
// For every row whose pool the shipped cc actually populates, the register
// handed back is the one that cc NAMES at that ordinal. Without this, the rows
// could be right and the indexing could still read the wrong list.
TEST(LirReturnPoolProjection, EachRowResolvesTheRegisterTheCcNames) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        std::size_t populatedRows = 0;
        for (auto const& row : kReturnPoolRows) {
            auto const* pool = returnRegisterPool(*cc, row.cls);
            ASSERT_NE(pool, nullptr);
            if (pool->empty()) continue;
            ++populatedRows;
            for (std::uint32_t ord = 0; ord < pool->size(); ++ord) {
                SCOPED_TRACE(std::string{lirRegClassName(row.cls)} + " ordinal "
                             + std::to_string(ord));
                DiagnosticReporter rep;
                auto const reg = returnRegisterForClass(**s, *cc, row.cls, ord,
                                                        kLabel, rep);
                ASSERT_TRUE(reg.has_value()) << summarize(rep);
                EXPECT_EQ(rep.errorCount(), 0u) << summarize(rep);
                auto const named = (*s)->registerByName((*pool)[ord]);
                ASSERT_TRUE(named.has_value());
                EXPECT_EQ(reg->id, *named)
                    << "the lookup returned a register the cc does not name at "
                       "this ordinal — the row's pool and the index disagree";
                EXPECT_EQ(reg->regClass(), row.cls);
                EXPECT_EQ(reg->isPhysical, 1u);
            }
        }
        EXPECT_GE(populatedRows, 2u)
            << "this target populates fewer than two return pools, so the pins "
               "above would pass while exercising almost nothing";

        // One past the end of a populated pool REFUSES, and the sentence names
        // the class it consulted — the bound is a property of the row's pool,
        // not of some other pool that happens to be longer.
        DiagnosticReporter rep;
        auto const over = returnRegisterForClass(
            **s, *cc, LirRegClass::GPR,
            static_cast<std::uint32_t>(cc->returnGprs.size()), kLabel, rep);
        EXPECT_FALSE(over.has_value());
        ASSERT_EQ(rep.all().size(), 1u) << summarize(rep);
        EXPECT_NE(rep.all()[0].actual.find(
                      std::string{lirRegClassName(LirRegClass::GPR)}),
                  std::string::npos)
            << "the out-of-range refusal does not name the class whose pool it "
               "bounded:\n" << rep.all()[0].actual;
    }
}

// ── (A2) THE CORPUS ARM — the shipped cc's declared pools are each REACHED ──
//
// ★★★ THE ARM THAT KEEPS THE REST FROM BEING THEATRE, and it is written out on
// purpose. Every pin above derives its expectation from `kReturnPoolRows`, so a
// mutant that DELETES a row moves both halves of the comparison together:
// ✔MEASURED — dropping the `vr` row (count adjusted) left all 16 LIR tests
// GREEN, because `vr` simply joined the refusal arm and the refusal correctly
// advertised the two rows that remained. Self-consistent and wrong is exactly
// the failure mode this whole file is about, one level up.
//
// So this claim is about the SHIPPED ABI rather than about the rows. arm64
// declares `returnVrs` (AAPCS64 §5.5 — a binary128 result comes back in v0) and
// `mir_to_lir`'s F128 return verb reads `cc->returnVrs[0]` to capture it; a
// build where a `vr` result does not resolve THROUGH that list is a
// wrong-register capture with no diagnostic. The three field names below are the
// corpus claim, not a second copy of the row set.
TEST(LirReturnPoolProjection, TheShippedCcDeclaredPoolsAreEachReachedByTheLookup) {
    struct DeclaredPool {
        LirRegClass                     cls;
        std::vector<std::string> const* names;
        char const*                     field;
    };

    bool sawPopulatedVrPool = false;
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        DeclaredPool const declared[] = {
            {LirRegClass::GPR, &cc->returnGprs, "returnGprs"},
            {LirRegClass::FPR, &cc->returnFprs, "returnFprs"},
            {LirRegClass::VR,  &cc->returnVrs,  "returnVrs"},
        };
        for (auto const& d : declared) {
            // x86_64 declares no VR return pool at all; an empty list is a
            // legitimate config answer and not something to assert against.
            if (d.names->empty()) continue;
            if (d.cls == LirRegClass::VR) sawPopulatedVrPool = true;
            SCOPED_TRACE(d.field);
            DiagnosticReporter rep;
            auto const reg = returnRegisterForClass(**s, *cc, d.cls, 0, kLabel, rep);
            ASSERT_TRUE(reg.has_value())
                << "the shipped cc declares a non-empty " << d.field
                << ", but the lookup REFUSES this class — a result the ABI has "
                   "a register for cannot be returned at all:" << summarize(rep);
            auto const named = (*s)->registerByName((*d.names)[0]);
            ASSERT_TRUE(named.has_value());
            EXPECT_EQ(reg->id, *named)
                << "the lookup did not resolve through " << d.field;
        }
    }
    EXPECT_TRUE(sawPopulatedVrPool)
        << "no shipped target populates a VR return pool, so the VR half of "
           "this arm probed nothing — re-aim it before trusting it";
}

// ── (B)+(C) THE REFUSAL FOR A CLASS WITH NO ROW ─────────────────────────────
//
// The arm lane O's conversion made correct and that nothing could observe. Both
// directions are asserted on the SAME message: it must name every advertised
// spelling, and it must name no class spelling that has no row.
TEST(LirReturnPoolProjection, ARefusedClassNamesEveryRowAndNothingElse) {
    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        for (std::size_t i = 0; i < kAllRegClassNames.size(); ++i) {
            auto const cls = static_cast<LirRegClass>(i);
            if (rowsName(cls)) continue;          // has a pool; not this arm
            SCOPED_TRACE(std::string{lirRegClassName(cls)});

            DiagnosticReporter rep;
            auto const reg = returnRegisterForClass(**s, *cc, cls, 0, kLabel, rep);
            EXPECT_FALSE(reg.has_value())
                << "a class with no result-register pool was ACCEPTED — the "
                   "else-branch default this row set replaced is back";
            ASSERT_EQ(rep.all().size(), 1u) << summarize(rep);
            std::string const& msg = rep.all()[0].actual;
            EXPECT_EQ(rep.all()[0].code, DiagnosticCode::L_CcRegLookupFailed);

            // (B) COMPLETENESS.
            for (std::string_view const n : kReturnPoolClassNames) {
                EXPECT_TRUE(advertisesQuoted(msg, n))
                    << "the refusal does NOT name '" << n
                    << "', which `kReturnPoolRows` accepts. A sentence NARROWER "
                       "than its check tells a reader by name that a class this "
                       "very lookup takes cannot be returned in a register. "
                       "Render the set through `renderAllowedList` over "
                       "`kReturnPoolClassNames`, never as a literal.\nmessage "
                       "was:\n" << msg;
            }

            // (C) HONESTY — the only non-advertised class spelling this
            // sentence may quote is the OFFENDING one, which it quotes by
            // design. Any other means the message claims a pool the lookup
            // does not have.
            for (std::string_view const n : kAllRegClassNames) {
                if (advertised(n)) continue;              // a row names it
                if (n == lirRegClassName(cls)) continue;  // the offending class
                EXPECT_FALSE(advertisesQuoted(msg, n))
                    << "the refusal quotes register class '" << n
                    << "', which no row names — a sentence WIDER than its check "
                       "sends a reader to ask for a pool that does not "
                       "exist.\nmessage was:\n" << msg;
            }
        }
    }
}
