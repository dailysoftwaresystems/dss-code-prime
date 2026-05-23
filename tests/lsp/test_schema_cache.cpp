// SchemaCache: shipped-mode resolution by name and by extension,
// caching identity (same pointer on hit), and lookup of an unknown
// language → NotFound / NoExtensionMatch.

#include "core/types/grammar_schema.hpp"
#include "lsp/schema_cache.hpp"

#include <gtest/gtest.h>

#include <memory>

using dss::lsp::SchemaCache;
using dss::lsp::SchemaResolveErrorKind;

TEST(SchemaCache, ResolvesShippedToyByName) {
    SchemaCache c;
    auto r = c.resolveByName("toy");
    ASSERT_TRUE(r.has_value()) << "loadShipped(\"toy\") must succeed; the JSON"
                                  " lives in src/source-config/languages/";
    EXPECT_NE(*r, nullptr);
    // Display name in the JSON is "Toy" (capitalized); the loader
    // preserves the file's `name` field. `loadShipped("toy")`
    // accepts the lowercase filename — the in-JSON name is the
    // human-readable label.
    EXPECT_EQ((*r)->name(), "Toy");
}

TEST(SchemaCache, CacheReturnsSamePointerOnSecondResolve) {
    SchemaCache c;
    auto a = c.resolveByName("toy");
    auto b = c.resolveByName("toy");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->get(), b->get())
        << "second resolve must return the cached shared_ptr, not reload";
}

TEST(SchemaCache, UnknownLanguageReturnsNotFound) {
    SchemaCache c;
    auto r = c.resolveByName("zzz-no-such-language");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, SchemaResolveErrorKind::NotFound);
}

TEST(SchemaCache, UnknownExtensionReturnsNoMatch) {
    SchemaCache c;
    auto r = c.resolveByExtension(".no-such-ext-anywhere");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, SchemaResolveErrorKind::NoExtensionMatch);
}

TEST(SchemaCache, HasSchemaDirReflectsConstructionMode) {
    SchemaCache shipped;
    EXPECT_FALSE(shipped.hasSchemaDir());
    SchemaCache dirMode{std::filesystem::path{"/tmp"}};
    EXPECT_TRUE(dirMode.hasSchemaDir());
}
