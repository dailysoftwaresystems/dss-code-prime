// D-DIAG-OVERLAP-REFUSAL-CODE-NOT-DISCRIMINATING (cycle P65, lane dg) — the
// static-data producer's diagnostic CODES, pinned as a PARTITION.
//
// ★ WHAT THIS FILE EXISTS TO PIN, AND WHY IT IS NOT THE MESSAGE TESTS AGAIN.
// `lowerMirGlobalsToDataItems` used to pass `K_NoMatchingObjectFormat` at every
// one of its emit sites, and its `encodeAggregateValue` / `encodeBitIntImage` /
// `bitIntLiteralValue` recursion threaded a bare `std::string why` that carried
// prose and no identity. So a consumer that triages, filters or greps BY CODE
// could not tell "your initializer names members that share bytes" (the user
// edits the source) from "this target declares no `aggregateLayout`" (the user
// changes the target) from "the encoder and the layout authority disagree" (a
// DSS defect; the source is blameless). Only the message text discriminated,
// and message text is the least stable surface this project has —
// `tests/asm/test_asm_substrate.cpp` already pins the overlap refusal's WORDS,
// and every one of those substring assertions would survive the code silently
// collapsing back into the shared bucket.
//
// ⚠⚠ A CODE ASSERTION IS THE EASIEST TEST IN THIS REPOSITORY TO WRITE
// VACUOUSLY. `EXPECT_EQ(code, K_Whatever)` on a lowering that never ran, or on
// a reporter that recorded nothing, passes. Every fixture below therefore
// asserts FIRST that the refusal actually happened (`errors == 1`, `items`
// empty, exactly one diagnostic recorded) and only then what it is filed under;
// and `TheThreeClassesAreThreeCODES` compares the codes to EACH OTHER, so it
// reds if any one of them collapses into another rather than only if a
// hard-coded name changes.
//
// ★ THE CONTROL IS `TheResidualClassKeepsTheSharedCode`, AND IT IS NOT
// DECORATION. Splitting causes out of a shared bucket has an obvious wrong
// version — move everything — which would leave the code just as
// undiscriminating as before, pointing the other way. That test names a refusal
// that MUST still be `K_NoMatchingObjectFormat` (a target that declares no
// `aggregateLayout` block: no cause the encoder raises, no compiler defect,
// just a capability the format document does not have), so an over-eager
// re-key reds here while the overlap pin stays green.

#include "asm/asm.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "core/types/unsuppressable_codes.hpp"
#include "mir/mir_node.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace dss;

