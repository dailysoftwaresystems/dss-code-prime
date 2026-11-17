// P69 L2b2 (lane `lm`): the ONE home of "a test names a row of a shipped document by its
// key" — `shipped_row_pointer.hpp`. Each selector form is pinned in both directions: the
// row it names, and the THROW where it names none or (a key selector) more than one,
// because a resolver that answered "some other row" would put back exactly the silent
// mis-aim it exists to remove.

#include "shipped_row_pointer.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dss::test_support::resolveRowPointer;
using dss::test_support::resolveRowPointers;
using dss::test_support::withSelectedIndex;

nlohmann::json const& doc() {
    static nlohmann::json const d = nlohmann::json::parse(R"JSON({
      "rows": [
        { "rule": "a" },
        { "rule": "b", "x": { "deep": [10, 20] } },
        { "rule": "c", "x": { "deep": [30] } },
        { "rule": "twin" },
        { "rule": "twin" },
        "not an object"
      ],
      "map": { "k": 1 }
    })JSON");
    return d;
}

// Rows 0, 1 and 2 all declare `rule`: only the VALUE can pick row 2.
TEST(ShippedRowPointer, AKeySelectorNamesTheOneRowWhoseMemberIsTheValue) {
    auto const r = resolveRowPointer(doc(), "/rows/{rule=c}");
    EXPECT_EQ(r.pointer, "/rows/2") << "the row whose rule IS c, not the first that has a rule";
    ASSERT_EQ(r.selected.size(), 1u);
    EXPECT_EQ(r.selected.front(), 2u);
    EXPECT_EQ(resolveRowPointer(doc(), "/rows/{rule=c}/x/deep/0").pointer, "/rows/2/x/deep/0");
}

// Asked of the MANY-pointer form: the one-pointer form's own count check would also
// throw on zero or two pointers and hide a selector that stopped refusing.
TEST(ShippedRowPointer, AKeySelectorThatMatchesNoRowThrows) {
    EXPECT_THROW((void)resolveRowPointers(doc(), "/rows/{rule=gone}"), std::runtime_error)
        << "a renamed or removed row is a stale aim, never an empty answer";
}

TEST(ShippedRowPointer, AKeySelectorThatMatchesTwoRowsThrows) {
    EXPECT_THROW((void)resolveRowPointers(doc(), "/rows/{rule=twin}"), std::runtime_error)
        << "a key selector names ONE row; two would be probed as one";
}

TEST(ShippedRowPointer, AMemberSelectorTakesTheFirstRowThatDeclaresTheMember) {
    auto const r = resolveRowPointer(doc(), "/rows/{x}/x");
    EXPECT_EQ(r.pointer, "/rows/1/x");
    EXPECT_THROW((void)resolveRowPointer(doc(), "/rows/{nobody}"), std::runtime_error);
}

TEST(ShippedRowPointer, AnEverySelectorNamesEachRowThatDeclaresTheMember) {
    auto const all = resolveRowPointers(doc(), "/rows/{x*}/x");
    ASSERT_EQ(all.size(), 2u) << "every row that declares x, not the first";
    EXPECT_EQ(all[0].pointer, "/rows/1/x");
    EXPECT_EQ(all[1].pointer, "/rows/2/x");
    EXPECT_THROW((void)resolveRowPointer(doc(), "/rows/{x*}/x"), std::runtime_error)
        << "the one-pointer form refuses a fan-out";
}

TEST(ShippedRowPointer, APlainPointerPassesThroughAndMustResolveWhole) {
    EXPECT_EQ(resolveRowPointer(doc(), "/map/k").pointer, "/map/k");
    EXPECT_TRUE(resolveRowPointer(doc(), "/map/k").selected.empty());
    EXPECT_EQ(resolveRowPointer(doc(), "").pointer, "")
        << "the empty pointer is the whole document (RFC 6901), a probe the vocabulary "
           "tests make";
    EXPECT_THROW((void)resolveRowPointer(doc(), "/rows/{rule=b}/missing"), std::runtime_error)
        << "a stale TAIL after a good selector is a stale aim too";
    EXPECT_THROW((void)resolveRowPointer(doc(), "/map/{k}"), std::runtime_error)
        << "a selector picks an element of an ARRAY";
    EXPECT_THROW((void)resolveRowPointer(doc(), "rows"), std::runtime_error);
}

TEST(ShippedRowPointer, TheLabelFollowsTheRowTheLastSelectorChose) {
    auto const r = resolveRowPointer(doc(), "/rows/{rule=b}");
    EXPECT_EQ(withSelectedIndex("'rows[{index}]' and again {index}", r),
              "'rows[1]' and again 1");
    EXPECT_EQ(withSelectedIndex("no slot", r), "no slot");
    EXPECT_THROW((void)withSelectedIndex("'rows[{index}]'", resolveRowPointer(doc(), "/map")),
                 std::runtime_error);
}

}  // namespace
