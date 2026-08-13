#pragma once

#include "analysis/semantic/semantic_model.hpp"
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
    // WHY `schema == nullptr`, in the document's own words. Empty iff a schema
    // WAS resolved.
    //
    // ★ A NULL SCHEMA USED TO BE MUTE (D-LSP-ASSEMBLY-DIALECT-UNSERVABLE).
    // `handleDidOpen_` resolved by extension and, on failure, simply opened the
    // document with `schema == nullptr`; the parse worker then PUBLISHED AN
    // EMPTY DIAGNOSTIC ARRAY, which an editor renders identically to "this file
    // is clean". So the two states an editor most needs to tell apart — "no
    // problems" and "no language service at all" — looked the same on the wire.
    // The reason now travels WITH the document, so every publish (open, and
    // every later edit) can restate it: a message that appears once and
    // vanishes on the next keystroke is barely better than no message.
    std::string                                schemaError;
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
    //
    // `schemaError` is the reason `schema` is null — REQUIRED to be non-empty
    // when `schema` is null and empty when it is not; see `DocumentSnapshot::
    // schemaError`. Defaulted so the many "a schema resolved" call sites stay
    // one argument shorter, never so a failure can be opened silently.
    void open(std::string uri,
              std::int32_t clientVersion,
              std::string text,
              std::shared_ptr<dss::GrammarSchema const> schema,
              std::string schemaError = {});

    // Update an open document's text + clientVersion. Bumps
    // parseGeneration; returns the new generation (callers use it
    // as the cancellation token for the parse job they enqueue).
    // Returns std::nullopt if the URI is not open.
    [[nodiscard]] std::optional<std::uint32_t>
        update(std::string const& uri, std::int32_t clientVersion, std::string text);

    // Re-point an ALREADY-OPEN document at a different schema (or at none, with
    // the reason), leaving its text and clientVersion untouched. Returns true
    // iff the document exists AND the `(schema, schemaError)` pair actually
    // CHANGED — so the caller republishes exactly the affected documents and
    // nothing else.
    //
    // ★ WHY IT EXISTS (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE). The
    // schema a document is parsed under is not only a property of its
    // extension: when the extension is claimed by two languages the WORKSPACE's
    // project manifests break the tie, and a manifest can be added or edited
    // mid-session. `open()` cannot serve that — it resets text, version and
    // diagnostics, i.e. it would forget everything the client has typed.
    //
    // ★ IT BUMPS `parseGeneration`, reusing the EXISTING stale-suppression
    // invariant instead of inventing a second one: a parse already in flight was
    // started under the OLD grammar, and its diagnostics must lose to the reparse
    // the caller is about to enqueue.
    [[nodiscard]] bool setSchema(std::string const& uri,
                                 std::shared_ptr<dss::GrammarSchema const> schema,
                                 std::string schemaError);

    // Every currently-open URI, SORTED. Sorted rather than in hash order for
    // the same reason `collectManifests` sorts: a republish sweep must visit
    // documents in an order two machines agree on, or the wire trace this
    // server is tested through becomes environment-dependent.
    [[nodiscard]] std::vector<std::string> openUris() const;

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

    // Store the SemanticModel produced by a parse that started at
    // `expectedGen`. Dropped (returns false) on a generation mismatch —
    // mirrors setDiagnostics' stale-suppression. SemanticModel is
    // move-only, so it is handed in as a shared_ptr<const> the store keeps
    // and hands back lock-free to query handlers.
    [[nodiscard]] bool setSemanticModel(
        std::string const& uri,
        std::uint32_t expectedGen,
        std::shared_ptr<dss::SemanticModel const> model);

    // Snapshot the current SemanticModel for a URI (under the mutex). The
    // returned shared_ptr lets the handler read the model lock-free for as
    // long as it holds the pointer, even if a newer parse swaps in a
    // replacement. Null if none stored.
    [[nodiscard]] std::shared_ptr<dss::SemanticModel const>
        semanticModelFor(std::string const& uri) const;

private:
    struct Entry;

    // ★ THE SILENT STATE IS MADE UNREPRESENTABLE AT EVERY DOOR, NOT AT ONE.
    // `schema == nullptr` with an empty `schemaError` is the shape
    // D-LSP-ASSEMBLY-DIALECT-UNSERVABLE removed (the publish path then emits an
    // empty diagnostics array, which an editor renders as "this file is
    // clean"). `open` enforced it; `setSchema` is a SECOND door into the same
    // state, so both call this rather than each carrying its own copy of the
    // substitute message — two copies is how the doors drift apart.
    // Caller must hold `mutex_`.
    static void enforceSchemaReasonLocked_(Entry& entry);

    struct Entry {
        std::int32_t                                clientVersion = 0;
        std::uint32_t                               parseGeneration = 0;
        std::string                                 text;
        std::shared_ptr<dss::GrammarSchema const>   schema;
        std::string                                 schemaError;
        std::vector<dss::ParseDiagnostic>           diagnostics;
        std::shared_ptr<dss::SemanticModel const>   semanticModel;
        std::uint32_t                               semanticGeneration = 0;
    };

    mutable std::mutex                                  mutex_;
    std::unordered_map<std::string, Entry>              docs_;
};

} // namespace dss::lsp
