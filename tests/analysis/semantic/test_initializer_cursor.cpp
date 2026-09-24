// ===========================================================================
// P68 round 9 (lane `cs`) — THE CURRENT OBJECT OF A BRACE LIST, AS A PURE CURSOR
// (src/analysis/semantic/initializer_cursor.hpp).
//
// C 6.7.9p17-p20: an element initializes "the next subobject" of the list's current
// object, or the subobject its designation names, and initialization continues
// "beginning with the next subobject after that described by the designator"; an
// aggregate subobject whose initializer does not begin with a brace takes "only
// enough initializers from the list" for its own members (BRACE ELISION); a union
// takes one member's worth and is complete. The cursor is the one owner of that
// placement for both tiers — the semantic tier sizes an array of unknown size with
// it, the HIR tier places every element with it — so it is pinned here against a
// synthetic type table, with no front end in the way.
//
// Every placement below is C's, ✔MEASURED 2026-09-23 on each reference separately
// with every program RUN (gcc 13.3.0 and clang 18.1.3 at `-std=c17 -pedantic-errors`
// and `-std=c2x`, mingw-w64 13.2.0 at both, MSVC 19.51 at `/std:c17` and
// `/std:clatest`; the lane's `.temp/probe/r9`, `r9b`): each case's program returns 42
// exactly when the elements land where these paths say.
//
// RED-ON-DISABLE: elision off (the value meets the aggregate as it is) → the elided
// paths; the union not completing after one member → the union cases; the unnamed
// bit-field not skipped → the bit-field case; the continuation after a designator
// resuming at the top level → the deep-designator cases.
// ===========================================================================

#include "analysis/semantic/initializer_cursor.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::initializer_cursor;

namespace {

// A synthetic type table: a TypeId is an index into `types` (0 is invalid).
class FakeView final : public TypeView {
public:
    struct Type {
        Shape                        shape = Shape::Scalar;
        std::optional<std::uint32_t> count;     // members / elements; nullopt = unknown length
        std::vector<TypeId>          members;   // an array's element type is members[0]
        std::vector<bool>            unnamedBitField;
    };

    FakeView() { types_.push_back({}); }   // TypeId{0} is the invalid id

    TypeId scalar() { return add({Shape::Scalar, std::nullopt, {}, {}}); }
    TypeId array(TypeId elem, std::optional<std::uint32_t> n) {
        return add({Shape::Array, n, {elem}, {}});
    }
    TypeId record(Shape shape, std::vector<TypeId> members, std::vector<bool> unnamed = {}) {
        auto const n = static_cast<std::uint32_t>(members.size());
        if (unnamed.empty()) unnamed.assign(n, false);
        return add({shape, n, std::move(members), std::move(unnamed)});
    }

    [[nodiscard]] Shape shape(TypeId t) const override { return types_[t.v].shape; }
    [[nodiscard]] std::optional<std::uint32_t> count(TypeId t) const override {
        return types_[t.v].count;
    }
    [[nodiscard]] TypeId member(TypeId t, std::uint32_t i) const override {
        Type const& ty = types_[t.v];
        return ty.shape == Shape::Array ? ty.members[0] : ty.members[i];
    }
    [[nodiscard]] bool unnamedBitField(TypeId t, std::uint32_t i) const override {
        Type const& ty = types_[t.v];
        return ty.shape != Shape::Array && i < ty.unnamedBitField.size()
            && ty.unnamedBitField[i];
    }

private:
    TypeId add(Type t) {
        types_.push_back(std::move(t));
        return TypeId{static_cast<std::uint32_t>(types_.size() - 1)};
    }
    std::vector<Type> types_;
};

// One element of a list: its designation (empty = positional), whether its value is
// itself a brace list, and — for the whole-initialization question — the type its
// value has (InvalidType for a scalar value, which initializes no aggregate whole).
struct Elem {
    std::vector<std::uint32_t> designator;
    bool                       braceList = false;
    TypeId                     valueType{};
};

// Place every element; render each placement as "a.b.c", "EXCESS" or "OUT".
std::vector<std::string> placeAll(Cursor& c, std::vector<Elem> const& elems) {
    std::vector<std::string> out;
    for (Elem const& e : elems) {
        Placement const p = c.place(std::span<std::uint32_t const>{e.designator}, e.braceList,
                                    [&](TypeId t) { return e.valueType.valid() && e.valueType == t; });
        if (p.kind == Placement::Kind::Excess) { out.push_back("EXCESS"); continue; }
        if (p.kind == Placement::Kind::OutOfRange) { out.push_back("OUT"); continue; }
        std::string s;
        for (std::size_t i = 0; i < p.path.size(); ++i) {
            if (i != 0) s += '.';
            s += std::to_string(p.path[i]);
        }
        out.push_back(s);
    }
    return out;
}

Elem pos() { return Elem{}; }
Elem posValue(TypeId t) { return Elem{{}, false, t}; }
Elem brace() { return Elem{{}, true, {}}; }
Elem at(std::vector<std::uint32_t> d) { return Elem{std::move(d), false, {}}; }

using V = std::vector<std::string>;

}  // namespace