namespace {

// The shipped-target aggregate params (natural alignment, 16-byte ISA cap) —
// the same pair `tests/asm/test_asm_substrate.cpp` lowers its globals under.
constexpr AggregateLayoutParams kNatural16{ScalarAlignmentRule::Natural, 16};

struct Lowered {
    std::vector<AssembledData>  items;
    std::size_t                 errors = 0;
    std::string                 messages;
    std::vector<DiagnosticCode> codes;
};

// Lower exactly ONE module global of `type` initialized by `init` and report
// what came back. `layout` is deliberately an `optional` rather than defaulted:
// the residual-class control below needs the arm where the target declared NO
// `aggregateLayout` block at all, and that is a parameter value, not a mutation.
[[nodiscard]] Lowered lowerOneGlobal(TypeInterner const& ti, TypeId type,
                                     MirLiteralValue init,
                                     std::optional<AggregateLayoutParams> layout) {
    MirBuilder          b;
    std::uint32_t const lit = b.literalPoolAdd(std::move(init));
    b.addGlobal(type, SymbolId{1}, lit, MirFuncId{}, SymbolBinding::Global,
                SymbolVisibility::Default, /*isConst=*/false,
                MirThreadStorage::Shared);
    Mir const          m = std::move(b).finish();
    DiagnosticReporter rep;
    auto items = lowerMirGlobalsToDataItems(m, ti, layout, DataModel::Lp64, rep);

    Lowered out;
    out.items  = std::move(items);
    out.errors = rep.errorCount();
    for (auto const& d : rep.all()) {
        out.messages += d.actual;
        out.codes.push_back(d.code);
    }
    return out;
}

[[nodiscard]] MirLiteralValue intLeaf(std::int64_t v, TypeKind kind) {
    MirLiteralValue f;
    f.value = v;
    f.core  = kind;
    return f;
}

[[nodiscard]] MirLiteralValue doubleLeaf(double d) {
    MirLiteralValue f;
    f.value = d;
    f.core  = TypeKind::F64;
    return f;
}

[[nodiscard]] MirLiteralValue aggOf(std::vector<MirLiteralValue> fields,
                                    TypeKind                     core) {
    MirAggregateValue agg;
    agg.fields = std::move(fields);
    MirLiteralValue v;
    v.value = std::move(agg);
    v.core  = core;
    return v;
}

// `ULARGE_INTEGER`: `{u64 QuadPart @0, u32 LowPart @0, u32 HighPart @4}` — the
// shipped windows.json overlay shape. Members SHARE BYTES, so a positional
// member-wise write's result depends on declaration order.
[[nodiscard]] TypeId overlayStruct(TypeInterner& ti) {
    std::array<TypeId, 3> const        fields{ti.primitive(TypeKind::U64),
                                              ti.primitive(TypeKind::U32),
                                              ti.primitive(TypeKind::U32)};
    std::array<std::int64_t, 0> const  noWidths{};
    std::array<std::uint64_t, 3> const offsets{0, 0, 4};
    return ti.structType("ULARGE_INTEGER", fields, noWidths, offsets);
}

// A plain, naturally-laid-out `struct { int a; int b; }` — the one fixture here
// whose members do NOT share bytes. Used by the residual-class control so that
// its refusal cannot be the overlap refusal wearing a different hat.
[[nodiscard]] TypeId disjointStruct(TypeInterner& ti) {
    std::array<TypeId, 2> const        fields{ti.primitive(TypeKind::I32),
                                              ti.primitive(TypeKind::I32)};
    std::array<std::int64_t, 0> const  noWidths{};
    std::array<std::uint64_t, 0> const noOffsets{};
    return ti.structType("Disjoint", fields, noWidths, noOffsets);
}

// ── the three classes, each raised through the arm that really produces it ───

// CLASS 1 — the user edits the initializer. A non-zero leaf into the overlay:
// `LowPart = 1` aliases `QuadPart`'s low four bytes.
[[nodiscard]] Lowered raiseOverlapRefusal(TypeInterner& ti) {
    TypeId const s = overlayStruct(ti);
    EXPECT_TRUE(compositeFieldsOverlap(s, ti, kNatural16, DataModel::Lp64))
        << "fixture precondition: these members must ACTUALLY share bytes, or "
           "this test is measuring some other refusal";
    return lowerOneGlobal(ti, s,
                          aggOf({intLeaf(0, TypeKind::U64),
                                 intLeaf(1, TypeKind::U32),
                                 intLeaf(0, TypeKind::U32)},
                                TypeKind::Struct),
                          kNatural16);
}

// CLASS 2 — a DSS defect. A `_Complex` member whose literal is a SCALAR leaf:
// C 6.2.5p13 lays a complex out as exactly two components, so a one-value leaf
// cannot carry both, and no source construct produces that literal — reaching
// it means the MIR literal and the interned type have drifted.
// ⓘ Raised through a STRUCT so the top-level value is an aggregate and the
// recursion inside `encodeAggregateValue` is what finds it — the same path the
// overlap refusal takes, which is what makes the two comparable at all.
[[nodiscard]] Lowered raiseEncoderInvariantBreach(TypeInterner& ti) {
    TypeId const cplx = ti.complex(ti.primitive(TypeKind::F64));
    std::array<TypeId, 1> const        fields{cplx};
    std::array<std::int64_t, 0> const  noWidths{};
    std::array<std::uint64_t, 0> const noOffsets{};
    TypeId const outer = ti.structType("HoldsAComplex", fields, noWidths, noOffsets);
    return lowerOneGlobal(ti, outer, aggOf({doubleLeaf(1.0)}, TypeKind::Struct),
                          kNatural16);
}

// CLASS 3 — the target is missing a capability. An aggregate global against a
// target that declares no `aggregateLayout` block: nothing about the source is
// wrong and nothing inside this compiler disagrees with itself; there is simply
// no ABI to lay the object out with.
[[nodiscard]] Lowered raiseResidualCapabilityGap(TypeInterner& ti) {
    return lowerOneGlobal(ti, disjointStruct(ti),
                          aggOf({intLeaf(1, TypeKind::I32),
                                 intLeaf(2, TypeKind::I32)},
                                TypeKind::Struct),
                          std::nullopt);
}

// One refusal, one diagnostic, no bytes — asserted before any code is read so a
// fixture that stopped refusing cannot pass by recording nothing.
void expectExactlyOneRefusal(Lowered const& r, char const* what) {
    ASSERT_EQ(r.errors, 1u) << what << ": expected exactly one refusal, got "
                            << r.errors << " — " << r.messages;
    ASSERT_TRUE(r.items.empty())
        << what << ": a refused initializer must emit NO data item, least of "
                   "all a partial one";
    ASSERT_EQ(r.codes.size(), 1u)
        << what << ": expected exactly one diagnostic — " << r.messages;
}

}  // namespace

