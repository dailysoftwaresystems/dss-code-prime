#include "lsp/schema_cache.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace dss::lsp {

namespace {

// Shipped grammar names probed during lazy lookup-by-extension.
constexpr std::string_view kShippedLanguages[] = {
    "toy",
    "c-subset",
    "tsql-subset",
};

// Case-insensitive (lowercase) view of an extension for portable
// matching. Windows filesystems are case-insensitive; POSIX is
// case-sensitive — but file-extension conventions are universally
// lowercase, so lowering both sides is safe everywhere.
[[nodiscard]] std::string lowercase(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (auto c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

SchemaCache::SchemaCache(std::optional<std::filesystem::path> schemaDir)
    : schemaDir_(std::move(schemaDir)) {
    // In shipped mode, prime the candidates list — these are the
    // names we'll try when resolving by extension.
    if (!schemaDir_.has_value()) {
        for (auto name : kShippedLanguages) {
            shippedCandidates_.emplace_back(name);
        }
    }
}

std::shared_ptr<dss::GrammarSchema const>
SchemaCache::lookupCachedLocked(std::string_view name) const {
    if (auto it = byName_.find(std::string{name}); it != byName_.end()) {
        return it->second;
    }
    return nullptr;
}

std::expected<std::shared_ptr<dss::GrammarSchema const>, SchemaResolveError>
SchemaCache::loadFresh(std::string_view name) {
    if (schemaDir_.has_value()) {
        const auto path = *schemaDir_ / (std::string{name} + ".lang.json");
        auto loaded = dss::GrammarSchema::loadFromFile(path);
        if (!loaded.has_value()) {
            std::string detail = "loadFromFile failed for ";
            detail += path.string();
            if (!loaded.error().empty()) {
                detail += ": ";
                detail += loaded.error()[0].message;
            }
            return std::unexpected(SchemaResolveError{
                SchemaResolveErrorKind::LoadFailed, std::move(detail)});
        }
        return *loaded;
    }
    auto loaded = dss::GrammarSchema::loadShipped(name);
    if (!loaded.has_value()) {
        std::string detail{"loadShipped(\""};
        detail += name;
        detail += "\") failed";
        if (!loaded.error().empty()) {
            detail += ": ";
            detail += loaded.error()[0].message;
        }
        return std::unexpected(SchemaResolveError{
            SchemaResolveErrorKind::NotFound, std::move(detail)});
    }
    return *loaded;
}

std::expected<std::shared_ptr<dss::GrammarSchema const>, SchemaResolveError>
SchemaCache::resolveByName(std::string_view languageName) {
    {
        std::lock_guard lk{mutex_};
        if (auto cached = lookupCachedLocked(languageName)) return cached;
    }
    auto loaded = loadFresh(languageName);
    if (!loaded.has_value()) return loaded;

    std::lock_guard lk{mutex_};
    // Double-check: a concurrent caller may have populated the
    // cache between our drop+reacquire. If so, return their pointer
    // and discard ours (both are valid schemas; sharing is correct).
    if (auto cached = lookupCachedLocked(languageName)) return cached;
    auto [it, inserted] = byName_.emplace(std::string{languageName}, *loaded);
    // Index every declared extension for this schema so subsequent
    // `resolveByExtension` calls are O(1).
    for (auto const& ext : it->second->fileExtensions()) {
        extToName_.emplace(lowercase(ext), it->first);
    }
    return it->second;
}

std::expected<std::shared_ptr<dss::GrammarSchema const>, SchemaResolveError>
SchemaCache::resolveByExtension(std::string_view fileExtension) {
    const auto lower = lowercase(fileExtension);
    {
        std::lock_guard lk{mutex_};
        if (auto it = extToName_.find(lower); it != extToName_.end()) {
            if (auto cached = lookupCachedLocked(it->second)) return cached;
        }
    }
    // Miss. Try each shipped candidate. If `--schema-dir` is set
    // we have no candidate list, so the caller must use
    // `resolveByName` for unknown extensions.
    for (auto const& name : shippedCandidates_) {
        auto loaded = resolveByName(name);
        if (!loaded.has_value()) continue;
        for (auto const& ext : (*loaded)->fileExtensions()) {
            if (lowercase(ext) == lower) return loaded;
        }
    }
    return std::unexpected(SchemaResolveError{
        SchemaResolveErrorKind::NoExtensionMatch,
        std::string{"no shipped schema declares file extension "} + lower});
}

} // namespace dss::lsp
