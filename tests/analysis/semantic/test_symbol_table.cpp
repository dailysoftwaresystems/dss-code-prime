#include "analysis/semantic/symbol_table.hpp"

#include <gtest/gtest.h>

using namespace dss;

// Fresh table reserves slot 0 for the InvalidSymbol sentinel — size()
// reports the number of REAL symbols.
TEST(SymbolTable, ConstructedEmptyExcludesSentinel) {
    SymbolTable st;
    EXPECT_EQ(st.size(), 0u);
    ASSERT_EQ(st.records().size(), 1u) << "the sentinel slot";
}

// Minting yields dense, monotonically-increasing ids; the first real
// SymbolId is `1` (slot 0 is reserved).
TEST(SymbolTable, MintReturnsDenseIds) {
    SymbolTable st;
    SymbolRecord a; a.name = "alpha";
    SymbolRecord b; b.name = "beta";
    const auto idA = st.mint(std::move(a));
    const auto idB = st.mint(std::move(b));
    EXPECT_EQ(idA.v, 1u);
    EXPECT_EQ(idB.v, 2u);
    EXPECT_EQ(st.size(), 2u);
}

// at() round-trips name; mutation through the returned reference sticks.
TEST(SymbolTable, AtReturnsMutableRecord) {
    SymbolTable st;
    SymbolRecord rec;
    rec.name = "foo";
    const auto id = st.mint(std::move(rec));
    EXPECT_EQ(st.at(id).name, "foo");
    st.at(id).name = "bar";
    EXPECT_EQ(std::as_const(st).at(id).name, "bar");
}

// Out-of-range/invalid id aborts (fail-loud — the SymbolTable is the
// authoritative SymbolId allocator, so an out-of-range id is a bug).
TEST(SymbolTableDeathTest, AtInvalidAborts) {
    SymbolTable st;
    EXPECT_DEATH({ (void)st.at(SymbolId{99}); }, "SymbolTable.*out of range");
    EXPECT_DEATH({ (void)st.at(InvalidSymbol); }, "SymbolTable.*out of range");
}

// release() hands over the vector intact for SemanticModel construction.
TEST(SymbolTable, ReleaseHandsOverVector) {
    SymbolTable st;
    SymbolRecord rec; rec.name = "x";
    st.mint(std::move(rec));
    auto out = std::move(st).release();
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[1].name, "x");
}
