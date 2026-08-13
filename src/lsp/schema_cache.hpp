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
    // More than one language declares this file extension, so the extension
    // alone cannot name one (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET:
    // `asm-x86_64-att` and `asm-arm64-gas` both declare `.s`/`.S`). DISTINCT
    // from `NoExtensionMatch` because the operator action is opposite — there
    // is no missing language to install, there is a choice to make, and the
    // detail NAMES the claimants. An assembly file is target-specific by
    // nature, so the dialect is a question the compile TARGET answers; this
    // resolver has no target and must not invent one.
    AmbiguousExtension,
    // Shipped-mode only: neither `$DSS_CONFIG_ROOT` nor the 8-level
    // cwd-walk located the `src/dss-config/sources/` directory.
    // Without it, extension-based resolution has no candidate list. Loud
    // failure prevents a silent "extension matches nothing" misdiagnosis
    // when the real problem is "shipped configs not discoverable from
    // this cwd" (e.g. binary invoked from a deploy location).
    ShippedDirNotFound,
    // Shipped-mode only: the shipped-config directory was located but
    // contains no `*.lang.json` files. Distinct from `ShippedDirNotFound`
    // so the operator knows whether to set --schema-dir or to populate
    // the located directory.
    ShippedDirEmpty,
};

struct DSS_EXPORT SchemaResolveError {
    SchemaResolveErrorKind kind;
    std::string            detail;
};

// Alias: every public resolver returns this shape.
using SchemaResult = std::expected<std::shared_ptr<dss::GrammarSchema const>,
                                    SchemaResolveError>;

// Result of the shipped-config-directory discovery walk: every name
// found (sorted, without the `.lang.json` suffix) PLUS the directory
// path that was located (or `nullopt` if the cwd-walk found nothing
// within its 8-level horizon). The two together let `SchemaCache`
// distinguish "directory not findable from this cwd" (config error)
// from "directory found but empty" (deploy error) — both surface as
// loud errors at the resolve site, not a silent empty list.
struct DSS_EXPORT ShippedDiscoveryResult {
    std::vector<std::string>             names;
    std::optional<std::filesystem::path> directory;
};

class DSS_EXPORT SchemaCache {
public:
    // `schemaDir` empty ⇒ shipped-only mode (the cache auto-discovers
    // `src/dss-config/sources/` via `findShippedConfigDir`:
    // `$DSS_CONFIG_ROOT`, else a cwd-walk). `discoveryStartPath`
    // overrides the walk's starting point and exists for tests that need
    // to pin "no shipped dir found" / "dir found but empty" behaviour
    // without mutating process cwd; production callers leave it default.
    explicit SchemaCache(std::optional<std::filesystem::path> schemaDir = std::nullopt,
                         std::optional<std::filesystem::path> discoveryStartPath = std::nullopt);

    // Locate `src/dss-config/sources/` via the shared resolver
    // (`findShippedConfigDir`): `$DSS_CONFIG_ROOT` first, else a walk up
    // to 8 parent levels from cwd. If found, enumerate `*.lang.json` and
    // return the sorted base names alongside the located directory.
    // An explicit `startPath` means "discover from exactly here" — it
    // outranks the env override and roots the walk. Public + static so
    // tests can pin behavior without mutating process cwd.
    [[nodiscard]] static ShippedDiscoveryResult discoverShippedLanguages(
        std::optional<std::filesystem::path> startPath = std::nullopt);

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
    // Built lazily as languages resolve: ext → EVERY language name that claims
    // it, in first-indexed order, deduplicated by name.
    //
    // ★ A LIST, NOT A NAME (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET). The
    // predecessor was `ext → name` populated with `emplace`, which SILENTLY
    // DROPPED the second claimant of an already-indexed extension: the map
    // kept whichever language was resolved first and every later claimant
    // vanished without a diagnostic. `.s` has two claimants today
    // (`asm-x86_64-att`, `asm-arm64-gas`), so the drop decided what an
    // assembly file MEANT in the editor. Keeping the whole list is what lets
    // `resolveByExtension` refuse instead of guess.
    std::unordered_map<std::string, std::vector<std::string>>           extClaimants_;
    // True once `resolveByExtension`'s cold scan has resolved-or-skipped every
    // entry of `shippedCandidates_`, i.e. once `extClaimants_` covers the whole
    // shipped universe. Until then a single-claimant entry means only "one so
    // far", which is NOT enough to answer with — see the fast path in
    // `resolveByExtension`. Meaningless (and unused) in --schema-dir mode,
    // where no universe is ever enumerated.
    bool                                                                shippedExtIndexComplete_ = false;
    // List of shipped language names probed once at construction.
    // Empty in --schema-dir mode (we don't enumerate the dir
    // upfront; we load lazily on demand).
    std::vector<std::string>                                            shippedCandidates_;
    // The shipped-config directory the discovery walk located. Set
    // alongside `shippedCandidates_` at construction in shipped mode;
    // `nullopt` when the cwd-walk failed (→ ShippedDirNotFound on
    // extension lookup) or when in --schema-dir mode (unused).
    std::optional<std::filesystem::path>                                shippedDir_;
};

} // namespace dss::lsp
