#include "core/types/token.hpp"

#include <gtest/gtest.h>

#include <type_traits>

using namespace dss;

TEST(Token, DefaultIsUnknown) {
    Token t;
    EXPECT_EQ(t.coreKind, CoreTokenKind::Unknown);
    EXPECT_FALSE(t.schemaKind.valid());
    EXPECT_TRUE(t.span.isEmpty());
}

TEST(Token, ConstructionFromCoreKind) {
    Token t{
        .coreKind   = CoreTokenKind::Identifier,
        .schemaKind = InvalidSchemaToken,
        .span       = SourceSpan::of(0, 5),
    };
    EXPECT_EQ(t.coreKind, CoreTokenKind::Identifier);
    EXPECT_EQ(t.span.length(), 5u);
}

TEST(Token, SchemaKindCanBeSet) {
    Token t;
    t.schemaKind = SchemaTokenId{42};
    EXPECT_TRUE(t.schemaKind.valid());
    EXPECT_EQ(t.schemaKind.v, 42u);
}

TEST(Token, IsTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<Token>);
}
