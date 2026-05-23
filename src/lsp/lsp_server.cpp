#include "lsp/lsp_server.hpp"

#include "analysis/syntactic/parser.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "lsp/diagnostic_translator.hpp"
#include "lsp/json_rpc.hpp"
#include "tokenizer/tokenizer.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace dss::lsp {

namespace {

using json = nlohmann::json;

// Extract a file extension (with leading dot, lowercased) from a
// `file://` URI. Returns empty string if the URI has no extension.
[[nodiscard]] std::string extensionFromUri(std::string_view uri) {
    // Best-effort: look for the last '.' after the last '/'.
    auto lastSlash = uri.find_last_of('/');
    auto search = (lastSlash == std::string_view::npos)
        ? uri : uri.substr(lastSlash + 1);
    auto dot = search.find_last_of('.');
    if (dot == std::string_view::npos) return {};
    std::string ext{search.substr(dot)};
    for (auto& c : ext) c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

// Parse a `TextDocumentItem` from the params JSON.
struct TextDocumentItem {
    std::string  uri;
    std::int32_t version = 0;
    std::string  text;
};

[[nodiscard]] TextDocumentItem parseTextDocumentItem(json const& params) {
    TextDocumentItem item;
    if (auto td = params.find("textDocument"); td != params.end()) {
        if (auto u = td->find("uri"); u != td->end() && u->is_string()) {
            item.uri = u->get<std::string>();
        }
        if (auto v = td->find("version"); v != td->end() && v->is_number_integer()) {
            item.version = v->get<std::int32_t>();
        }
        if (auto t = td->find("text"); t != td->end() && t->is_string()) {
            item.text = t->get<std::string>();
        }
    }
    return item;
}

// Parse a didChange params. Iterates `contentChanges` and keeps the
// last entry that lacks a `range` (full-content sync). Incremental
// range-based edits are a future concern.
struct DidChangeParams {
    std::string  uri;
    std::int32_t version = 0;
    std::string  text;
    bool         hasFullContent = false;
};

[[nodiscard]] DidChangeParams parseDidChange(json const& params) {
    DidChangeParams out;
    if (auto td = params.find("textDocument"); td != params.end()) {
        if (auto u = td->find("uri"); u != td->end() && u->is_string()) {
            out.uri = u->get<std::string>();
        }
        if (auto v = td->find("version"); v != td->end() && v->is_number_integer()) {
            out.version = v->get<std::int32_t>();
        }
    }
    if (auto cc = params.find("contentChanges"); cc != params.end() && cc->is_array()) {
        for (auto const& change : *cc) {
            // Full-content change has only a "text" field; range-
            // based incremental change has "range" + "text".
            if (!change.contains("range")) {
                if (auto t = change.find("text"); t != change.end() && t->is_string()) {
                    out.text           = t->get<std::string>();
                    out.hasFullContent = true;
                }
            }
        }
    }
    return out;
}

[[nodiscard]] std::string parseUriOnly(json const& params) {
    if (auto td = params.find("textDocument"); td != params.end()) {
        if (auto u = td->find("uri"); u != td->end() && u->is_string()) {
            return u->get<std::string>();
        }
    }
    return {};
}

} // namespace

LspServer::LspServer(std::unique_ptr<LspTransport> transport,
                     std::unique_ptr<IExecutor>    executor,
                     SchemaCache&                  schemaCache,
                     LspServerOptions              options)
    : transport_(std::move(transport))
    , executor_(std::move(executor))
    , schemaCache_(schemaCache)
    , options_(std::move(options)) {
    registerHandlers_();
}

LspServer::~LspServer() noexcept {
    if (transport_) transport_->close();
    if (executor_)  executor_->shutdown();
}

void LspServer::registerHandlers_() {
    dispatcher_.registerRequest(Method::Initialize,
        [this](Request const& r) { return handleInitialize_(r); });
    dispatcher_.registerRequest(Method::Shutdown,
        [this](Request const& r) { return handleShutdown_(r); });

    dispatcher_.registerNotification(Method::Initialized,
        [this](Notification const& n) { handleInitialized_(n); });
    dispatcher_.registerNotification(Method::Exit,
        [this](Notification const& n) { handleExit_(n); });
    dispatcher_.registerNotification(Method::TextDocumentDidOpen,
        [this](Notification const& n) { handleDidOpen_(n); });
    dispatcher_.registerNotification(Method::TextDocumentDidChange,
        [this](Notification const& n) { handleDidChange_(n); });
    dispatcher_.registerNotification(Method::TextDocumentDidClose,
        [this](Notification const& n) { handleDidClose_(n); });
    dispatcher_.registerNotification(Method::TextDocumentDidSave,
        [this](Notification const& n) { handleDidSave_(n); });
}

int LspServer::run() {
    while (!exitReceived_.load(std::memory_order_acquire)) {
        auto msg = transport_->readMessage();
        if (!msg.has_value()) {
            // EOF or transport error — exit the loop. Clean EOF
            // before `exit` is treated as exit code 0 if shutdown
            // was already received, 1 otherwise (LSP convention:
            // exiting without explicit `exit` is an error).
            return shutdownReceived_.load(std::memory_order_acquire) ? 0 : 1;
        }
        auto parsed = JsonRpc::parse(*msg);
        if (!parsed.has_value()) {
            // Reply with a parse-error response (JSON-RPC §5.1).
            // No id available — server-initiated error.
            const auto err = JsonRpc::serializeError(
                LspId{std::monostate{}}, -32700,
                std::string{"Parse error: "} + parsed.error().detail);
            (void)transport_->writeMessage(err);
            continue;
        }
        auto response = dispatcher_.dispatch(*parsed);
        if (response.has_value()) {
            (void)transport_->writeMessage(*response);
        }
    }
    // Clean exit: drain workers before returning.
    executor_->shutdown();
    return 0;
}

std::optional<std::string> LspServer::handleInitialize_(Request const& /*req*/) {
    json result;
    result["capabilities"]["textDocumentSync"]      = 1; // 1 == Full per LSP §13.7
    result["capabilities"]["positionEncoding"]      = "utf-16";
    result["capabilities"]["diagnosticProvider"]["interFileDependencies"] = false;
    result["capabilities"]["diagnosticProvider"]["workspaceDiagnostics"]  = false;
    result["serverInfo"]["name"]    = options_.diagnosticSource;
    result["serverInfo"]["version"] = "0.1.0";
    return result.dump();
}

std::optional<std::string> LspServer::handleShutdown_(Request const& /*req*/) {
    shutdownReceived_.store(true, std::memory_order_release);
    return std::string{"null"};
}

void LspServer::handleInitialized_(Notification const& /*n*/) {
}

void LspServer::handleExit_(Notification const& /*n*/) {
    exitReceived_.store(true, std::memory_order_release);
    // Drain in-flight parse workers BEFORE closing the transport so
    // their last publishDiagnostics writes still go through (LSP
    // forbids traffic after `exit` — but already-running workers
    // would emit anyway; this lets them finish cleanly).
    if (executor_)  executor_->shutdown();
    if (transport_) transport_->close();
}

void LspServer::handleDidOpen_(Notification const& n) {
    if (n.params.empty()) return;
    json params;
    try { params = json::parse(n.params); }
    catch (...) { return; }
    const auto item = parseTextDocumentItem(params);
    if (item.uri.empty()) return;

    // Resolve schema by file extension.
    std::shared_ptr<dss::GrammarSchema const> schema;
    const auto ext = extensionFromUri(item.uri);
    if (!ext.empty()) {
        auto resolved = schemaCache_.resolveByExtension(ext);
        if (resolved.has_value()) schema = *resolved;
    }
    documents_.open(item.uri, item.version, item.text, schema);
    enqueueParse_(item.uri);
}

void LspServer::handleDidChange_(Notification const& n) {
    if (n.params.empty()) return;
    json params;
    try { params = json::parse(n.params); }
    catch (...) { return; }
    const auto change = parseDidChange(params);
    if (change.uri.empty() || !change.hasFullContent) return;
    (void)documents_.update(change.uri, change.version, change.text);
    enqueueParse_(change.uri);
}

void LspServer::handleDidClose_(Notification const& n) {
    if (n.params.empty()) return;
    json params;
    try { params = json::parse(n.params); }
    catch (...) { return; }
    const auto uri = parseUriOnly(params);
    if (uri.empty()) return;
    documents_.close(uri);

    // LSP spec: clear diagnostics for the closed URI by publishing
    // an empty diagnostics array.
    PublishDiagnosticsParams pdp;
    pdp.uri = uri;
    const auto body = serializePublishDiagnostics(pdp);
    const auto notif = JsonRpc::serializeNotification(
        "textDocument/publishDiagnostics", body);
    (void)transport_->writeMessage(notif);
}

void LspServer::handleDidSave_(Notification const& /*n*/) {
    // didChange already re-parses on every edit.
}

void LspServer::enqueueParse_(std::string uri) {
    auto snap = documents_.snapshot(uri);
    if (!snap.has_value()) return;

    executor_->submit([this,
                       uri    = std::move(uri),
                       snap   = std::move(*snap)]() mutable {
        if (!snap.schema) {
            // No schema — clear any prior diagnostics.
            std::vector<dss::ParseDiagnostic> empty;
            if (documents_.setDiagnostics(uri, snap.parseGeneration,
                                           std::move(empty))) {
                publishDiagnostics_(uri);
            }
            return;
        }
        auto src = dss::SourceBuffer::fromString(snap.text, uri);
        dss::Tokenizer tk{src, snap.schema};
        auto [stream, lexDiags] = std::move(tk).tokenize();
        dss::Parser p{src, snap.schema, std::move(stream)};
        auto result = std::move(p).parse();

        // Copy out: the span aliases tree storage that dies at scope end.
        std::vector<dss::ParseDiagnostic> diags(
            result.tree.diagnostics().all().begin(),
            result.tree.diagnostics().all().end());

        if (documents_.setDiagnostics(uri, snap.parseGeneration,
                                       std::move(diags))) {
            publishDiagnostics_(uri);
        }
    });
}

void LspServer::publishDiagnostics_(std::string const& uri) {
    auto snap = documents_.snapshot(uri);
    if (!snap.has_value()) return;
    auto diags = documents_.diagnosticsFor(uri);

    // Re-construct a SourceBuffer over the document's current text
    // so the translator can compute line/col + UTF-16 columns.
    // Cheap: SourceBuffer's ctor just builds the line-offset table.
    auto src = dss::SourceBuffer::fromString(snap->text, uri);
    PublishDiagnosticsParams params;
    params.uri         = uri;
    params.version     = snap->clientVersion;
    params.diagnostics = translateDiagnostics(
        std::span<dss::ParseDiagnostic const>{diags}, *src);
    for (auto& d : params.diagnostics) {
        d.source = options_.diagnosticSource;
    }
    const auto body  = serializePublishDiagnostics(params);
    const auto notif = JsonRpc::serializeNotification(
        "textDocument/publishDiagnostics", body);
    (void)transport_->writeMessage(notif);
}

} // namespace dss::lsp
