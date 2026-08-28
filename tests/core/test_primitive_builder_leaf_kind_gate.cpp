// D-LATTICE-PRIMITIVE-BUILDER-ACCEPTS-A-NON-PRIMITIVE-KIND
//
// `TypeInterner::primitive(kind)` interned WHATEVER kind it was handed, with
// empty operands, empty scalars and no name — so `primitive(TypeKind::Struct)`
// minted a fieldless "struct" record and `primitive(TypeKind::Complex)` an
// element-less "complex". Both are well-formed `TypeRecord`s that fail
// generations later, at a layout query or a `complexElement` decode, naming a
// consumer that did nothing wrong. `isPrimitiveTypeKind` existed and was
// applied — by the config loader, to ONE of this builder's callers.
//
// ★ WHY THIS FILE EXISTS AT ALL. The gap was ✔MEASURED as NOT a live defect:
// every `primitive(TypeKind::X)` LITERAL site in `src/` passes a leaf kind. A
// guard whose refusal cannot be shown to fire is worse than no guard — it reads
// as protection and provides none — so the deliverable here is the FIRING, in
// both directions: every non-leaf kind aborts with a sentence that names it,
// and every leaf kind still interns.
//
// ★★ THE EXPECTATIONS ARE HAND-WRITTEN, NOT PROJECTED. Neither list below may
// be read off `kTypeKindNameTable` or off `isPrimitiveTypeKind` — a pin whose
// expected set comes from the same predicate the code consults moves BOTH
// HALVES TOGETHER, and a kind silently changing sides would keep this file
// green. Every set here is a literal, every count is a literal, and the two
// lists are asserted DISJOINT and TOTAL so a new enumerator cannot land in
// neither and leave both arms quietly incomplete.

#include "core/types/strong_ids.hpp"
#include "core/types/type_lattice/type_interner.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;

namespace {

[[nodiscard]] TypeInterner makeInterner(std::uint32_t owner) {
    return TypeInterner{CompilationUnitId{owner}};
}

// The TWENTY kinds `primitive` must REFUSE, with the spelling its sentence must
// print. Hand-written; the reason each one is not leaf-rebuildable is the same
// reason `isPrimitiveTypeKind` states beside the enum, restated here so this
// file can be read without it.
const std::vector<std::pair<TypeKind, std::string_view>> kNonLeafKinds{
    {TypeKind::Struct,       "Struct"},        // fields + nominal name
    {TypeKind::Union,        "Union"},         // fields + nominal name
    {TypeKind::Tuple,        "Tuple"},         // element operands
    {TypeKind::Array,        "Array"},         // element + length scalar
    {TypeKind::Slice,        "Slice"},         // element operand
    {TypeKind::Enum,         "Enum"},          // name + underlying scalar
    {TypeKind::Vector,       "Vector"},        // element + lane scalar
    {TypeKind::Matrix,       "Matrix"},        // element + row/col scalars
    {TypeKind::Ptr,          "Ptr"},           // pointee operand
    {TypeKind::Ref,          "Ref"},           // referent operand
    {TypeKind::FnPtr,        "FnPtr"},         // signature operand
    {TypeKind::Nullable,     "Nullable"},      // inner operand
    {TypeKind::Optional,     "Optional"},      // inner operand
    {TypeKind::FnSig,        "FnSig"},         // params + result
    {TypeKind::Param,        "Param"},         // type operand
    {TypeKind::Bind,         "Bind"},          // bound operand
    {TypeKind::Extension,    "Extension"},     // registry-minted kindId
    {TypeKind::VolatileQual, "VolatileQual"},  // inner + qualifier bitset
    {TypeKind::BitInt,       "BitInt"},        // width + signedness scalars
    {TypeKind::Complex,      "Complex"},       // element float operand
};

// The TWENTY kinds `primitive` must ACCEPT — the INVERSE arm, and the reason
// this guard is not an always-on refusal wearing a check's costume.
const std::vector<std::pair<TypeKind, std::string_view>> kLeafKinds{
    {TypeKind::Bool,     "Bool"},
    {TypeKind::I8,       "I8"},    {TypeKind::I16,  "I16"},
    {TypeKind::I32,      "I32"},   {TypeKind::I64,  "I64"},
    {TypeKind::I128,     "I128"},
    {TypeKind::U8,       "U8"},    {TypeKind::U16,  "U16"},
    {TypeKind::U32,      "U32"},   {TypeKind::U64,  "U64"},
    {TypeKind::U128,     "U128"},
    {TypeKind::F16,      "F16"},   {TypeKind::F32,  "F32"},
    {TypeKind::F64,      "F64"},   {TypeKind::F80,  "F80"},
    {TypeKind::F128,     "F128"},
    {TypeKind::Char,     "Char"},  {TypeKind::Byte, "Byte"},
    {TypeKind::Void,     "Void"},
    {TypeKind::NullptrT, "NullptrT"},  // C23 nullptr_t — an operand-less scalar
};

} // namespace

