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
// ⚠ Runs against BOTH shipped targets, so a pin cannot confuse "the rows say
// so" with "this one target declares it that way". The two differ in the sizes
// of their pools (x86_64's `returnFprs` is 2 or 1; arm64's is 4) rather than in
// which pools exist.
//
// ⚠⚠ THIS NOTE USED TO SAY "`returnVrs` is declared by arm64 and EMPTY on
// x86_64", AND THAT DIFFERENCE IS GONE ALONG WITH THE KEY. R1 of the operator's
// design A′ made arm64 declare its SIMD&FP file ONCE, so a binary128 result
// comes back in `returnFprs[0]` = `v0` (the FULL register) rather than in a
// second `returnVrs` pool over the same physical registers, `kReturnPoolRows`
// is two rows, and no shipped target declares a `vr`-class register at all.
// ★ A VR result therefore now finds NO ROW and is refused BY NAME — the
// fail-loud answer this file exists to make observable — which is a stronger
// statement of the same defect, not a weaker one.

#include "core/types/config_key_vocabulary.hpp"   // detail::renderAllowedList
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/target_schema.hpp"
#include "lir/lir_callconv.hpp"
#include "lir/lir_reg.hpp"

#include "vocabulary_message_probe.hpp"

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
// ✔ IT NOW ASKS THE ONE TOKENIZER (2026-08-23, cycle P28,
// D-TEST-VOCABULARY-PROBE-MESSAGE-HALF-IS-UNREACHABLE-AND-JSON-COUPLED). This
// used to be a private `msg.find("'" + name + "'")`, and the comment here gave
// the honest reason: `tests/core/vocabulary_projection_probe.hpp` was NOT on
// this target's include path, and it dragged `nlohmann/json.hpp` behind it for a
// TU that touches no JSON. Both obstacles are gone — the message-reading half
// lives in `tests/test_support/vocabulary_message_probe.hpp`, which is json-free
// and on every test target's `-I` path — so this predicate defers to the one
// definition of "a quoted token" instead of holding a second opinion about it.
//
// ⚠ THE SUBSTRING FORM AND THE TOKENIZER DISAGREE ON POSSESSIVES, and the
// tokenizer is the stricter of the two: it PAIRS apostrophes, so a possessive in
// the prose scrambles every token after it. That is the constraint the rest of
// the tree already holds these sentences to, and this file is now held to it as
// well rather than being quietly exempt.
[[nodiscard]] bool advertisesQuoted(std::string const& msg, std::string_view name) {
    for (auto const& q : ::dss::test_support::quotedTokens(msg)) {
        if (q == name) return true;
    }
    return false;
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
// So this claim is about the SHIPPED ABI rather than about the rows. AAPCS64
// §5.5 returns a binary128 result in `v0`, and `mir_to_lir`'s F128 return verb
// reads `cc->returnFprs[0]` to capture it; a build where that result does not
// resolve THROUGH that list is a wrong-register capture with no diagnostic. The
// field names below are the corpus claim, not a second copy of the row set.
//
// ⚠⚠ A THIRD ROW `{LirRegClass::VR, &cc->returnVrs, "returnVrs"}` USED TO SIT
// IN THIS TABLE, GUARDED BY A `sawPopulatedVrPool` NON-VACUITY FLAG BECAUSE
// x86_64 LEFT THE POOL EMPTY. R1 of design A′ deleted `returnVrs` — it named a
// SECOND return pool over the same physical registers `returnFprs` names — so
// the row cannot be spelled any more and the flag it needed has nothing to
// count. ★ The ABI fact it carried did not disappear: it moved into the
// `returnFprs` row below, where `v0` is now named at its full sixteen bytes,
// and the width assertion is what keeps that from silently becoming the 8-byte
// view again.
TEST(LirReturnPoolProjection, TheShippedCcDeclaredPoolsAreEachReachedByTheLookup) {
    struct DeclaredPool {
        LirRegClass                     cls;
        std::vector<std::string> const* names;
        char const*                     field;
    };

    for (char const* const t : kTargets) {
        SCOPED_TRACE(t);
        auto s = TargetSchema::loadShipped(t);
        ASSERT_TRUE(s.has_value());
        auto const* cc = (*s)->callingConvention(0);
        ASSERT_NE(cc, nullptr);

        DeclaredPool const declared[] = {
            {LirRegClass::GPR, &cc->returnGprs, "returnGprs"},
            {LirRegClass::FPR, &cc->returnFprs, "returnFprs"},
        };
        for (auto const& d : declared) {
            ASSERT_FALSE(d.names->empty())
                << "the shipped cc declares an EMPTY " << d.field
                << ", so this row probed nothing — every shipped cc returns "
                   "both an integer and a floating result in registers";
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

    // ★ THE HALF THE DELETED `returnVrs` ROW USED TO CARRY, AS AN ABI CLAIM
    // ABOUT AArch64. A binary128 result comes back in v0 — the FULL 16-byte
    // SIMD&FP register — so the FP return pool must name that register and not
    // its 8-byte `d0` view. This is written per-target rather than derived,
    // because it is the fact a second pool used to state and something has to
    // keep stating it.
    auto arm = TargetSchema::loadShipped("arm64");
    ASSERT_TRUE(arm.has_value());
    auto const* aapcs64 = (*arm)->callingConvention(0);
    ASSERT_NE(aapcs64, nullptr);
    ASSERT_FALSE(aapcs64->returnFprs.empty());
    EXPECT_EQ(aapcs64->returnFprs[0], "v0");
    auto const v0 = (*arm)->registerByName(aapcs64->returnFprs[0]);
    ASSERT_TRUE(v0.has_value());
    auto const* v0Info = (*arm)->registerInfo(*v0);
    ASSERT_NE(v0Info, nullptr);
    EXPECT_EQ(v0Info->widthBytes, 16u)
        << "the FP return register must be the FULL SIMD&FP register — an "
           "8-byte view here captures half of a binary128 result and the other "
           "half comes from nowhere";
    EXPECT_TRUE(v0Info->subOf.empty())
        << "a calling convention must name a full register, never a view";
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
