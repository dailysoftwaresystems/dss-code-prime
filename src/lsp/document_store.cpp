#include "lsp/document_store.hpp"

#include <utility>

namespace dss::lsp {

void DocumentStore::open(std::string uri,
                         std::int32_t clientVersion,
                         std::string text,
                         std::shared_ptr<dss::GrammarSchema const> schema,
                         std::string schemaError) {
    std::lock_guard lk{mutex_};
    auto& entry = docs_[std::move(uri)];
    entry.clientVersion   = clientVersion;
    entry.parseGeneration = 0;
    entry.text            = std::move(text);
    entry.schema          = std::move(schema);
    entry.schemaError     = std::move(schemaError);
    // ★ THE SILENT STATE IS MADE UNREPRESENTABLE AT THE BOUNDARY.
    // `schema == nullptr` with no reason is exactly what
    // D-LSP-ASSEMBLY-DIALECT-UNSERVABLE removed: the publish path then emits an
    // empty diagnostics array, which an editor renders as "this file is clean".
    // The one production caller supplies a reason for every arm, so this
    // substitution does not fire today — but that is a property of ONE call
    // site, not of the type, and this field is exactly the kind a future arm
    // forgets. (Contrast the deliberately-absent `ProjectDeclaresNoTarget` in
    // `workspace_project.hpp`: that one could never fire, because a permanent
    // upstream validator refuses the state outright. This one can, from any new
    // caller.) Exercised by
    // `DocumentStore.SchemalessOpenWithoutAReasonSubstitutesALoudOne`.
    if (!entry.schema && entry.schemaError.empty()) {
        entry.schemaError =
            "no language service for this document, and the server did not "
            "record why — that omission is a defect in the LSP's schema "
            "resolution path, not a property of the file";
    }
    entry.diagnostics.clear();
}

std::optional<std::uint32_t>
DocumentStore::update(std::string const& uri, std::int32_t clientVersion, std::string text) {
    std::lock_guard lk{mutex_};
    auto it = docs_.find(uri);
    if (it == docs_.end()) return std::nullopt;
    it->second.clientVersion = clientVersion;
    it->second.text          = std::move(text);
    ++it->second.parseGeneration;
    return it->second.parseGeneration;
}

void DocumentStore::close(std::string const& uri) {
    std::lock_guard lk{mutex_};
    docs_.erase(uri);
}

std::optional<DocumentSnapshot>
DocumentStore::snapshot(std::string const& uri) const {
    std::lock_guard lk{mutex_};
    auto it = docs_.find(uri);
    if (it == docs_.end()) return std::nullopt;
    return DocumentSnapshot{
        .uri             = uri,
        .clientVersion   = it->second.clientVersion,
        .parseGeneration = it->second.parseGeneration,
        .text            = it->second.text,
        .schema          = it->second.schema,
        .schemaError     = it->second.schemaError,
    };
}

bool DocumentStore::setDiagnostics(std::string const& uri,
                                    std::uint32_t expectedGen,
                                    std::vector<dss::ParseDiagnostic> diags) {
    std::lock_guard lk{mutex_};
    auto it = docs_.find(uri);
    if (it == docs_.end()) return false;
    if (it->second.parseGeneration != expectedGen) return false;
    it->second.diagnostics = std::move(diags);
    return true;
}

std::vector<dss::ParseDiagnostic>
DocumentStore::diagnosticsFor(std::string const& uri) const {
    std::lock_guard lk{mutex_};
    auto it = docs_.find(uri);
    if (it == docs_.end()) return {};
    return it->second.diagnostics;
}

bool DocumentStore::setSemanticModel(
    std::string const& uri,
    std::uint32_t expectedGen,
    std::shared_ptr<dss::SemanticModel const> model) {
    std::lock_guard lk{mutex_};
    auto it = docs_.find(uri);
    if (it == docs_.end()) return false;
    if (it->second.parseGeneration != expectedGen) return false;
    it->second.semanticModel      = std::move(model);
    it->second.semanticGeneration = expectedGen;
    return true;
}

std::shared_ptr<dss::SemanticModel const>
DocumentStore::semanticModelFor(std::string const& uri) const {
    std::lock_guard lk{mutex_};
    auto it = docs_.find(uri);
    if (it == docs_.end()) return nullptr;
    return it->second.semanticModel;
}

} // namespace dss::lsp
