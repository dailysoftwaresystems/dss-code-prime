#include "lsp/schema_cache.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace dss::lsp {

namespace {

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

// Build a `SchemaResolveError` with `<verb> failed[: <first loader
// diagnostic>]`. Used by both schema-dir and shipped load paths.
[[nodiscard]] SchemaResolveError makeLoadError(
    SchemaResolveErrorKind kind,
    std::string            prefix,
    std::vector<dss::ConfigDiagnostic> const& diags) {
    if (!diags.empty()) {
        prefix += ": ";
        prefix += diags[0].message;
    }
    return SchemaResolveError{kind, std::move(prefix)};
}

} // namespace

ShippedDiscoveryResult SchemaCache::discoverShippedLanguages(
    std::optional<std::filesystem::path> startPath) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path here = startPath.value_or(fs::current_path(ec));
    for (int i = 0; i < 8 && !here.empty(); ++i) {
        const fs::path candidate = here / "src" / "dss-config" / "sources";
        if (fs::is_directory(candidate, ec)) {
            std::vector<std::string> names;
            for (auto const& entry : fs::directory_iterator(candidate, ec)) {
                if (!entry.is_regular_file()) continue;
                const auto name = entry.path().filename().string();
                constexpr std::string_view kSuffix = ".lang.json";
                if (name.size() <= kSuffix.size()) continue;
                if (name.compare(name.size() - kSuffix.size(),
                                 kSuffix.size(), kSuffix) != 0) continue;
                names.push_back(name.substr(0, name.size() - kSuffix.size()));
            }
            std::sort(names.begin(), names.end());
            return {std::move(names), candidate};
        }
        const fs::path parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }
    // Walk exhausted without finding the directory. Distinguishing this
    // "not located" outcome from "located but empty" is load-bearing —
    // `resolveByExtension` surfaces the two as different errors so the
    // operator knows whether to fix --schema-dir or to populate the dir.
    return {{}, std::nullopt};
}

SchemaCache::SchemaCache(std::optional<std::filesystem::path> schemaDir,
                         std::optional<std::filesystem::path> discoveryStartPath)
    : schemaDir_(std::move(schemaDir)) {
    // In shipped mode, prime the candidates list by scanning the
    // shipped-config directory — every `*.lang.json` is a candidate
    // when resolving by extension. No hardcoded language names
    // anywhere (08.55 cleanup).
    if (!schemaDir_.has_value()) {
        auto result = discoverShippedLanguages(std::move(discoveryStartPath));
        shippedCandidates_ = std::move(result.names);
        shippedDir_        = std::move(result.directory);
    }
}

std::shared_ptr<dss::GrammarSchema const>
SchemaCache::lookupCachedLocked(std::string_view name) const {
    if (auto it = byName_.find(std::string{name}); it != byName_.end()) {
        return it->second;
    }
    return nullptr;
}

SchemaResult SchemaCache::loadFresh(std::string_view name) {
    if (schemaDir_.has_value()) {
        const auto path = *schemaDir_ / (std::string{name} + ".lang.json");
        auto loaded = dss::GrammarSchema::loadFromFile(path);
        if (!loaded.has_value()) {
            return std::unexpected(makeLoadError(
                SchemaResolveErrorKind::LoadFailed,
                "loadFromFile failed for " + path.string(),
                loaded.error()));
        }
        return *loaded;
    }
    auto loaded = dss::GrammarSchema::loadShipped(name);
    if (!loaded.has_value()) {
        return std::unexpected(makeLoadError(
            SchemaResolveErrorKind::NotFound,
            "loadShipped(\"" + std::string{name} + "\") failed",
            loaded.error()));
    }
    return *loaded;
}

SchemaResult SchemaCache::resolveByName(std::string_view languageName) {
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

SchemaResult SchemaCache::resolveByExtension(std::string_view fileExtension) {
    const auto lower = lowercase(fileExtension);
    {
        std::lock_guard lk{mutex_};
        if (auto it = extToName_.find(lower); it != extToName_.end()) {
            if (auto cached = lookupCachedLocked(it->second)) return cached;
        }
    }
    // Cache miss. In --schema-dir mode there is no shipped-candidate
    // list (we don't enumerate the user's dir up front) — fall through
    // to the NoExtensionMatch path below.
    //
    // In shipped mode an EMPTY candidate list is a config-error class,
    // not a "valid: no language declares this extension" answer. Surface
    // the *reason* the list is empty so the operator can act:
    //   - directory not found  ⇒ ShippedDirNotFound (set --schema-dir or
    //     run from the repo)
    //   - directory empty      ⇒ ShippedDirEmpty (populate it)
    // Without these distinct codes, a deploy mis-configuration silently
    // looks like "your file isn't supported", which is the wrong fix.
    if (!schemaDir_.has_value() && shippedCandidates_.empty()) {
        if (!shippedDir_.has_value()) {
            return std::unexpected(SchemaResolveError{
                SchemaResolveErrorKind::ShippedDirNotFound,
                "shipped-language directory `src/dss-config/sources` "
                "was not located within 8 parent levels of the working "
                "directory; pass --schema-dir explicitly or invoke from "
                "the repository"});
        }
        return std::unexpected(SchemaResolveError{
            SchemaResolveErrorKind::ShippedDirEmpty,
            std::string{"shipped-language directory `"} +
                shippedDir_->string() + "` contains no `*.lang.json` files"});
    }
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
