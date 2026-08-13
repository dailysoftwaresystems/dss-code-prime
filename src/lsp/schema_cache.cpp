#include "lsp/schema_cache.hpp"

#include "core/types/config_path_walk.hpp"  // findShippedConfigDir — the ONE src/dss-config/<dir> resolver

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

// Scan one `src/dss-config/sources` directory for `*.lang.json` stems.
// Factored out so the DSS_CONFIG_ROOT branch and the cwd-walk branch below
// cannot drift in what they consider a candidate language.
[[nodiscard]] static ShippedDiscoveryResult scanSourcesDir(
    std::filesystem::path const& candidate) {
    namespace fs = std::filesystem;
    std::error_code ec;
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

ShippedDiscoveryResult SchemaCache::discoverShippedLanguages(
    std::optional<std::filesystem::path> startPath) {
    // ── DSS_CONFIG_ROOT first (D-LSP-SHIPPED-DISCOVERY-IGNORES-DSS-CONFIG-ROOT)
    // This walk was one of THREE hand-rolled copies of "resolve a directory
    // under src/dss-config/", and it ignored the override entirely. The result
    // was a tree that lied to itself out-of-tree: `resolveByName` worked,
    // because it routes through `loadShipped` -> `findShippedConfig`, which
    // reads the env — while `resolveByExtension` failed, because its candidate
    // list comes from THIS lookup. MEASURED: with a build directory outside the
    // source tree and DSS_CONFIG_ROOT set, the three resolveByName tests in
    // `lsp/test_schema_cache` passed and the three resolveByExtension tests
    // failed.
    // The precedence now lives ONCE, in `findShippedConfigDir` (env override,
    // then an 8-hop cwd walk, then not-found) — the same substrate the file
    // form uses, so the copies can no longer drift apart.
    // An EXPLICIT `startPath` still wins outright — it is the caller saying
    // "discover from exactly here", which the tests rely on to point discovery
    // at a scratch dir; honouring the ambient environment over it would make
    // those fixtures untestable. That property is a documented contract of the
    // shared primitive, not something this call site re-implements.
    if (auto const dir = findShippedConfigDir("sources", startPath)) {
        return scanSourcesDir(*dir);
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
    //
    // ★★ `emplace` USED TO SILENTLY DROP a second claimant of an already-
    // indexed extension, which turned the cache into a memo of whichever
    // language happened to resolve FIRST — and `.s` now has two claimants
    // (`asm-x86_64-att`, `asm-arm64-gas`), so that drop is live rather than
    // theoretical (D-DRIVER-ASM-DIALECT-SELECTED-BY-TARGET). The index now
    // records EVERY claimant per extension; `resolveByExtension` refuses to
    // answer when there is more than one. Deduplicated by NAME so re-resolving
    // one language does not make it ambiguous with itself.
    for (auto const& ext : it->second->fileExtensions()) {
        auto& claimants = extClaimants_[lowercase(ext)];
        if (std::find(claimants.begin(), claimants.end(), it->first)
            == claimants.end()) {
            claimants.push_back(it->first);
        }
    }
    return it->second;
}

// Build the `AmbiguousExtension` error for an extension more than one shipped
// language claims. Factored out so the cache-hit path and the cold-scan path
// cannot drift in what they SAY about the same situation — the two used to be
// the fast and slow halves of one answer, and an ambiguity that reported
// differently depending on cache warmth would be untestable.
[[nodiscard]] static SchemaResolveError ambiguousExtensionError(
    std::string const& ext, std::vector<std::string> const& claimants) {
    std::string names;
    for (auto const& n : claimants) {
        if (!names.empty()) names += ", ";
        names += n;
    }
    return SchemaResolveError{
        SchemaResolveErrorKind::AmbiguousExtension,
        "file extension " + ext + " is claimed by "
            + std::to_string(claimants.size()) + " shipped languages ("
            + names
            + ") — the extension alone cannot name one of them. An assembly "
              "file is target-specific by nature, so the dialect is a question "
              "the compile TARGET answers; this resolver has no target, so it "
              "refuses rather than guessing."};
}

SchemaResult SchemaCache::resolveByExtension(std::string_view fileExtension) {
    const auto lower = lowercase(fileExtension);
    {
        std::lock_guard lk{mutex_};
        if (auto it = extClaimants_.find(lower); it != extClaimants_.end()) {
            // ⚠ THE CACHE-HIT PATH MUST APPLY THE SAME RULE AS THE COLD SCAN.
            // A warm cache that answered a `.s` while a cold one refused would
            // make the defect depend on which file the editor opened first.
            if (it->second.size() > 1u) {
                return std::unexpected(
                    ambiguousExtensionError(lower, it->second));
            }
            // ⚠⚠ AND "ONE CLAIMANT SO FAR" IS NOT "ONE CLAIMANT". The index
            // only holds languages somebody already resolved BY NAME, so a
            // single `resolveByName("asm-x86_64-att")` leaves `.s` looking
            // unique while `asm-arm64-gas` claims it too — answering from that
            // is the very first-wins bug, re-entered through the fast path.
            // The short-circuit is therefore gated on the index being COMPLETE
            // over the candidate universe.
            //   • shipped mode: complete once the full candidate scan below
            //     has run at least once.
            //   • --schema-dir mode: there IS no enumerable universe (the
            //     user's directory is never scanned up front), so the warm
            //     index is all the knowledge that exists and answering from it
            //     is not a shortcut past a check — it is the only path. A
            //     collision inside that directory is still caught above,
            //     because both languages land in one claimant list.
            if (schemaDir_.has_value() || shippedExtIndexComplete_) {
                if (auto cached = lookupCachedLocked(it->second.front())) {
                    return cached;
                }
            }
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
    // ★★ SCAN EVERY CANDIDATE — do NOT return on the first match.
    //
    // This loop used to `return loaded;` the moment a language claimed the
    // extension, with the candidate list sorted alphabetically by stem
    // (`scanSourcesDir`). That made `asm-arm64-gas` the permanent answer for
    // every `.s` in the editor, silently, purely because 'a-r' sorts before
    // 'x-8' — an x86 assembly file would have been highlighted, folded and
    // diagnosed as arm64 assembly, and nothing would have said so. The scan
    // now runs to completion and the CLAIMANT COUNT decides.
    //
    // Cost: unchanged in the common case and bounded in every case — the
    // per-name resolve is memoized by `resolveByName`, so this is one pass
    // over the shipped candidate list per cold extension, and the completion
    // flag below makes subsequent lookups O(1).
    std::vector<std::string> claimants;
    for (auto const& name : shippedCandidates_) {
        auto loaded = resolveByName(name);
        // A candidate that fails to LOAD is skipped rather than fatal: one
        // broken document in the shipped directory must not make every other
        // language unresolvable. It also cannot silently become the answer —
        // it contributes no claim.
        if (!loaded.has_value()) continue;
        for (auto const& ext : (*loaded)->fileExtensions()) {
            if (lowercase(ext) == lower) { claimants.push_back(name); break; }
        }
    }
    {
        std::lock_guard lk{mutex_};
        // Every shipped candidate has now been resolved-or-skipped, so the
        // extension index covers the whole universe and the fast path above
        // may trust a unique claimant from here on.
        shippedExtIndexComplete_ = true;
    }
    if (claimants.size() > 1u) {
        return std::unexpected(ambiguousExtensionError(lower, claimants));
    }
    if (claimants.size() == 1u) return resolveByName(claimants.front());
    return std::unexpected(SchemaResolveError{
        SchemaResolveErrorKind::NoExtensionMatch,
        std::string{"no shipped schema declares file extension "} + lower});
}

} // namespace dss::lsp
