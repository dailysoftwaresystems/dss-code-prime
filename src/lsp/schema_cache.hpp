#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"

#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Schema cache with two-mode resolution:
//   - Shipped name: `GrammarSchema::loadShipped(name)` (cwd-walk).
//   - Explicit dir: load `<schemaDir>/<name>.lang.json`.
//
// One `shared_ptr<GrammarSchema const>` per language name is kept
// for the cache's lifetime. Multiple worker threads may resolve
// concurrently; the cache serializes loads via an internal mutex
// and uses a double-check pattern so the load itself runs without
// holding the lock (file I/O is slow; would otherwise serialize
// every cold parse).
//
// File-extension → language-name mapping is also cached so the
// server can route `didOpen` URIs to schemas via their extension
// without re-walking the shipped configs every request.

namespace dss::lsp {

enum class SchemaResolveErrorKind : std::uint8_t {
    NotFound,           // schema dir / shipped name doesn't exist
    LoadFailed,         // file exists but loader rejected it
    NoExtensionMatch,   // file extension matches no known language
};

struct DSS_EXPORT SchemaResolveError {
    SchemaResolveErrorKind kind;
    std::string            detail;
};

// Alias: every public resolver returns this shape.
using SchemaResult = std::expected<std::shared_ptr<dss::GrammarSchema const>,
                                    SchemaResolveError>;

class DSS_EXPORT SchemaCache {
public:
    // `schemaDir` empty ⇒ shipped-only mode.
    explicit SchemaCache(std::optional<std::filesystem::path> schemaDir = std::nullopt);

    SchemaCache(SchemaCache const&)            = delete;
    SchemaCache& operator=(SchemaCache const&) = delete;
    SchemaCache(SchemaCache&&)                 = delete;
    SchemaCache& operator=(SchemaCache&&)      = delete;

    [[nodiscard]] SchemaResult resolveByName(std::string_view languageName);

    // Iterates known languages' `fileExtensions()` and returns the
    // first case-insensitive match. In shipped mode, probes each
    // shipped candidate on cache miss.
    [[nodiscard]] SchemaResult resolveByExtension(std::string_view fileExtension);

    [[nodiscard]] bool hasSchemaDir() const noexcept {
        return schemaDir_.has_value();
    }

private:
    [[nodiscard]] std::shared_ptr<dss::GrammarSchema const>
        lookupCachedLocked(std::string_view name) const;

    // Loads a schema OUTSIDE the cache lock (file I/O too slow to
    // serialize). Returns unexpected with diagnostic detail on miss.
    [[nodiscard]] SchemaResult loadFresh(std::string_view name);

    std::optional<std::filesystem::path>                                schemaDir_;
    mutable std::mutex                                                  mutex_;
    std::unordered_map<std::string,
                       std::shared_ptr<dss::GrammarSchema const>>       byName_;
    // Built lazily during `resolveByExtension`: ext → language name.
    std::unordered_map<std::string, std::string>                        extToName_;
    // List of shipped language names probed once at construction.
    // Empty in --schema-dir mode (we don't enumerate the dir
    // upfront; we load lazily on demand).
    std::vector<std::string>                                            shippedCandidates_;
};

} // namespace dss::lsp
