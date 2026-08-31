#include "core/types/config_document_memo.hpp"

#include "core/crypto/sha256.hpp"                 // crypto::sha256Hex — the ONE content digest
#include "core/substrate/checked_file_read.hpp"   // the ONE checked whole-file read

#include <deque>
#include <mutex>

namespace dss::detail {

std::optional<std::string>
digestConfigDocumentFile(std::string_view path) {
    // THE ONE CHECKED READ
    // (D-CORE-SHIPPED-CONFIG-LOADERS-DRAIN-A-STREAM-WITHOUT-CHECKING-IT).
    // A dependency that reads SHORT must not digest to a value that merely
    // happens to differ — it is reported as UNREADABLE, so the caller misses
    // and rebuilds through the loader's own checked read, which reports the
    // real I/O failure against the real path. Digesting a truncated document
    // here would turn an I/O fault into a cache miss and hide it.
    auto text = core::readFileChecked(path);
    if (!text) return std::nullopt;
    return crypto::sha256Hex(*text);
}

namespace {

struct MemoEntry {
    std::string                           family;
    std::string                           label;
    std::string                           digest;
    std::vector<ConfigDocumentDependency> dependencies;
    std::shared_ptr<void>                 schema;
};

// ⓘ Function-local statics rather than namespace-scope ones, so the store is
// constructed on first use and cannot be caught in a static-initialisation
// order dependency with whichever loader happens to run first.
[[nodiscard]] std::mutex& memoMutex() {
    static std::mutex m;
    return m;
}

[[nodiscard]] std::deque<MemoEntry>& memoEntries() {
    static std::deque<MemoEntry> entries;
    return entries;
}

[[nodiscard]] ConfigDocumentMemoStore::Stats& memoStats() {
    static ConfigDocumentMemoStore::Stats s;
    return s;
}

// ★ FOUR TIMES THE WHOLE SHIPPED CORPUS, AND THE DERIVATION IS THE POINT —
// see the capacity note in the header for the measurement that replaced the
// refuted "handful of documents" premise this constant used to rest on.
//
// ✔MEASURED 2026-08-31 at `f865897c`: the corpus is 32 documents (24
// `.format.json` + 6 `.lang.json` + 2 `.target.json`), and TWO production paths
// scan the object-format class in FULL because they are proving uniqueness. The
// bound must therefore exceed the whole corpus, not one path's working set —
// any smaller number reintroduces the same cliff one document class at a time.
// 4x leaves room for the corpus to QUADRUPLE before the bound binds again, and
// `tests/program/test_config_memo_holds_a_total_scan` reddens if it ever does.
//
// ⓘ It is not larger, and that bound is what keeps the memory guard a guard: a
// mutation harness walking many distinct MUTANTS of one 506 KB grammar is the
// case this exists to bound, and it is the one case where holding every entry
// would be the defect rather than the fix.
constexpr std::size_t kCapacity = 128;

// ★ Stops at the FIRST dependency that moved. Re-reading the rest would be
// wasted work — the verdict is already settled, and the verdict is all a
// lookup needs.
[[nodiscard]] bool dependenciesStillMatch(
    std::vector<ConfigDocumentDependency> const& dependencies) {
    for (auto const& dep : dependencies) {
        auto const now = digestConfigDocumentFile(dep.path);
        if (!now.has_value() || *now != dep.digest) return false;
    }
    return true;
}

} // namespace

std::shared_ptr<void> ConfigDocumentMemoStore::lookup(std::string_view family,
                                                      std::string_view label,
                                                      std::string_view digest) {
    // ⚠ The dependency re-read happens UNDER the lock. It costs a file read per
    // dependency, which is not free — but releasing the lock to do it would let
    // a concurrent `store` retire the very entry being validated, and the
    // alternative (copying the entry out, validating, then re-finding it) makes
    // the window smaller rather than closing it. The contended case is a
    // handful of documents per process, so the simple correct order wins.
    std::lock_guard const guard{memoMutex()};
    auto& entries = memoEntries();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->family != family || it->label != label || it->digest != digest) {
            continue;
        }
        if (!dependenciesStillMatch(it->dependencies)) {
            memoStats().dependencyRejections += 1;
            memoStats().misses += 1;
            return nullptr;
        }
        memoStats().hits += 1;
        // ★ THE `store` SIDE EVICTS FROM THE FRONT, SO A HIT MOVES ITS ENTRY TO
        // THE BACK — that pair IS the LRU policy, and the reason it is here
        // rather than in `store` is that a hit is the only evidence a document
        // is still in use. Under the previous FIFO the language document, loaded
        // first and used throughout, was the FIRST thing a 24-document
        // object-format scan evicted. The schema is copied out BEFORE the
        // rotation because the rotation invalidates `it`.
        std::shared_ptr<void> schema = it->schema;
        MemoEntry             moved  = std::move(*it);
        entries.erase(it);
        entries.push_back(std::move(moved));
        return schema;
    }
    memoStats().misses += 1;
    return nullptr;
}

void ConfigDocumentMemoStore::store(std::string_view family, std::string label,
                                    std::string digest,
                                    std::vector<ConfigDocumentDependency> dependencies,
                                    std::shared_ptr<void> schema) {
    if (schema == nullptr) return;
    std::lock_guard const guard{memoMutex()};
    // ⓘ A duplicate key is LEFT ALONE rather than replaced. Two threads that
    // raced the same cold build produced schemas this project has a standing
    // arm proving indistinguishable, so either would serve — and keeping the
    // first means a holder of it never sees the entry swap underneath a
    // subsequent lookup.
    for (auto const& e : memoEntries()) {
        if (e.family == family && e.label == label && e.digest == digest) return;
    }
    while (memoEntries().size() >= kCapacity) {
        memoEntries().pop_front();
        memoStats().evictions += 1;
    }
    memoEntries().push_back(MemoEntry{std::string{family}, std::move(label),
                                      std::move(digest), std::move(dependencies),
                                      std::move(schema)});
}

void ConfigDocumentMemoStore::clear() {
    std::lock_guard const guard{memoMutex()};
    memoEntries().clear();
}

ConfigDocumentMemoStore::Stats ConfigDocumentMemoStore::stats() {
    std::lock_guard const guard{memoMutex()};
    return memoStats();
}

void ConfigDocumentMemoStore::resetStats() {
    std::lock_guard const guard{memoMutex()};
    memoStats() = Stats{};
}

std::size_t ConfigDocumentMemoStore::capacity() noexcept { return kCapacity; }

std::size_t ConfigDocumentMemoStore::size() {
    std::lock_guard const guard{memoMutex()};
    return memoEntries().size();
}

} // namespace dss::detail
