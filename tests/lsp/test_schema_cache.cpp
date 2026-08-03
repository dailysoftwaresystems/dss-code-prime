// SchemaCache: shipped-mode resolution by name and by extension,
// caching identity (same pointer on hit), and lookup of an unknown
// language → NotFound / NoExtensionMatch.

#include "core/types/grammar_schema.hpp"
#include "lsp/schema_cache.hpp"
#include "scratch_dir.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using dss::lsp::SchemaCache;
using dss::lsp::SchemaResolveErrorKind;

TEST(SchemaCache, ResolvesShippedToyByName) {
    SchemaCache c;
    auto r = c.resolveByName("toy");
    ASSERT_TRUE(r.has_value()) << "loadShipped(\"toy\") must succeed; the JSON"
                                  " lives in src/dss-config/sources/";
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

// F14: prove the directory-scan covers ALL shipped languages by
// extension — `.c`/`.h` resolve to c-subset, `.toy` to toy,
// `.sql`/`.tsql` to tsql-subset. Without this pin a future change
// that drops a language from the discovery loop would silently
// regress LSP UX for that file type.
TEST(SchemaCache, ShippedModeFindsAllLanguagesByExtension) {
    SchemaCache c;
    {
        auto r = c.resolveByExtension(".c");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->name(), "CSubset");
    }
    {
        auto r = c.resolveByExtension(".toy");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->name(), "Toy");
    }
    {
        auto r = c.resolveByExtension(".sql");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->name(), "TsqlSubset");
    }
    {
        auto r = c.resolveByExtension(".tsql");
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ((*r)->name(), "TsqlSubset");
    }
}

// F14: an unknown extension returns NoExtensionMatch (NOT a silent
// empty list / NotFound). Confirms the discovery loop is loud about
// "we walked the shipped dir and nothing claimed this extension."
TEST(SchemaCache, UnknownExtensionWithShippedConfigsReportsNoMatch) {
    SchemaCache c;
    auto r = c.resolveByExtension(".totally-bogus-extension");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, SchemaResolveErrorKind::NoExtensionMatch);
}

// F14 (loud-fail follow-up; NO WORKAROUNDS): the discovery walk is a
// pure function — verify it explicitly distinguishes "directory not
// found" from "directory found". `temp_directory_path()` is guaranteed
// to have no `src/dss-config/sources/` ancestor in any realistic
// CI/dev layout, so the walk MUST return an empty `directory`.
TEST(SchemaCache, DiscoverShippedLanguagesReportsAbsentDirectory) {
    const auto tmp = std::filesystem::temp_directory_path();
    const auto result = SchemaCache::discoverShippedLanguages(tmp);
    EXPECT_FALSE(result.directory.has_value())
        << "tmp path " << tmp.string()
        << " unexpectedly has a `src/dss-config/sources/` ancestor";
    EXPECT_TRUE(result.names.empty());
}

// F14 (loud-fail follow-up; NO WORKAROUNDS): with the discovery seeded
// from a non-repo path, the LSP must surface ShippedDirNotFound on the
// FIRST extension lookup — not the legacy silent "no shipped schema
// declares this extension". The deploy operator needs to see "configs
// aren't discoverable" so they can fix --schema-dir, not chase a
// phantom extension-mapping problem.
TEST(SchemaCache, ShippedModeWithNoDirectoryReportsShippedDirNotFound) {
    SchemaCache c{std::nullopt, std::filesystem::temp_directory_path()};
    auto r = c.resolveByExtension(".toy");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, SchemaResolveErrorKind::ShippedDirNotFound);
    EXPECT_NE(r.error().detail.find("dss-config/sources"),
              std::string::npos)
        << "error must name the directory the operator should populate, "
        << "got: " << r.error().detail;
}

// F14 (loud-fail follow-up; NO WORKAROUNDS): a discovered-but-empty
// shipped directory is a deploy-error class distinct from
// not-found-at-all. Build the empty case via a temp dir containing
// `src/dss-config/sources/` with zero `*.lang.json` files.
TEST(SchemaCache, ShippedModeWithEmptyDirectoryReportsShippedDirEmpty) {
    namespace fs = std::filesystem;
    // D-TEST-FIXED-SCRATCH-PATH-POPULATION — the root came from a CONSTANT name
    // under `temp_directory_path()`, and the old "clean any prior run"
    // `remove_all` existed precisely BECAUSE that path could be stale. Under two
    // concurrent instances that line was the weapon: one instance wiped the tree
    // the other had just built, so the walk reported ShippedDirNotFound (kind 3)
    // instead of ShippedDirEmpty (kind 4), or `create_directories` itself blew up
    // with EINVAL mid-rmdir. Both MEASURED. `ScratchDir` claims a per-instance
    // slot atomically (SINGULAR `create_directory`; the pid is only a seed), so
    // the "prior run" wipe is unnecessary by construction and is gone.
    dss::test_support::ScratchDir scratch{
        dss::test_support::Location::Temp, "schema-cache-empty"};
    // Throwing overload on purpose: a scratch tree we could not build is a setup
    // failure the suite must see, not something to paper over.
    fs::create_directories(scratch.path() / "src" / "dss-config" / "sources");
    SchemaCache c{std::nullopt, scratch.path()};
    auto r = c.resolveByExtension(".toy");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, SchemaResolveErrorKind::ShippedDirEmpty);
    EXPECT_NE(r.error().detail.find("contains no"), std::string::npos)
        << "error must explain the directory is empty, got: "
        << r.error().detail;
    // `c` is destroyed before `scratch` (reverse declaration order), so the
    // dtor's `remove_all` runs after the cache has released the directory.
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
