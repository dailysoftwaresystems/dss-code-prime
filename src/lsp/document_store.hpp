#pragma once

#include "core/export.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Per-URI document state. The store is keyed by URI; each entry
// holds the current source text, the resolved schema, a monotonic
// `parseGeneration` (the cancellation token for in-flight parses),
// and the last published diagnostics.
//
// Thread-safety: all public methods take an internal mutex. Worker
// threads read text+schema via `snapshot(uri)` (returns a value
// copy) and write back diagnostics via `setDiagnostics(uri, gen,
// diags)` which silently drops the result if a newer `update`
// has bumped the generation in the meantime. This is the
// stale-parse suppression invariant.

namespace dss::lsp {

struct DSS_EXPORT DocumentSnapshot {
    std::string                                uri;
    std::int32_t                               clientVersion = 0;
    std::uint32_t                              parseGeneration = 0;
    std::string                                text;
    std::shared_ptr<dss::GrammarSchema const>  schema;     // null if no schema for this URI
};

class DSS_EXPORT DocumentStore {
public:
    DocumentStore() = default;

    DocumentStore(DocumentStore const&)            = delete;
    DocumentStore& operator=(DocumentStore const&) = delete;
    DocumentStore(DocumentStore&&)                 = delete;
    DocumentStore& operator=(DocumentStore&&)      = delete;

    // Open a document. Sets clientVersion + text + schema; resets
    // parseGeneration to 0. Replaces any prior state for the URI.
    void open(std::string uri,
              std::int32_t clientVersion,
              std::string text,
              std::shared_ptr<dss::GrammarSchema const> schema);

    // Update an open document's text + clientVersion. Bumps
    // parseGeneration; returns the new generation (callers use it
    // as the cancellation token for the parse job they enqueue).
    // Returns std::nullopt if the URI is not open.
    [[nodiscard]] std::optional<std::uint32_t>
        update(std::string const& uri, std::int32_t clientVersion, std::string text);

    // Close (remove) a document. No-op if not open.
    void close(std::string const& uri);

    // Atomically snapshot a document's state for a worker thread.
    // Returns std::nullopt if the URI is not open.
    [[nodiscard]] std::optional<DocumentSnapshot>
        snapshot(std::string const& uri) const;

    // Write back diagnostics for a parse that started at `expectedGen`.
    // If the document's current generation differs (newer update
    // arrived), the call is silently dropped — the stale parse's
    // diagnostics never reach the client. Returns true if applied,
    // false if dropped.
    [[nodiscard]] bool setDiagnostics(std::string const& uri,
                                       std::uint32_t expectedGen,
                                       std::vector<dss::ParseDiagnostic> diags);

    // Read back the last-published diagnostics for a URI. Returns
    // empty vector if URI is unknown OR no diagnostics published.
    [[nodiscard]] std::vector<dss::ParseDiagnostic>
        diagnosticsFor(std::string const& uri) const;

private:
    struct Entry {
        std::int32_t                                clientVersion = 0;
        std::uint32_t                               parseGeneration = 0;
        std::string                                 text;
        std::shared_ptr<dss::GrammarSchema const>   schema;
        std::vector<dss::ParseDiagnostic>           diagnostics;
    };

    mutable std::mutex                                  mutex_;
    std::unordered_map<std::string, Entry>              docs_;
};

} // namespace dss::lsp