// `int a[2][2] = {1, 2, 3, 40}` and one element too many.
TEST(InitializerCursor, RowsAreElidedInOrderAndTheFifthIsExcess) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const a = v.array(v.array(i, 2), 2);
    Cursor c{a, v};
    EXPECT_EQ(placeAll(c, {pos(), pos(), pos(), pos(), pos()}),
              (V{"0.0", "0.1", "1.0", "1.1", "EXCESS"}));
    EXPECT_EQ(c.extent(), 2);
}

// `int a[][2] = {1, 2, 3, 40}` — an array of unknown size is TWO rows, not four.
TEST(InitializerCursor, AnUnknownSizeCountsTheRowsElisionFilled) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const a = v.array(v.array(i, 2), std::nullopt);
    Cursor c{a, v};
    EXPECT_EQ(placeAll(c, {pos(), pos(), pos(), pos()}), (V{"0.0", "0.1", "1.0", "1.1"}));
    EXPECT_EQ(c.extent(), 2);
    Cursor odd{a, v};
    (void)placeAll(odd, {pos(), pos(), pos()});
    EXPECT_EQ(odd.extent(), 2) << "a partly filled last row still counts";
}

// `struct P ps[] = {1, 2, 3, 39}`, and `{q, 1, 1}` where `q` IS a `struct P`: a
// compatible structure value initializes the element whole and stops the elision.
TEST(InitializerCursor, StructElementsAreElidedUnlessTheValueIsTheStructure) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const p = v.record(Shape::Struct, {i, i});
    TypeId const ps = v.array(p, std::nullopt);
    Cursor c{ps, v};
    EXPECT_EQ(placeAll(c, {pos(), pos(), pos(), pos()}), (V{"0.0", "0.1", "1.0", "1.1"}));
    EXPECT_EQ(c.extent(), 2);
    Cursor q{ps, v};
    EXPECT_EQ(placeAll(q, {posValue(p), pos(), pos()}), (V{"0", "1.0", "1.1"}));
    EXPECT_EQ(q.extent(), 2);
}

// `int a[][2] = {[1][0] = 1, 41}`: the continuation after a DEEP designator is the
// next subobject INSIDE the designated row.
TEST(InitializerCursor, ADeepDesignatorContinuesInsideItsAggregate) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const a = v.array(v.array(i, 2), std::nullopt);
    Cursor c{a, v};
    EXPECT_EQ(placeAll(c, {at({1, 0}), pos(), pos()}), (V{"1.0", "1.1", "2.0"}));
    EXPECT_EQ(c.extent(), 3);
}

// `struct O o = {.i.a = 30, 12}` puts 12 in `o.i.b`; `{.i.b = 30, 12}` leaves `i` and
// puts it in `o.c`; `struct T t = {.a[1] = 40, 2}` leaves the array for `t.b`.
TEST(InitializerCursor, AMemberDesignatorContinuesInsideThenLeaves) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const inner = v.record(Shape::Struct, {i, i});
    TypeId const o = v.record(Shape::Struct, {inner, i});
    Cursor c1{o, v};
    EXPECT_EQ(placeAll(c1, {at({0, 0}), pos(), pos(), pos()}), (V{"0.0", "0.1", "1", "EXCESS"}));
    Cursor c2{o, v};
    EXPECT_EQ(placeAll(c2, {at({0, 1}), pos()}), (V{"0.1", "1"}));
    TypeId const t = v.record(Shape::Struct, {v.array(i, 2), i});
    Cursor c3{t, v};
    EXPECT_EQ(placeAll(c3, {at({0, 1}), pos()}), (V{"0.1", "1"}));
}