// ── THE TWO LISTS ARE DISJOINT AND TOTAL ────────────────────────────────────
//
// ★ THE ANTI-VACUITY ARM, and it is the one that keeps the other two honest. A
// new enumerator that lands in NEITHER list would leave both arms passing while
// saying nothing about it — the shape of a green suite that has stopped
// asserting. 20 + 20 == the enum's cardinality, checked as literals, so adding a
// kind reds HERE until somebody decides which side it belongs on. That is the
// same protection `isPrimitiveTypeKind`'s missing `default:` gives the compiler,
// restated for the pin.
TEST(PrimitiveBuilderLeafKindGate, TheTwoListsPartitionTheEnum) {
    ASSERT_EQ(kNonLeafKinds.size(), 20u);
    ASSERT_EQ(kLeafKinds.size(), 20u);
    // A literal, NOT `kTypeKindNameTable.rows.size()`: reading the count off the
    // table would move with it and assert nothing.
    ASSERT_EQ(static_cast<std::uint32_t>(TypeKind::Count_), 40u)
        << "a TypeKind was added or removed: decide which of this file's two "
           "lists it belongs in before changing this number";

    std::set<std::uint32_t> seen;
    for (auto const& [k, name] : kNonLeafKinds) {
        EXPECT_TRUE(seen.insert(static_cast<std::uint32_t>(k)).second)
            << "duplicate in kNonLeafKinds: " << name;
    }
    for (auto const& [k, name] : kLeafKinds) {
        EXPECT_TRUE(seen.insert(static_cast<std::uint32_t>(k)).second)
            << "in BOTH lists (or duplicated): " << name;
    }
    EXPECT_EQ(seen.size(), 40u);   // total coverage of [0, Count_)
    // Every ordinal below Count_ is covered — a gap would mean an enumerator is
    // in neither list even though the sizes add up (two entries for one kind).
    for (std::uint32_t i = 0; i < 40u; ++i) {
        // `.contains`, not `count(...) == 1`: `seen` is a `std::set`, where
        // `count` is 0-or-1 by definition, so `== 1` reads as "exactly once"
        // and asserts only membership. The duplicate half of the claim is
        // already carried by the `insert(...).second` loops above and by the
        // size check — this loop's job is the GAP, and that is what it says now.
        EXPECT_TRUE(seen.contains(i))
            << "TypeKind ordinal " << i << " is in neither list";
    }
}

// ── THE REFUSAL FIRES, PER KIND ─────────────────────────────────────────────
//
// One arm per non-leaf kind rather than one loop inside one expectation: a
// death test ends at the FIRST abort, so a loop would prove the guard fires for
// `Struct` and assert nothing about the other nineteen — an existence claim
// wearing a universal claim's clothes.
//
// ⚠ The matcher is a plain substring with no regex metacharacters: gtest's death
// matcher is POSIX ERE on Linux and gtest's own simple regex on Windows, and the
// message's parenthesised anchor citation would mean different things to the two.
class PrimitiveBuilderRefusalDeathTest
    : public ::testing::TestWithParam<std::pair<TypeKind, std::string_view>> {};

TEST_P(PrimitiveBuilderRefusalDeathTest, NonLeafKindAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto const [kind, name] = GetParam();
    std::string const expected =
        "TypeKind " + std::string{name} + " is not a LEAF kind";
    // The 1-arg overload — the spelling ~93 literal call sites in `src/` use.
    EXPECT_DEATH({ auto ti = makeInterner(1); (void)ti.primitive(kind); },
                 expected);
    // ★ AND THE 2-ARG OVERLOAD, because the guard is a SINGLE line and the claim
    // "one site covers both" is a claim about delegation, not a fact about the
    // API. If the 1-arg overload ever stops delegating, this arm is what says so.
    EXPECT_DEATH({ auto ti = makeInterner(1); (void)ti.primitive(kind, "tag"); },
                 expected);
}

INSTANTIATE_TEST_SUITE_P(
    AllTwentyNonLeafKinds, PrimitiveBuilderRefusalDeathTest,
    ::testing::ValuesIn(kNonLeafKinds),
    [](::testing::TestParamInfo<std::pair<TypeKind, std::string_view>> const& i) {
        return std::string{i.param.second};
    });

// ── THE SENTENCE ITSELF ─────────────────────────────────────────────────────
//
// ⚠ A sibling lane closed a row today because a refusal printed `lvalue kind
// ordinal 30`. This one must name the kind BY NAME and the accepted set through
// the ONE renderer, so the reader can act on it without a `TypeKind` header
// open. The accepted spellings are asserted as LITERALS — reading them off
// `namesWhere(kTypeKindNameTable, isPrimitiveTypeKind)` would be the projection
// the message already performs, restated, and would stay green through a rename.
TEST(PrimitiveBuilderLeafKindGateDeathTest, MessageNamesTheKindAndTheAcceptedSet) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto trigger = [] {
        auto ti = makeInterner(1);
        (void)ti.primitive(TypeKind::Struct);
    };
    EXPECT_DEATH({ trigger(); }, "TypeKind Struct is not a LEAF kind");
    // It says WHY, in terms of the record it would otherwise have minted.
    EXPECT_DEATH({ trigger(); }, "NO operands, NO scalars and NO name");
    // It names the accepted set — first row, last row, and the one the old
    // prose comment in `type_interner.hpp` was missing.
    EXPECT_DEATH({ trigger(); }, "'Bool'");
    EXPECT_DEATH({ trigger(); }, "'Void'");
    EXPECT_DEATH({ trigger(); }, "'NullptrT'");
    // It points at the fix, not just at the failure.
    EXPECT_DEATH({ trigger(); }, "isPrimitiveTypeKind");
    // And it cites the anchor, un-wrapped.
    EXPECT_DEATH({ trigger(); },
                 "D-LATTICE-PRIMITIVE-BUILDER-ACCEPTS-A-NON-PRIMITIVE-KIND");
}