// ── THE ROW'S SUBJECT ────────────────────────────────────────────────────────
//
// RED-ON-DISABLE (REMOVE direction): change the overlap arm's `why.set` in
// `encodeAggregateValue` back to `K_NoMatchingObjectFormat` — or drop the
// `codeOr` hand-off at the aggregate render arm in `lowerMirGlobalsToDataItems`
// so the arm hard-codes the shared code again — and this test fails by name,
// as does `TheThreeClassesAreThreeCODES`, while
// `TheResidualClassKeepsTheSharedCode` stays green.
TEST(StaticDataDiagnosticCodes, OverlapRefusalCarriesItsOwnCode) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const   r = raiseOverlapRefusal(ti);
    expectExactlyOneRefusal(r, "overlapping explicit-offset static init");

    EXPECT_EQ(r.codes[0], DiagnosticCode::K_OverlappingStaticInitUnsupported)
        << "the overlap cause must be filed under its OWN code — a consumer "
           "triaging by code cannot reach the message: " << r.messages;
    EXPECT_NE(r.codes[0], DiagnosticCode::K_NoMatchingObjectFormat)
        << "and specifically NOT under the code it shared with every other arm "
           "of lowerMirGlobalsToDataItems";
    // The message must still say what it always said: the split was to make the
    // cause addressable, never to move the explanation into the code's name.
    EXPECT_NE(r.messages.find("its members share bytes; assign the members "
                              "individually"),
              std::string::npos)
        << "the remedy stays IN the message: " << r.messages;
}

// ── THE PARTITION ITSELF ─────────────────────────────────────────────────────
//
// The property the row is about is not "the overlap cause has a code" — it is
// that these causes are SEPARABLE. Comparing the three to each other says that
// directly, and it reds on a collapse in EITHER direction: an arm that reverts
// to the shared code, or a re-key that swept a cause into the wrong class.
TEST(StaticDataDiagnosticCodes, TheThreeClassesAreThreeCODES) {
    TypeInterner ti{CompilationUnitId{1}};

    auto const overlap = raiseOverlapRefusal(ti);
    expectExactlyOneRefusal(overlap, "class 1 (user edits the initializer)");
    auto const breach = raiseEncoderInvariantBreach(ti);
    expectExactlyOneRefusal(breach, "class 2 (a DSS defect)");
    auto const residual = raiseResidualCapabilityGap(ti);
    expectExactlyOneRefusal(residual, "class 3 (target capability gap)");

    EXPECT_NE(overlap.codes[0], breach.codes[0])
        << "a source the user can fix and a defect the user cannot must not "
           "arrive under one code: " << diagnosticCodeName(overlap.codes[0]);
    EXPECT_NE(overlap.codes[0], residual.codes[0])
        << "a shared-bytes initializer and a missing target capability must "
           "not arrive under one code: " << diagnosticCodeName(overlap.codes[0]);
    EXPECT_NE(breach.codes[0], residual.codes[0])
        << "a compiler defect and a missing target capability must not arrive "
           "under one code: " << diagnosticCodeName(breach.codes[0]);

    EXPECT_EQ(overlap.codes[0], DiagnosticCode::K_OverlappingStaticInitUnsupported);
    EXPECT_EQ(breach.codes[0], DiagnosticCode::K_StaticDataEncoderInvariantBreach);
    EXPECT_EQ(residual.codes[0], DiagnosticCode::K_NoMatchingObjectFormat);
}

