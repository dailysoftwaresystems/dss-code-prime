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

// Six shipped languages plus the targets and formats one invocation touches,
// with headroom. See the capacity note in the header: evicting can only cost a
// rebuild, never a wrong answer.
constexpr std::size_t kCapacity = 16;

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
    for (auto const& e : memoEntries()) {
        if (e.family != family || e.label != label || e.digest != digest) continue;
        if (!dependenciesStillMatch(e.dependencies)) {
            memoStats().dependencyRejections += 1;
            memoStats().misses += 1;
            return nullptr;
        }
        memoStats().hits += 1;
        return e.schema;
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

} // namespace dss::detail