// A union takes ONE member's worth: `union U {struct {int a, b;} s; int i;} u = {40,
// 2}` fills `s` by elision and then is complete; `struct S {union {int i; char c;} u;
// int k;} s = {.u.i = 40, 2}` puts 2 in `k`; `struct S s = {2, 40}` elides into the
// union's first member and continues with `k`.
TEST(InitializerCursor, AUnionIsCompleteAfterOneMember) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const s2 = v.record(Shape::Struct, {i, i});
    TypeId const u = v.record(Shape::Union, {s2, i});
    Cursor c1{u, v};
    EXPECT_EQ(placeAll(c1, {pos(), pos(), pos()}), (V{"0.0", "0.1", "EXCESS"}));
    TypeId const inner = v.record(Shape::Union, {i, i});
    TypeId const s = v.record(Shape::Struct, {inner, i});
    Cursor c2{s, v};
    EXPECT_EQ(placeAll(c2, {at({0, 0}), pos()}), (V{"0.0", "1"}));
    Cursor c3{s, v};
    EXPECT_EQ(placeAll(c3, {pos(), pos(), pos()}), (V{"0.0", "1", "EXCESS"}));
}

// `union V {int i; float f;} v = {.f = 1.0f, .i = 42}` — each designator places; the
// lowering keeps the LAST (C 6.7.9p19). A positional element after either is excess.
TEST(InitializerCursor, EveryDesignatorOfAUnionListPlaces) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const u = v.record(Shape::Union, {i, i});
    Cursor c{u, v};
    EXPECT_EQ(placeAll(c, {at({1}), at({0}), pos()}), (V{"1", "0", "EXCESS"}));
}

// `struct B {int a; unsigned : 3; int b;} bs[] = {40, 2, 7, 8}` — the unnamed
// bit-field is skipped by positional initialization, elided or not.
TEST(InitializerCursor, AnUnnamedBitFieldIsSkipped) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const b = v.record(Shape::Struct, {i, i, i}, {false, true, false});
    Cursor c{v.array(b, std::nullopt), v};
    EXPECT_EQ(placeAll(c, {pos(), pos(), pos(), pos()}), (V{"0.0", "0.2", "1.0", "1.2"}));
    EXPECT_EQ(c.extent(), 2);
}

// A brace list initializes whatever the position names — the elided scalar `a[0][1]`
// in `{1, {2}, 3, 36}`, the whole row `a[1]` in `{1, 2, {3, 4}, 32}`.
TEST(InitializerCursor, ABraceListTakesThePositionAsItIs) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const a22 = v.array(v.array(i, 2), 2);
    Cursor c1{a22, v};
    EXPECT_EQ(placeAll(c1, {pos(), brace(), pos(), pos()}), (V{"0.0", "0.1", "1.0", "1.1"}));
    TypeId const a32 = v.array(v.array(i, 2), 3);
    Cursor c2{a32, v};
    EXPECT_EQ(placeAll(c2, {pos(), pos(), brace(), pos()}), (V{"0.0", "0.1", "1", "2.0"}));
}

// `struct V {char s[4]; int x;} v[] = {"ab", 40, "cd", 1}` — a string initializes the
// character array whole (the caller's whole-initialization answer), so it is NOT
// elided into `s[0]`.
TEST(InitializerCursor, AStringInitializesItsCharacterArrayWhole) {
    FakeView v;
    TypeId const ch = v.scalar();
    TypeId const i = v.scalar();
    TypeId const s4 = v.array(ch, 4);
    TypeId const rec = v.record(Shape::Struct, {s4, i});
    Cursor c{v.array(rec, std::nullopt), v};
    EXPECT_EQ(placeAll(c, {posValue(s4), pos(), posValue(s4), pos()}),
              (V{"0.0", "0.1", "1.0", "1.1"}));
    EXPECT_EQ(c.extent(), 2);
}

// A designator that names no subobject is OUT OF RANGE at any depth, and the cursor
// does not move; a designator past a bounded end is never "excess".
TEST(InitializerCursor, ADesignatorPastTheEndIsOutOfRangeAtAnyDepth) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const a22 = v.array(v.array(i, 2), 2);
    Cursor c{a22, v};
    EXPECT_EQ(placeAll(c, {at({5}), at({0, 5}), at({0, 0, 0}), pos()}),
              (V{"OUT", "OUT", "OUT", "0.0"}));
}

// An aggregate with no positional member stops the elision: the value meets it as it
// is (the lowering judges that), rather than the cursor inventing a path.
TEST(InitializerCursor, AnAggregateWithNoPositionalMemberStopsTheElision) {
    FakeView v;
    TypeId const i = v.scalar();
    TypeId const onlyUnnamed = v.record(Shape::Struct, {i}, {true});
    TypeId const flex = v.record(Shape::Struct, {i, v.array(i, std::nullopt)});
    Cursor c1{v.record(Shape::Struct, {onlyUnnamed, i}), v};
    EXPECT_EQ(placeAll(c1, {pos(), pos()}), (V{"0", "1"}));
    Cursor c2{flex, v};
    EXPECT_EQ(placeAll(c2, {pos(), pos(), pos()}), (V{"0", "1", "EXCESS"}));
}
