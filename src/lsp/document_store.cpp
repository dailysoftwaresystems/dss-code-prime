#include "lsp/document_store.hpp"

#include <utility>

namespace dss::lsp {

void DocumentStore::open(std::string uri,
                         std::int32_t clientVersion,
                         std::string text,
                         std::shared_ptr<dss::GrammarSchema const> schema) {
    std::lock_guard lk{mutex_};
    auto& entry = docs_[std::move(uri)];
    entry.clientVersion   = clientVersion;
    entry.parseGeneration = 0;
    entry.text            = std::move(text);
    entry.schema          = std::move(schema);
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

} // namespace dss::lsp
