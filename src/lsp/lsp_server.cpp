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
#include "lsp/workspace_project.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
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

// Every workspace folder the `initialize` request named, deduplicated, in the
// order the client listed them.
//
// ALL THREE SPELLINGS, UNIONED. LSP has deprecated `rootPath` in favour of
// `rootUri` and `rootUri` in favour of `workspaceFolders`, but real clients
// still send the older keys (and often several at once), so reading only the
// modern one would leave whole editors with no workspace. Deduplication makes
// the overlap harmless.
//
// ★ EVERY folder, not the first. A multi-root workspace that declared two
// projects and got answered from folder[0] would be first-wins with extra
// steps — the same defect one layer up. Taking all of them means two folders
// that disagree produce an AMBIGUITY, which is the truth.
[[nodiscard]] std::vector<std::filesystem::path>
workspaceRootsFromInitialize(json const& params) {
    std::vector<std::filesystem::path> roots;
    auto push = [&roots](std::filesystem::path p) {
        if (std::find(roots.begin(), roots.end(), p) == roots.end()) {
            roots.push_back(std::move(p));
        }
    };
    auto pushUri = [&push](std::string const& uri) {
        // A non-`file:` root (a virtual or remote workspace) yields nullopt and
        // is DROPPED rather than guessed at — there is no local directory to
        // scan, and inventing one would answer with the wrong project.
        if (auto p = pathFromFileUri(uri)) push(std::move(*p));
    };
    if (auto wf = params.find("workspaceFolders");
        wf != params.end() && wf->is_array()) {
        for (auto const& folder : *wf) {
            if (auto u = folder.find("uri");
                u != folder.end() && u->is_string()) {
                pushUri(u->get<std::string>());
            }
        }
    }
    if (auto ru = params.find("rootUri"); ru != params.end() && ru->is_string()) {
        pushUri(ru->get<std::string>());
    }
    if (auto rp = params.find("rootPath"); rp != params.end() && rp->is_string()) {
        // `rootPath` is a plain filesystem path, not a URI — no scheme, no
        // percent-encoding.
        auto raw = rp->get<std::string>();
        if (!raw.empty()) push(std::filesystem::path{raw});
    }
    return roots;
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

LspServer::LspServer(std::unique_ptr<LspTransport>         transport,
                     std::unique_ptr<substrate::IExecutor> executor,
                     SchemaCache&                          schemaCache,
                     LspServerOptions                      options)
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

    // ── The four workspace notifications that can move the manifest set ─────
    // (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE). ONE handler for all
    // four: none of their params tells us anything the filesystem does not, so
    // discriminating between them would be a switch whose arms are identical.
    for (auto m : {Method::WorkspaceDidChangeWatchedFiles,
                   Method::WorkspaceDidCreateFiles,
                   Method::WorkspaceDidRenameFiles,
                   Method::WorkspaceDidDeleteFiles}) {
        dispatcher_.registerNotification(m,
            [this](Notification const& n) {
                handleWorkspaceManifestsMayHaveChanged_(n);
            });
    }

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

std::optional<std::string> LspServer::handleInitialize_(Request const& req) {
    // ── The workspace's TARGET channel (D-LSP-ASSEMBLY-DIALECT-UNSERVABLE) ──
    // `initialize` is the ONLY message that carries the workspace ROOTS, and a
    // root is the only thing that leads to a project manifest, so this is where
    // the editor learns which CPU its `.s` files belong to. The ROOTS are fixed
    // here; the PREFERENCE derived from them is not — see
    // `refreshWorkspacePreference_` (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE),
    // which re-derives it whenever the manifest set may have moved
    // and republishes the open documents whose answer changed.
    //
    // Threading: every dispatcher handler runs on the `run()` thread, so this
    // write and the `handleDidOpen_` read below need no synchronisation. Worker
    // threads never touch `workspacePreference_` — they see only the per-
    // document `schemaError` string, which the DocumentStore's mutex covers.
    if (!req.params.empty()) {
        try {
            auto params      = json::parse(req.params);
            workspaceRoots_  = workspaceRootsFromInitialize(params);
        } catch (...) {
            // Malformed params: keep the empty root list. The resulting
            // `NoWorkspaceRoot` reason is the honest one — nothing named a
            // workspace — and it reaches the user on the first ambiguous open.
            workspaceRoots_.clear();
        }
    }
    initializeReceived_ = true;
    // No document can be open yet, so the republish half of this is a no-op and
    // the call reduces to the first resolution. Going through the SAME function
    // is what stops "how the preference is computed at initialize" and "how it
    // is recomputed later" from drifting apart.
    (void)refreshWorkspacePreference_();

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

    // ── `workspace.fileOperations` — A STATIC REGISTRATION, NO OUTBOUND
    //    REQUEST (LSP 3.16 `ServerCapabilities.workspace.fileOperations`) ─────
    //
    // ✔VERIFIED against Microsoft's own reference implementation
    // (`vscode-languageserver-node/protocol/src/common/protocol.fileOperations.ts`):
    //   FileOperationOptions            { didCreate?/willCreate?/didRename?/… : FileOperationRegistrationOptions }
    //   FileOperationRegistrationOptions{ filters: FileOperationFilter[] }
    //   FileOperationFilter             { scheme?: string; pattern: FileOperationPattern }
    //   FileOperationPattern            { glob: string; matches?: 'file'|'folder'; options?: { ignoreCase?: boolean } }
    // and the wire methods `workspace/didCreateFiles` / `didRenameFiles` /
    // `didDeleteFiles`, gated by the client capability
    // `workspace.fileOperations.didCreate` (…`didRename`, …`didDelete`).
    //
    // ★ THIS IS THE PART THAT NEEDED NO PROTOCOL SURFACE. It is declared in the
    // `initialize` RESULT — a response to an inbound request — so it does not
    // need `client/registerCapability`, which is a server→client REQUEST this
    // server cannot originate (`JsonRpc` has serializeResponse / serializeError
    // / serializeNotification and no serializeRequest; `MethodDispatcher`
    // correlates INBOUND ids only). That distinction is the whole reason
    // D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE was believed blocked.
    //
    // ⚠ WHAT IT DOES *NOT* COVER, STATED: these three fire for file operations
    // the CLIENT performs (its explorer / its API) — not for a manifest written
    // by `git checkout` or a terminal, and not for a CONTENT edit to an
    // existing manifest. Those are covered by the refresh hung off `didOpen` /
    // `didSave`, which re-reads the filesystem regardless of who touched it.
    // The two channels overlap on purpose: this one is immediate, that one is
    // universal.
    //
    // The glob is DERIVED from `kProjectFileSuffix`, never re-spelled — one
    // place decides what a project manifest is called.
    {
        json filter;
        filter["scheme"]             = "file";
        filter["pattern"]["glob"]    =
            std::string{"**/*"} + std::string{kProjectFileSuffix};
        filter["pattern"]["matches"] = "file";
        const json filters = json::array({std::move(filter)});
        auto& fileOps = caps["workspace"]["fileOperations"];
        fileOps["didCreate"]["filters"] = filters;
        fileOps["didRename"]["filters"] = filters;
        fileOps["didDelete"]["filters"] = filters;
        // No `willCreate`/`willRename`/`willDelete`: those are REQUESTS the
        // client waits on and whose result is a `WorkspaceEdit`. We have no
        // edit to contribute, and claiming them would make the editor block on
        // a server that has nothing to say.
    }

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

LspServer::SchemaResolution
LspServer::resolveSchemaForUri_(std::string const& uri) {
    // ── Resolve schema by file extension, with the workspace as tie-breaker ──
    //
    // ★★ THE FAILURE ARM USED TO BE EMPTY. This block read
    //     `if (resolved.has_value()) schema = *resolved;`
    // and nothing else: a document whose language could not be determined was
    // opened with a null schema and NO diagnostic, and the parse worker then
    // published an empty diagnostics array — indistinguishable, in the editor,
    // from a file with no problems. Both halves of that (the missing tie-break
    // AND the missing diagnostic) are D-LSP-ASSEMBLY-DIALECT-UNSERVABLE.
    SchemaResolution out;
    const auto ext = extensionFromUri(uri);
    if (ext.empty()) {
        // Not a resolver failure — there is nothing to resolve. Still a REASON
        // the document has no language service, so it is still said out loud.
        out.reason = "no language service for `" + uri
            + "` — the document URI has no file extension, and the extension is "
              "what selects a source language";
        return out;
    }
    std::span<std::string const> preferred{};
    if (workspacePreference_.has_value()) {
        preferred = workspacePreference_->languages;
    }
    auto resolved = schemaCache_.resolveByExtension(ext, preferred);
    if (resolved.has_value()) {
        out.schema = *resolved;
    } else {
        out.reason = describeUnresolvedSchema(
            ext, resolved.error(), workspacePreference_);
    }
    return out;
}

bool LspServer::refreshWorkspacePreference_() {
    // Before `initialize` the held value is the deliberately-distinct "the
    // client has not sent `initialize` yet" reason. Recomputing would replace it
    // with "the client named no workspace folder", which is a different claim
    // and, at that point, an unfounded one.
    if (!initializeReceived_) return false;

    auto fresh = resolveWorkspaceLanguagePreference(workspaceRoots_);
    if (fresh == workspacePreference_) return false;
    workspacePreference_ = std::move(fresh);

    // ★ REPUBLISH, DON'T JUST REMEMBER. Every open document is re-resolved
    // under the new preference; the ones whose schema (or whose reason for
    // having none) actually moved are re-parsed, and the parse worker's
    // `publishDiagnostics_` puts the new answer on the wire. `setSchema`
    // returns false for a document that did not move, so an unrelated manifest
    // edit costs no republishes at all.
    for (auto const& uri : documents_.openUris()) {
        auto r = resolveSchemaForUri_(uri);
        if (documents_.setSchema(uri, std::move(r.schema), std::move(r.reason))) {
            enqueueParse_(uri);
        }
    }
    return true;
}

void LspServer::handleWorkspaceManifestsMayHaveChanged_(
    Notification const& /*n*/) {
    // Params ignored on purpose — see the note on the `Method::Workspace*`
    // enumerators. Whatever they name, the answer is re-derived by re-reading
    // the manifests, and a filter here could only make us MISS a change.
    //
    // ⚠ `workspace/didChangeWatchedFiles` is handled but NOT registered:
    // registering a watcher needs `client/registerCapability`, a server→client
    // REQUEST this protocol layer cannot originate
    // (D-LSP-NO-OUTBOUND-REQUEST-CHANNEL). Handling it anyway is strictly
    // better than the alternative, which is what happened before: the
    // dispatcher's unknown-notification path DROPPED it silently, so a client
    // that does send one — unprompted, or because a user configured a watcher
    // in client config — was ignored for no reason.
    (void)refreshWorkspacePreference_();
}

void LspServer::handleDidOpen_(Notification const& n) {
    auto params = tryParseParams(n);
    if (!params) return;
    const auto item = parseTextDocumentItem(*params);
    if (item.uri.empty()) return;

    // ★ LOOK BEFORE RESOLVING (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE).
    // The manifest set may have changed since `initialize`; this document is not
    // in the store yet, so the refresh's republish sweep cannot touch it and the
    // resolution below simply uses the up-to-date preference.
    (void)refreshWorkspacePreference_();

    auto r = resolveSchemaForUri_(item.uri);
    documents_.open(item.uri, item.version, item.text, std::move(r.schema),
                    std::move(r.reason));
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
    // The SAVED DOCUMENT needs nothing — `didChange` already re-parsed it on
    // every edit. What a save additionally means is that the FILESYSTEM moved,
    // and if what moved was a `*.dss-project.json` the workspace's dialect
    // preference moved with it.
    //
    // ★ THIS IS THE UNIVERSAL HALF OF THE LIVENESS
    // (D-LSP-WORKSPACE-PREFERENCE-FROZEN-AT-INITIALIZE). The
    // `workspace.fileOperations` registration advertised in `initialize` is
    // immediate but partial — it fires only for CLIENT-performed create /
    // rename / delete, never for a content edit and never for a file `git` or a
    // terminal wrote. Re-checking on any save (and on any open) covers all of
    // those, from any source, at the cost of a latency of one user action. It
    // needs no client capability, no registration, and no outbound request,
    // which is precisely why it is the load-bearing half.
    //
    // Deliberately NOT hung off `didChange`: that arrives per keystroke, and a
    // directory scan plus a manifest parse per keystroke is a cost the answer
    // does not justify (a manifest the user has not saved is not yet a fact).
    (void)refreshWorkspacePreference_();
}

void LspServer::enqueueParse_(std::string uri) {
    auto snap = documents_.snapshot(uri);
    if (!snap.has_value()) return;

    executor_->submit([this,
                       uri    = std::move(uri),
                       snap   = std::move(*snap)]() mutable {
        if (!snap.schema) {
            // No schema — clear any prior PARSE diagnostics. The document's
            // `schemaError` is NOT cleared here and is re-attached by
            // `publishDiagnostics_`, so this publish carries the reason rather
            // than an empty array (which reads as "clean" in every editor).
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
        // TF-C74: DELIBERATELY no `setTargetPredefinedMacros` — the LSP has no
        // active target (it also sets no `setActiveFormat`), so the effective
        // predefined-macro list is the LANGUAGE's alone, exactly as before this
        // cycle. An editor session is not a compile: picking a target here would
        // make `#ifdef __aarch64__` resolve differently in the editor than in
        // the build, which is worse than resolving neither arm. Trigger to
        // revisit: the day the LSP learns the workspace's active target.
        // TF-C97: likewise no `setFormatPredefinedMacros`. The argument is the
        // same one, and the format half is if anything stronger: with no active
        // format there is no `dataModel`, so guessing `__LP64__` would make the
        // editor take LP64 header arms in a workspace that might build LLP64.
        // Same trigger — both channels light up together the day the LSP learns
        // the workspace's `<target>:<format>` pair.
        //
        // ★ D-PP-HEADER-CASE-INSENSITIVE-PE (2026-08-04) — THIS ONE IS
        // USER-VISIBLE TODAY, unlike the two above, so it is stated rather than
        // inherited. With no active format the editor takes the conservative
        // POSIX rule, which means that on a pe64 project the LSP SQUIGGLES
        // `#include <Windows.h>` — the exact line this axis exists to make
        // build — while the compiler accepts it. That is a false positive in
        // the editor, not a wrong build, and it is the safe direction of the
        // two (the alternative, guessing case-insensitive, would hide a real
        // error on an elf workspace). It is written EXPLICITLY here rather than
        // taken from a default so the gap cannot be mistaken for an oversight.
        // Trigger to revisit: the same one as the two channels above — the day
        // the LSP learns the workspace's `<target>:<format>` pair, all three
        // light up together. Anchored as
        // `D-LSP-HEADER-CASE-RULE-NOT-WORKSPACE-AWARE`.
        dss::UnitBuilder builder{snap.schema, dss::DiagnosticBudget::libraryDefault()};
        builder.setHeaderNameMatching(dss::kDefaultHeaderNameMatching);
        builder.addInMemory(snap.text, uri);
        auto cu = std::make_shared<dss::CompilationUnit>(
            std::move(builder).finish());
        auto model = std::make_shared<dss::SemanticModel const>(
            dss::analyze(cu, dss::DiagnosticBudget::libraryDefault()));

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
    // ★ THE SCHEMA-LESS DOCUMENT SPEAKS. Emitted on EVERY publish, not just the
    // first: the reason lives on the document, so an edit that re-publishes an
    // (still) unresolvable file restates it instead of clearing it. Placed
    // FIRST so it is what the editor's problem panel shows at the top — it is
    // the reason the other diagnostics are absent.
    //
    // The code is `D_UnknownFileExtension`, REUSED rather than minted: it is
    // already the compiler's code for "this file's source language cannot be
    // determined", covering both the zero-claimant and the two-or-more-claimant
    // shapes, and it is what the driver emits when no `--language` was given
    // and the target's declared dialect cannot answer either. One code, both
    // halves of the program, so an editor and a build report the same identity
    // for the same condition.
    if (!snap->schemaError.empty()) {
        Diagnostic d;
        // Range 0:0–0:0. The condition is a property of the DOCUMENT, not of
        // any span inside it, and LSP has no document-level diagnostic channel.
        d.severity = DiagnosticSeverity::Error;
        d.code     = std::string{dss::diagnosticCodeName(
            dss::DiagnosticCode::D_UnknownFileExtension)};
        d.message  = snap->schemaError;
        params.diagnostics.insert(params.diagnostics.begin(), std::move(d));
    }
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
