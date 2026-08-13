#pragma once

#include "core/export.hpp"
#include "core/substrate/thread_pool.hpp"
#include "lsp/document_store.hpp"
#include "lsp/method_dispatcher.hpp"
#include "lsp/protocol.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/transport.hpp"
#include "lsp/workspace_project.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <vector>

// LSP server. Composes the transport (stdio or in-memory), the
// method dispatcher, the schema cache, the document store, and the
// executor (real ThreadPool in production, SynchronousExecutor in
// tests). All dependencies are injected via the constructor — no
// hidden globals.
//
// Lifecycle:
//   1. Construct with transport + schema cache + executor.
//   2. Call `run()` from the main thread. `run()` blocks: it reads
//      messages from the transport in a loop, dispatches them, and
//      writes responses. Worker threads run parse jobs concurrently.
//   3. `run()` returns 0 on clean LSP shutdown (`shutdown` request
//      acknowledged + `exit` notification received) or non-zero on
//      transport failure.

namespace dss::lsp {

struct DSS_EXPORT LspServerOptions {
    // Default name reported in `Diagnostic.source`.
    std::string diagnosticSource = "dss-code-prime";
};

class DSS_EXPORT LspServer {
public:
    // Inject all collaborators. The server owns transport + executor
    // (both move-only); `schemaCache` is held by reference so callers
    // can share it across server lifetimes.
    LspServer(std::unique_ptr<LspTransport>       transport,
              std::unique_ptr<substrate::IExecutor> executor,
              SchemaCache&                         schemaCache,
              LspServerOptions                     options = {});

    ~LspServer() noexcept;

    LspServer(LspServer const&)            = delete;
    LspServer& operator=(LspServer const&) = delete;
    LspServer(LspServer&&)                 = delete;
    LspServer& operator=(LspServer&&)      = delete;

    // Drive the message loop. Blocks until shutdown+exit OR
    // transport EOF/error. Returns 0 on clean shutdown, non-zero
    // on transport failure.
    int run();

    // Read-only view of the document store (test introspection).
    [[nodiscard]] DocumentStore const& documents() const noexcept {
        return documents_;
    }

    // The workspace's language preference as resolved at `initialize` — the
    // channel that lets the editor tell two assembly dialects apart. Exposed
    // read-only so a test can assert on the RESOLUTION itself, not only on its
    // downstream effect on a document.
    [[nodiscard]] WorkspacePreferenceResult const&
    workspacePreference() const noexcept {
        return workspacePreference_;
    }

private:
    void registerHandlers_();

    // Request handlers — return JSON-serialized result body.
    [[nodiscard]] std::optional<std::string> handleInitialize_(Request const& req);
    [[nodiscard]] std::optional<std::string> handleShutdown_(Request const& req);

    // Semantic request handlers (SE7). Each resolves the document's cached
    // SemanticModel and maps the request position onto the AST/symbol
    // tables. Returns the LSP-spec default (null / []) when no model is
    // available or the position resolves to nothing.
    [[nodiscard]] std::optional<std::string> handleHover_(Request const& req);
    [[nodiscard]] std::optional<std::string> handleDefinition_(Request const& req);
    [[nodiscard]] std::optional<std::string> handleReferences_(Request const& req);
    [[nodiscard]] std::optional<std::string> handleRename_(Request const& req);
    [[nodiscard]] std::optional<std::string> handleCompletion_(Request const& req);
    [[nodiscard]] std::optional<std::string> handleSignatureHelp_(Request const& req);

    // Notification handlers.
    void handleInitialized_(Notification const& n);
    void handleExit_(Notification const& n);
    void handleDidOpen_(Notification const& n);
    void handleDidChange_(Notification const& n);
    void handleDidClose_(Notification const& n);
    void handleDidSave_(Notification const& n);