// ── THE CONTROL ──────────────────────────────────────────────────────────────
//
// Non-vacuous and deliberately NOT a mirror of the pins above: it names the arm
// that must NOT have moved. A re-key that swept the whole function onto new
// codes reds HERE while every specific pin above stays green.
TEST(StaticDataDiagnosticCodes, TheResidualClassKeepsTheSharedCode) {
    TypeInterner ti{CompilationUnitId{1}};
    auto const   r = raiseResidualCapabilityGap(ti);
    expectExactlyOneRefusal(r, "aggregate global, no aggregateLayout declared");

    EXPECT_EQ(r.codes[0], DiagnosticCode::K_NoMatchingObjectFormat)
        << "a target-capability gap is what K_NoMatchingObjectFormat MEANS; "
           "splitting causes out of it must not empty it: " << r.messages;
    EXPECT_NE(r.messages.find("declared no `aggregateLayout` block"),
              std::string::npos)
        << "fixture precondition: this must be the capability arm, not some "
           "other refusal that also kept the shared code: " << r.messages;
}

// ── THE PROPERTY THAT MAKES A CODE WORTH MINTING ─────────────────────────────
//
// A code a consumer cannot NAME is a code it cannot grep for, and this project
// renders the symbolic name (not the hex) on every diagnostic surface. A new
// enumerator with no `diagnosticCodeName` arm renders under the unallocated
// marker, which would leave the row's defect exactly where it was while every
// assertion above still passed.
TEST(StaticDataDiagnosticCodes, BothNewCodesAreNameable) {
    for (auto const code : {DiagnosticCode::K_OverlappingStaticInitUnsupported,
                            DiagnosticCode::K_StaticDataEncoderInvariantBreach}) {
        auto const name = diagnosticCodeName(code);
        EXPECT_NE(name, kUnallocatedDiagnosticCodeName)
            << "an allocated code that renders as unallocated is ungreppable, "
               "which is the whole defect this row closed";
        ASSERT_FALSE(name.empty());
        EXPECT_EQ(name.front(), 'K')
            << "a static-data producer code belongs to the K band: " << name;
    }
}

// ── THE DELIBERATE SUPPRESSION DECISION ──────────────────────────────────────
//
// ⚠ THIS IS NOT PAPERWORK, AND IT IS NOT INHERITED. The parent
// `K_NoMatchingObjectFormat` is unsuppressable because silencing it lets the
// LINKER dispatch the wrong format walker — a rationale neither of these codes
// can reach, since both fire from the static-DATA producer upstream of any
// walker. Their membership is re-derived from their own control flow: every
// site that emits them is followed by a `continue` that skips the loop body's
// `out.push_back`, so the refused global contributes NO `AssembledData`.
// Silenced, `errorCount()` reads zero and the artifact ships with that object's
// initializer bytes absent — prong (1), established on this tier's mechanism.
// The rationale string is asserted non-empty because a member with no reason
// makes a `--suppress` request fail with an empty explanation.
TEST(StaticDataDiagnosticCodes, BothNewCodesAreUnsuppressableWithARecordedReason) {
    for (auto const code : {DiagnosticCode::K_OverlappingStaticInitUnsupported,
                            DiagnosticCode::K_StaticDataEncoderInvariantBreach}) {
        EXPECT_TRUE(isUnsuppressable(code))
            << diagnosticCodeName(code)
            << ": suppressed, the global's data item is dropped by the "
               "`continue` that follows the refusal and the build reports "
               "success with the object's bytes missing from the artifact";
        EXPECT_FALSE(unsuppressableRationale(code).empty())
            << diagnosticCodeName(code)
            << " is a member with no recorded reason, so a --suppress request "
               "naming it would be refused with an empty explanation";
    }
}