// ── A KIND WITH NO SPELLING SAYS SO WITH ITS ORDINAL ────────────────────────
//
// `Count_` is the enum-cardinality sentinel and is deliberately unlisted in
// `kTypeKindNameTable`, so `typeKindNameOrEmpty` answers EMPTY — and a naive
// `name()` would answer it with row 0's spelling, reporting `Bool` (a perfectly
// legal kind) as the offender. The ordinal form covers an out-of-range cast with
// the same sentence, and is the sentence `reinternFatal` already uses.
TEST(PrimitiveBuilderLeafKindGateDeathTest, UnspelledKindReportsItsOrdinal) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    // A literal 40: it is `Count_`'s ordinal today, and this arm SHOULD red when
    // a kind is appended — the partition test above is where that gets decided.
    EXPECT_DEATH(
        { auto ti = makeInterner(1); (void)ti.primitive(TypeKind::Count_); },
        "TypeKind <unnamed kind #40> is not a LEAF kind");
    // An ordinal outside the enum entirely (a bad cast, a corrupt scalar read
    // back through `static_cast<TypeKind>(scalars(t)[0])`).
    EXPECT_DEATH(
        {
            auto ti = makeInterner(1);
            (void)ti.primitive(static_cast<TypeKind>(200));
        },
        "TypeKind <unnamed kind #200> is not a LEAF kind");
    // ⚠ NOT the row-0 spelling. If the guard ever reaches for `name()` instead of
    // `nameOrEmpty`, the sentence names `Bool` and this is what catches it.
    EXPECT_DEATH(
        { auto ti = makeInterner(1); (void)ti.primitive(TypeKind::Count_); },
        "unnamed kind");
}

// ── THE INVERSE: EVERY LEAF KIND STILL INTERNS ──────────────────────────────
//
// ★ THIS IS THE ARM THAT MAKES THE REFUSAL A CHECK RATHER THAN A WALL. A guard
// that refuses everything passes every death test above and is a catastrophe;
// only this arm can tell the two apart. It asserts the accepted case in full —
// the record is built, its kind round-trips, and it is genuinely operand-less
// and scalar-less, which is the property that made it leaf-rebuildable.
TEST(PrimitiveBuilderLeafKindGate, EveryLeafKindStillInterns) {
    auto ti = makeInterner(1);
    ASSERT_EQ(kLeafKinds.size(), 20u);
    std::set<std::uint32_t> ids;
    for (auto const& [kind, name] : kLeafKinds) {
        const TypeId id = ti.primitive(kind);
        ASSERT_TRUE(id.valid()) << name;
        EXPECT_EQ(ti.kind(id), kind) << name;
        EXPECT_TRUE(ti.operands(id).empty()) << name;
        EXPECT_TRUE(ti.scalars(id).empty()) << name;
        EXPECT_TRUE(ids.insert(id.v).second)
            << name << " collided with an earlier leaf kind";
    }
    // Twenty distinct types and not one more — the guard did not silently drop
    // or merge an accepted kind on its way through.
    EXPECT_EQ(ids.size(), 20u);
    EXPECT_EQ(ti.size(), 20u);
}

// The 2-arg overload's accepted path is intact too, including the two identity
// properties the guard sits directly on top of: an EMPTY tag must intern
// bit-identically to the 1-arg overload (D-LANG-TYPE-IDENTITY-VOCABULARY), and a
// non-empty tag must stay distinct. A guard placed after the name short-circuit
// instead of before it would still pass the death arms and break these.
TEST(PrimitiveBuilderLeafKindGate, EveryLeafKindStillInternsNamedAndAnonymous) {
    auto ti = makeInterner(1);
    ASSERT_EQ(kLeafKinds.size(), 20u);
    for (auto const& [kind, name] : kLeafKinds) {
        const TypeId anon      = ti.primitive(kind);
        const TypeId anonAgain = ti.primitive(kind, {});
        const TypeId tagged    = ti.primitive(kind, "vocab");
        const TypeId taggedAgain = ti.primitive(kind, "vocab");
        EXPECT_EQ(anon.v, anonAgain.v) << name << ": empty tag must be anonymous";
        EXPECT_EQ(tagged.v, taggedAgain.v) << name << ": tagged must dedup";
        EXPECT_NE(anon.v, tagged.v) << name << ": tag must carry identity";
        EXPECT_EQ(ti.kind(tagged), kind) << name;
    }
}