    // ONE handler for all four `workspace/did*` notifications that can move the
    // manifest set (`didChangeWatchedFiles`, `didCreateFiles`, `didRenameFiles`,
    // `didDeleteFiles`). Their params are deliberately ignored: the preference
    // is re-DERIVED by re-reading the manifests, so the only bit any of them
    // carries that we can act on is "look again".
    void handleWorkspaceManifestsMayHaveChanged_(Notification const& n);

    // The ONE schema-resolution site: a document URI → the schema it must be
    // parsed under, or the REASON there is none. `didOpen` and the liveness
    // refresh both go through it, so a document opened under one preference and
    // a document RE-resolved under a changed one can never disagree about how
    // the answer is computed. Reads `workspacePreference_` ⇒ `run()` thread only.
    struct SchemaResolution {
        std::shared_ptr<dss::GrammarSchema const> schema;  // null ⇒ see `reason`
        std::string                               reason;  // empty iff `schema`
    };
    [[nodiscard]] SchemaResolution resolveSchemaForUri_(std::string const& uri);

    // ── LIVENESS (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE) ──────────
    // Re-read the workspace's project manifests. If the preference they yield
    // DIFFERS from the held one, adopt it and then RE-RESOLVE + REPUBLISH every
    // open document whose schema (or whose reason for having none) changed.
    // Returns true iff the preference changed.
    //
    // ★ THE REPUBLISH IS THE POINT, NOT AN EXTRA. Re-reading the manifests per
    // `didOpen` was REJECTED when this channel was built, because it would let a
    // mid-session edit silently change the meaning of ALREADY-OPEN documents
    // while the editor kept displaying diagnostics computed under the old
    // grammar. That objection is answered by doing the second half — the
    // affected documents are re-parsed and re-published — not by refusing to
    // look. A refresh that changed future resolutions only would reintroduce
    // exactly the defect the original design refused.
    bool refreshWorkspacePreference_();

    // Submit a parse job for `uri`. Captures the current generation
    // from the document store; the worker drops the result if a
    // newer update has bumped it.
    void enqueueParse_(std::string uri);

    // Send a publishDiagnostics notification for `uri` using the
    // store's currently-published diagnostics.
    void publishDiagnostics_(std::string const& uri);

    std::unique_ptr<LspTransport>         transport_;
    std::unique_ptr<substrate::IExecutor> executor_;
    SchemaCache&                   schemaCache_;
    LspServerOptions               options_;
    MethodDispatcher               dispatcher_;
    DocumentStore                  documents_;

    // Workspace folders named by `initialize`, and the language preference
    // derived from their project manifests. `workspaceRoots_` is written once
    // in `handleInitialize_`; `workspacePreference_` is RE-derived from it by
    // `refreshWorkspacePreference_` whenever the manifest set may have moved.
    // Every writer and reader runs on the `run()` thread, so neither needs a
    // lock. Parse workers never see them.
    //
    // The pre-`initialize` value is a LOUD one, not an empty preference: a
    // default-constructed `expected` would read as "a preference exists and
    // names nothing", which is the silent-answer shape. A client that opens a
    // document before initializing gets a reason that says exactly that — which
    // is why `refreshWorkspacePreference_` refuses to run before `initialize`
    // (`initializeReceived_`): recomputing would overwrite "the client has not
    // sent `initialize` yet" with the merely-plausible "the client named no
    // workspace folder", losing the distinction on purpose.
    bool                               initializeReceived_ = false;
    std::vector<std::filesystem::path> workspaceRoots_;
    WorkspacePreferenceResult          workspacePreference_ =
        std::unexpected(WorkspaceProjectError{
            WorkspaceProjectErrorKind::NoWorkspaceRoot,
            "the client has not sent `initialize` yet, so no workspace folder "
            "is known"});

    // `shutdownReceived_` flips on `shutdown` — observed by `run()`
    // to distinguish a clean (exit-code 0) EOF from a premature one
    // (exit-code 1). `exitReceived_` flips on `exit` — run loop
    // exits at the top of the next iteration.
    std::atomic<bool>              shutdownReceived_{false};
    std::atomic<bool>              exitReceived_{false};
};

} // namespace dss::lsp
