// SchemaCache: shipped-mode resolution by name and by extension,
// caching identity (same pointer on hit), and lookup of an unknown
// language → NotFound / NoExtensionMatch.

#include "core/types/grammar_schema.hpp"
#include "lsp/schema_cache.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

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
    // Use a portable non-existent path — the ctor only stores the
    // optional; it does not stat the directory.
    SchemaCache dirMode{std::filesystem::path{"does-not-exist"}};
    EXPECT_TRUE(dirMode.hasSchemaDir());
}

TEST(SchemaCache, ConcurrentResolveYieldsSingleSharedPointer) {
    // The cache's whole reason to hold a mutex is to ensure that N
    // threads asking for the same language all converge on ONE
    // shared_ptr — no torn loads, no duplicate parses.
    SchemaCache c;
    constexpr int kThreads = 16;
    std::vector<std::shared_ptr<dss::GrammarSchema const>> results(kThreads);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            auto r = c.resolveByName("toy");
            ASSERT_TRUE(r.has_value());
            results[i] = *r;
        });
    }
    while (ready.load(std::memory_order_acquire) < kThreads) { /* spin */ }
    go.store(true, std::memory_order_release);
    for (auto& t : ts) t.join();

    // All threads must observe the same shared_ptr value (identity).
    for (int i = 1; i < kThreads; ++i) {
        EXPECT_EQ(results[0].get(), results[i].get())
            << "thread " << i << " saw a different schema instance";
    }
}
