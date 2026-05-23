// Pins for `lsp::parseMethod` / `lsp::methodName` round-trip. The
// protocol value types (Position, Range, Diagnostic) are aggregate
// structs without behavior; their JSON serialization belongs to the
// translator + json_rpc layers and is exercised by their own tests.

#include "lsp/protocol.hpp"

#include <gtest/gtest.h>

#include <string_view>

using dss::lsp::Method;
using dss::lsp::methodName;
using dss::lsp::parseMethod;

TEST(LspProtocol, ParseMethodKnownStrings) {
    EXPECT_EQ(parseMethod("initialize"),              Method::Initialize);
    EXPECT_EQ(parseMethod("initialized"),             Method::Initialized);
    EXPECT_EQ(parseMethod("shutdown"),                Method::Shutdown);
    EXPECT_EQ(parseMethod("exit"),                    Method::Exit);
    EXPECT_EQ(parseMethod("textDocument/didOpen"),    Method::TextDocumentDidOpen);
    EXPECT_EQ(parseMethod("textDocument/didChange"),  Method::TextDocumentDidChange);
    EXPECT_EQ(parseMethod("textDocument/didClose"),   Method::TextDocumentDidClose);
    EXPECT_EQ(parseMethod("textDocument/didSave"),    Method::TextDocumentDidSave);
}

TEST(LspProtocol, ParseMethodUnknownStringMapsToUnknown) {
    EXPECT_EQ(parseMethod("textDocument/hover"),       Method::Unknown);
    EXPECT_EQ(parseMethod(""),                          Method::Unknown);
    EXPECT_EQ(parseMethod("initialize\n"),              Method::Unknown);
    EXPECT_EQ(parseMethod("INITIALIZE"),                Method::Unknown);
}

TEST(LspProtocol, MethodNameRoundTripsForKnownMethods) {
    constexpr Method known[] = {
        Method::Initialize,
        Method::Initialized,
        Method::Shutdown,
        Method::Exit,
        Method::TextDocumentDidOpen,
        Method::TextDocumentDidChange,
        Method::TextDocumentDidClose,
        Method::TextDocumentDidSave,
    };
    for (auto m : known) {
        const auto name = methodName(m);
        EXPECT_FALSE(name.empty()) << "method " << static_cast<int>(m)
                                     << " missing wire-protocol name";
        EXPECT_EQ(parseMethod(name), m);
    }
}

TEST(LspProtocol, MethodNameForUnknownIsEmpty) {
    EXPECT_TRUE(methodName(Method::Unknown).empty());
}
