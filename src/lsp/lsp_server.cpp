#include "lsp/lsp_server.hpp"

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lsp/diagnostic_translator.hpp"
#include "lsp/json_rpc.hpp"
#include "lsp/lsp_semantic_query.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::lsp {

namespace {

using json = nlohmann::json;

// Extract a file extension (with leading dot, lowercased) from a
// `file://` URI. Returns empty string if the URI has no extension.
[[nodiscard]] std::string extensionFromUri(std::string_view uri) {
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

// Common prelude for notification handlers: bail on empty or
// malformed params. Returns std::nullopt to signal "drop the
// notification silently"; caller returns to the dispatch loop.
[[nodiscard]] std::optional<json> tryParseParams(Notification const& n) {
    if (n.params.empty()) return std::nullopt;
    try { return json::parse(n.params); }
    catch (...) { return std::nullopt; }
}

// A `TextDocumentPositionParams` (uri + {line, character}). Used by every
// semantic request handler.
struct TextDocumentPosition {
    std::string uri;
    Position    position;
};

[[nodiscard]] std::optional<TextDocumentPosition>
parseTextDocumentPosition(Request const& req) {
    if (req.params.empty()) return std::nullopt;
    json params;
    try { params = json::parse(req.params); }
    catch (...) { return std::nullopt; }
    TextDocumentPosition out;
    if (auto td = params.find("textDocument"); td != params.end()) {
        if (auto u = td->find("uri"); u != td->end() && u->is_string()) {
            out.uri = u->get<std::string>();
        }
    }
    if (auto p = params.find("position"); p != params.end()) {
        if (auto l = p->find("line"); l != p->end() && l->is_number_integer()) {
            out.position.line = l->get<std::uint32_t>();
        }
        if (auto c = p->find("character"); c != p->end() && c->is_number_integer()) {
            out.position.character = c->get<std::uint32_t>();
        }
    }
    if (out.uri.empty()) return std::nullopt;
    return out;
}

// Render a TypeId to a short human string for hover / completion detail.
// Uses the interner's structural kind + nominal name; FnSig formats as
// `(params) -> result`. Keeps it minimal — no full pretty-printer.
[[nodiscard]] std::string typeString(dss::TypeInterner const& interner,
                                     dss::TypeId ty) {
    if (!ty.valid()) return "<unknown>";
    auto kindName = [](dss::TypeKind k) -> std::string {
        switch (k) {
            case dss::TypeKind::Bool: return "bool";
            case dss::TypeKind::I8:   return "i8";
            case dss::TypeKind::I16:  return "i16";
            case dss::TypeKind::I32:  return "i32";
            case dss::TypeKind::I64:  return "i64";
            case dss::TypeKind::I128: return "i128";
            case dss::TypeKind::U8:   return "u8";
            case dss::TypeKind::U16:  return "u16";
            case dss::TypeKind::U32:  return "u32";
            case dss::TypeKind::U64:  return "u64";
            case dss::TypeKind::U128: return "u128";
            case dss::TypeKind::F16:  return "f16";
            case dss::TypeKind::F32:  return "f32";
            case dss::TypeKind::F64:  return "f64";
            case dss::TypeKind::F80:  return "f80";
            case dss::TypeKind::F128: return "f128";
            case dss::TypeKind::Char: return "char";
            case dss::TypeKind::Byte: return "byte";
            case dss::TypeKind::Void: return "void";
            default:                  return "type";
        }
    };
    const auto k = interner.kind(ty);
    if (k == dss::TypeKind::FnSig) {
        std::string s = "(";
        auto params = interner.fnParams(ty);
        for (std::size_t i = 0; i < params.size(); ++i) {
            if (i > 0) s += ", ";
            s += kindName(interner.kind(params[i]));
        }
        s += ") -> ";
        s += kindName(interner.kind(interner.fnResult(ty)));
        return s;
    }
    auto nm = interner.name(ty);
    if (!nm.empty()) return std::string{nm};
    return kindName(k);
}

// LSP SymbolKind-ish detail string for a declaration kind (used as the
// `kind` field hint and the hover label prefix).
[[nodiscard]] std::string_view declKindLabel(dss::DeclarationKind k) {
    switch (k) {
        case dss::DeclarationKind::Variable: return "variable";
        case dss::DeclarationKind::Function: return "function";
        case dss::DeclarationKind::Table:    return "table";
        case dss::DeclarationKind::Type:     return "type";
    }
    return "symbol";
}

// LSP CompletionItemKind (LSP §10.18): 6=Variable, 3=Function, 7=Class
// (used for table/type), 22=Struct. Map our DeclarationKind onto the
// closest wire value.
[[nodiscard]] int completionItemKind(dss::DeclarationKind k) {
    switch (k) {
        case dss::DeclarationKind::Variable: return 6;
        case dss::DeclarationKind::Function: return 3;
        case dss::DeclarationKind::Table:    return 7;
        case dss::DeclarationKind::Type:     return 7;
    }
    return 6;
}

[[nodiscard]] json rangeJson(Range const& r) {
    return json{
        {"start", {{"line", r.start.line}, {"character", r.start.character}}},
        {"end",   {{"line", r.end.line},   {"character", r.end.character}}},
    };
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
    // Drain workers before closing transport so any in-flight
    // writeMessage completes against the open stream (matches
    // handleExit_'s ordering).
    if (executor_)  executor_->shutdown();
    if (transport_) transport_->close();
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

    // Semantic request handlers (SE7) — backed by the cached SemanticModel.
    dispatcher_.registerRequest(Method::TextDocumentHover,
        [this](Request const& r) { return handleHover_(r); });
    dispatcher_.registerRequest(Method::TextDocumentCompletion,
        [this](Request const& r) { return handleCompletion_(r); });
    dispatcher_.registerRequest(Method::TextDocumentDefinition,
        [this](Request const& r) { return handleDefinition_(r); });
    dispatcher_.registerRequest(Method::TextDocumentReferences,
        [this](Request const& r) { return handleReferences_(r); });
    dispatcher_.registerRequest(Method::TextDocumentRename,
        [this](Request const& r) { return handleRename_(r); });
    dispatcher_.registerRequest(Method::TextDocumentSignatureHelp,
        [this](Request const& r) { return handleSignatureHelp_(r); });
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
    // LSP §3.6: `exit` without prior `shutdown` is an error (exit 1);
    // with prior `shutdown` it's a clean teardown (exit 0). The
    // executor was already drained by handleExit_; no second call.
    return shutdownReceived_.load(std::memory_order_acquire) ? 0 : 1;
}

std::optional<std::string> LspServer::handleInitialize_(Request const& /*req*/) {
    json result;
    auto& caps = result["capabilities"];
    caps["textDocumentSync"] = 1; // 1 == Full per LSP §13.7
    caps["positionEncoding"] = "utf-16";
    caps["diagnosticProvider"]["interFileDependencies"] = false;
    caps["diagnosticProvider"]["workspaceDiagnostics"]  = false;
    // hover/definition/references/rename accept `bool | <T>Options`
    // per LSP §10. completion + signatureHelp REQUIRE the options
    // object form (§10.18, §10.20) — `true` would be invalid. The
    // shape asymmetry is the spec's, not ours.
    caps["hoverProvider"]          = true;
    caps["completionProvider"]     = json::object();
    caps["definitionProvider"]     = true;
    caps["referencesProvider"]     = true;
    caps["renameProvider"]         = true;
    caps["signatureHelpProvider"]  = json::object();
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
    auto params = tryParseParams(n);
    if (!params) return;
    const auto item = parseTextDocumentItem(*params);
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
    auto params = tryParseParams(n);
    if (!params) return;
    const auto change = parseDidChange(*params);
    if (change.uri.empty() || !change.hasFullContent) return;
    (void)documents_.update(change.uri, change.version, change.text);
    enqueueParse_(change.uri);
}

void LspServer::handleDidClose_(Notification const& n) {
    auto params = tryParseParams(n);
    if (!params) return;
    const auto uri = parseUriOnly(*params);
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
        // Build a single-file CompilationUnit and run full semantic
        // analysis. The CU must outlive the SemanticModel (its side-tables
        // hold raw Tree*), so we wrap it in a shared_ptr and hand it to
        // analyze(), which keeps its own shared_ptr inside the model.
        dss::UnitBuilder builder{snap.schema};
        builder.addInMemory(snap.text, uri);
        auto cu = std::make_shared<dss::CompilationUnit>(
            std::move(builder).finish());
        auto model = std::make_shared<dss::SemanticModel const>(
            dss::analyze(cu));

        // Union the per-tree parse diagnostics (lexer + parser, folded by
        // UnitBuilder) with the semantic diagnostics for publishing.
        std::vector<dss::ParseDiagnostic> diags;
        if (!cu->trees().empty()) {
            auto parseDiags = cu->trees()[0].diagnostics().all();
            diags.assign(parseDiags.begin(), parseDiags.end());
        }
        auto semDiags = model->diagnostics().all();
        diags.insert(diags.end(), semDiags.begin(), semDiags.end());

        // Store the model under the same generation guard, then publish.
        // setDiagnostics gates on generation too — a newer edit drops both.
        const bool applied =
            documents_.setDiagnostics(uri, snap.parseGeneration,
                                      std::move(diags));
        (void)documents_.setSemanticModel(uri, snap.parseGeneration,
                                          std::move(model));
        if (applied) {
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

// ── Semantic request handlers (SE7) ────────────────────────────────────

namespace {

// Resolve the (model, tree, byteOffset, node) tuple a position-based
// handler needs. Returns false when no model/tree/node is available — the
// caller returns the LSP default. The tree is always trees()[0] (single-
// file CU per LSP document).
struct ResolvedQuery {
    std::shared_ptr<dss::SemanticModel const> model;
    dss::Tree const*                          tree   = nullptr;
    dss::ByteOffset                           offset{};
    NodeId                                    node{};
};

[[nodiscard]] bool resolveQuery(DocumentStore const& docs,
                                TextDocumentPosition const& tp,
                                ResolvedQuery& out) {
    out.model = docs.semanticModelFor(tp.uri);
    if (!out.model) return false;
    auto trees = out.model->unit().trees();
    if (trees.empty() || !trees[0].root().valid()) return false;
    out.tree   = &trees[0];
    out.offset = positionToByteOffset(out.tree->source(), tp.position);
    out.node   = nodeAtOffset(*out.tree, out.offset);
    return out.node.valid();
}

// A Location {uri, range} for a node's span in `tree`.
[[nodiscard]] json locationJson(std::string const& uri, dss::Tree const& tree,
                                NodeId node) {
    return json{
        {"uri", uri},
        {"range", rangeJson(spanToRange(tree.source(), tree.span(node)))},
    };
}

} // namespace

std::optional<std::string> LspServer::handleHover_(Request const& req) {
    auto tp = parseTextDocumentPosition(req);
    if (!tp) return std::string{"null"};
    ResolvedQuery q;
    if (!resolveQuery(documents_, *tp, q)) return std::string{"null"};

    const SymbolId sym = q.model->symbolAt(q.node);
    auto const* rec = q.model->recordFor(sym);
    if (rec == nullptr) {
        // No symbol bound here; fall back to the node's own type if any.
        const dss::TypeId ty = q.model->typeAt(q.node);
        if (!ty.valid()) return std::string{"null"};
        json result;
        result["contents"] = {
            {"kind", "markdown"},
            {"value", "```\n" + typeString(q.model->lattice().interner(), ty)
                      + "\n```"},
        };
        result["range"] = rangeJson(spanToRange(q.tree->source(),
                                                q.tree->span(q.node)));
        return result.dump();
    }

    auto const& interner = q.model->lattice().interner();
    std::string md = "```\n";
    md += std::string{declKindLabel(rec->kind)};
    md += ' ';
    md += rec->name;
    if (rec->type.valid()) {
        md += ": ";
        md += typeString(interner, rec->type);
    }
    md += "\n```";

    json result;
    result["contents"] = {{"kind", "markdown"}, {"value", md}};
    result["range"] = rangeJson(spanToRange(q.tree->source(),
                                            q.tree->span(q.node)));
    return result.dump();
}

std::optional<std::string> LspServer::handleDefinition_(Request const& req) {
    auto tp = parseTextDocumentPosition(req);
    if (!tp) return std::string{"null"};
    ResolvedQuery q;
    if (!resolveQuery(documents_, *tp, q)) return std::string{"null"};

    const SymbolId sym = q.model->symbolAt(q.node);
    auto const* rec = q.model->recordFor(sym);
    if (rec == nullptr || !rec->declNode.valid()) return std::string{"null"};
    // The decl node belongs to the symbol's tree; for a single-file CU
    // that is the same tree. Resolve via the model's CU trees by id.
    dss::Tree const* declTree = q.tree;
    for (auto const& t : q.model->unit().trees()) {
        if (t.id().v == rec->tree.v) { declTree = &t; break; }
    }
    return locationJson(tp->uri, *declTree, rec->declNode).dump();
}

std::optional<std::string> LspServer::handleReferences_(Request const& req) {
    auto tp = parseTextDocumentPosition(req);
    if (!tp) return std::string{"[]"};
    ResolvedQuery q;
    if (!resolveQuery(documents_, *tp, q)) return std::string{"[]"};

    const SymbolId sym = q.model->symbolAt(q.node);
    auto const* rec = q.model->recordFor(sym);
    if (rec == nullptr) return std::string{"[]"};

    // includeDeclaration defaults true per LSP; honor context if present.
    bool includeDecl = true;
    try {
        auto params = json::parse(req.params);
        if (auto ctx = params.find("context"); ctx != params.end()) {
            if (auto inc = ctx->find("includeDeclaration");
                inc != ctx->end() && inc->is_boolean()) {
                includeDecl = inc->get<bool>();
            }
        }
    } catch (...) { /* keep default */ }

    json arr = json::array();
    if (includeDecl && rec->declNode.valid()) {
        arr.push_back(locationJson(tp->uri, *q.tree, rec->declNode));
    }
    for (NodeId use : q.model->usesOf(sym)) {
        arr.push_back(locationJson(tp->uri, *q.tree, use));
    }
    return arr.dump();
}

std::optional<std::string> LspServer::handleRename_(Request const& req) {
    auto tp = parseTextDocumentPosition(req);
    if (!tp) return std::string{"null"};
    ResolvedQuery q;
    if (!resolveQuery(documents_, *tp, q)) return std::string{"null"};

    std::string newName;
    try {
        auto params = json::parse(req.params);
        if (auto n = params.find("newName"); n != params.end() && n->is_string()) {
            newName = n->get<std::string>();
        }
    } catch (...) { return std::string{"null"}; }
    if (newName.empty()) return std::string{"null"};

    const SymbolId sym = q.model->symbolAt(q.node);
    auto const* rec = q.model->recordFor(sym);
    if (rec == nullptr) return std::string{"null"};

    json edits = json::array();
    auto pushEdit = [&](NodeId n) {
        edits.push_back(json{
            {"range", rangeJson(spanToRange(q.tree->source(), q.tree->span(n)))},
            {"newText", newName},
        });
    };
    if (rec->declNode.valid()) pushEdit(rec->declNode);
    for (NodeId use : q.model->usesOf(sym)) pushEdit(use);

    json result;
    result["changes"] = json::object();
    result["changes"][tp->uri] = std::move(edits);
    return result.dump();
}

std::optional<std::string> LspServer::handleCompletion_(Request const& req) {
    auto tp = parseTextDocumentPosition(req);
    if (!tp) return std::string{"null"};
    auto model = documents_.semanticModelFor(tp->uri);
    if (!model) return std::string{"null"};
    auto trees = model->unit().trees();
    if (trees.empty() || !trees[0].root().valid()) return std::string{"null"};
    dss::Tree const& tree = trees[0];
    const dss::ByteOffset offset =
        positionToByteOffset(tree.source(), tp->position);

    // Find the deepest scope containing the offset, then collect bindings
    // up the parent chain (inner shadows outer — first-seen wins).
    auto const& interner = model->lattice().interner();
    auto const& scopes   = model->scopes();
    ScopeId scope = scopeAtOffset(*model, tree, offset);

    std::unordered_map<std::string, SymbolId> visible;
    while (scope.valid() && scope.v < scopes.size()) {
        for (auto const& [name, symId] : scopes[scope.v].bindings) {
            visible.emplace(name, symId);  // inner (earlier) wins
        }
        scope = scopes[scope.v].parent;
    }

    json items = json::array();
    for (auto const& [name, symId] : visible) {
        auto const* rec = model->recordFor(symId);
        if (rec == nullptr) continue;
        json item;
        item["label"] = name;
        item["kind"]  = completionItemKind(rec->kind);
        std::string detail{declKindLabel(rec->kind)};
        if (rec->type.valid()) {
            detail += ": ";
            detail += typeString(interner, rec->type);
        }
        item["detail"] = detail;
        items.push_back(std::move(item));
    }
    return items.dump();
}

std::optional<std::string> LspServer::handleSignatureHelp_(Request const& req) {
    auto tp = parseTextDocumentPosition(req);
    if (!tp) return std::string{"null"};
    ResolvedQuery q;
    if (!resolveQuery(documents_, *tp, q)) return std::string{"null"};

    // Walk ancestors to find an enclosing call-rule node, then resolve its
    // callee to a FnSig. callRules come from the schema's SemanticConfig.
    auto const& cfg = q.model->unit().schema().semantics();
    auto const& interner = q.model->lattice().interner();

    dss::TreeCursor cursor{*q.tree, q.node, dss::CursorMode::Ast};
    for (;;) {
        const NodeId cur = cursor.current();
        if (q.tree->kind(cur) == NodeKind::Internal) {
            const auto rule = q.tree->rule(cur);
            for (auto const& cr : cfg.callRules) {
                if (cr.rule.v != rule.v) continue;
                // Resolve the callee child to a symbol via its bound node.
                std::vector<NodeId> kids;
                for (NodeId c : q.tree->children(cur)) {
                    if (!isEmptySpace(q.tree->flags(c))) kids.push_back(c);
                }
                if (cr.calleeChild >= kids.size()) break;
                // Resolve the callee: prefer the SymbolId already bound to
                // the callee leaf (when it sits under a reference rule);
                // otherwise fall back to a scope-chain lookup by name (a
                // call callee — e.g. tsql's COALESCE — is not itself a
                // reference node, so the engine resolves it by name; we
                // mirror that here).
                NodeId calleeLeaf = kids[cr.calleeChild];
                SymbolId calleeSym = q.model->symbolAt(calleeLeaf);
                if (!calleeSym.valid()) {
                    for (NodeId c : q.tree->children(calleeLeaf)) {
                        if (isEmptySpace(q.tree->flags(c))) continue;
                        calleeSym = q.model->symbolAt(c);
                        if (calleeSym.valid()) break;
                    }
                }
                if (!calleeSym.valid()) {
                    // Name-based scope lookup. The callee leaf is a token
                    // (or wraps one); take its text and search the scope
                    // chain from the call site.
                    std::string_view calleeText = q.tree->text(calleeLeaf);
                    ScopeId scope = scopeAtOffset(*q.model, *q.tree, q.offset);
                    auto const& scopes = q.model->scopes();
                    while (scope.valid() && scope.v < scopes.size()) {
                        auto it = scopes[scope.v].bindings.find(
                            std::string{calleeText});
                        if (it != scopes[scope.v].bindings.end()) {
                            calleeSym = it->second;
                            break;
                        }
                        scope = scopes[scope.v].parent;
                    }
                }
                auto const* rec = q.model->recordFor(calleeSym);
                if (rec == nullptr || !rec->type.valid()
                    || interner.kind(rec->type) != dss::TypeKind::FnSig) {
                    return std::string{"null"};
                }
                auto params = interner.fnParams(rec->type);
                json paramArr = json::array();
                std::string label = rec->name + "(";
                for (std::size_t i = 0; i < params.size(); ++i) {
                    if (i > 0) label += ", ";
                    const std::string pstr = typeString(interner, params[i]);
                    label += pstr;
                    paramArr.push_back(json{{"label", pstr}});
                }
                label += ") -> ";
                label += typeString(interner, interner.fnResult(rec->type));

                json sig;
                sig["label"]      = label;
                sig["parameters"] = std::move(paramArr);
                json result;
                result["signatures"]      = json::array({std::move(sig)});
                result["activeSignature"] = 0;
                result["activeParameter"] = 0;
                return result.dump();
            }
        }
        if (!cursor.gotoParent()) break;
    }
    return std::string{"null"};
}

} // namespace dss::lsp
