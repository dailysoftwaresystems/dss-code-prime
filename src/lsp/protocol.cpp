#include "lsp/protocol.hpp"

#include <string_view>

namespace dss::lsp {

namespace {

// Wire-protocol method name → enum. Linear scan over a constexpr
// table: 8 entries, branch-predictable, zero allocations. Kept in
// sync with `methodName()` below — adding a method requires both.
struct MethodEntry {
    std::string_view name;
    Method           id;
};

constexpr MethodEntry kMethodTable[] = {
    {"initialize",                  Method::Initialize},
    {"initialized",                 Method::Initialized},
    {"shutdown",                    Method::Shutdown},
    {"exit",                        Method::Exit},
    {"textDocument/didOpen",        Method::TextDocumentDidOpen},
    {"textDocument/didChange",      Method::TextDocumentDidChange},
    {"textDocument/didClose",       Method::TextDocumentDidClose},
    {"textDocument/didSave",        Method::TextDocumentDidSave},
};

} // namespace

Method parseMethod(std::string_view raw) noexcept {
    for (auto const& e : kMethodTable) {
        if (e.name == raw) return e.id;
    }
    return Method::Unknown;
}

std::string_view methodName(Method m) noexcept {
    for (auto const& e : kMethodTable) {
        if (e.id == m) return e.name;
    }
    return {};
}

} // namespace dss::lsp
