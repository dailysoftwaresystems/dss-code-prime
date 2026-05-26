#pragma once

#include "core/export.hpp"
#include "lsp/document_store.hpp"
#include "lsp/method_dispatcher.hpp"
#include "lsp/protocol.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"

#include <atomic>
#include <memory>

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
    LspServer(std::unique_ptr<LspTransport> transport,
              std::unique_ptr<IExecutor>    executor,
              SchemaCache&                  schemaCache,
              LspServerOptions              options = {});

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

    // Submit a parse job for `uri`. Captures the current generation
    // from the document store; the worker drops the result if a
    // newer update has bumped it.
    void enqueueParse_(std::string uri);

    // Send a publishDiagnostics notification for `uri` using the
    // store's currently-published diagnostics.
    void publishDiagnostics_(std::string const& uri);

    std::unique_ptr<LspTransport>  transport_;
    std::unique_ptr<IExecutor>     executor_;
    SchemaCache&                   schemaCache_;
    LspServerOptions               options_;
    MethodDispatcher               dispatcher_;
    DocumentStore                  documents_;

    // `shutdownReceived_` flips on `shutdown` — observed by `run()`
    // to distinguish a clean (exit-code 0) EOF from a premature one
    // (exit-code 1). `exitReceived_` flips on `exit` — run loop
    // exits at the top of the next iteration.
    std::atomic<bool>              shutdownReceived_{false};
    std::atomic<bool>              exitReceived_{false};
};

} // namespace dss::lsp
