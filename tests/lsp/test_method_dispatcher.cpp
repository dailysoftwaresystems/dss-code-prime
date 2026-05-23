// MethodDispatcher: request handlers, notification handlers, and
// the LSP §3.1 fallbacks (unknown request → -32601, unknown
// notification → silent drop).

#include "lsp/method_dispatcher.hpp"
#include "lsp/protocol.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <variant>

using dss::lsp::IncomingMessage;
using dss::lsp::LspId;
using dss::lsp::Method;
using dss::lsp::MethodDispatcher;
using dss::lsp::Notification;
using dss::lsp::Request;

TEST(MethodDispatcher, DispatchesRegisteredRequest) {
    MethodDispatcher d;
    int hits = 0;
    d.registerRequest(Method::Initialize, [&](Request const&) {
        ++hits;
        return std::optional<std::string>{R"({"ok":true})"};
    });
    Request req{Method::Initialize, LspId{std::int64_t{42}}, "{}"};
    auto out = d.dispatch(IncomingMessage{req});
    EXPECT_EQ(hits, 1);
    ASSERT_TRUE(out.has_value());
    // Response must include the id and the handler's result body.
    EXPECT_NE(out->find("\"id\":42"), std::string::npos);
    EXPECT_NE(out->find(R"("result":{"ok":true})"), std::string::npos);
}

TEST(MethodDispatcher, UnknownRequestReturnsMethodNotFound) {
    MethodDispatcher d;
    Request req{Method::Unknown, LspId{std::int64_t{7}}, "{}"};
    auto out = d.dispatch(IncomingMessage{req});
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(out->find("\"code\":-32601"), std::string::npos);
    EXPECT_NE(out->find("\"id\":7"), std::string::npos);
}

TEST(MethodDispatcher, UnregisteredRequestReturnsMethodNotFound) {
    MethodDispatcher d;
    // Initialize is known to the enum but has no handler.
    Request req{Method::Initialize, LspId{std::int64_t{1}}, "{}"};
    auto out = d.dispatch(IncomingMessage{req});
    ASSERT_TRUE(out.has_value());
    EXPECT_NE(out->find("\"code\":-32601"), std::string::npos);
}

TEST(MethodDispatcher, DispatchesRegisteredNotification) {
    MethodDispatcher d;
    int hits = 0;
    d.registerNotification(Method::TextDocumentDidOpen,
                            [&](Notification const&) { ++hits; });
    Notification n{Method::TextDocumentDidOpen, "{}"};
    auto out = d.dispatch(IncomingMessage{n});
    EXPECT_EQ(hits, 1);
    EXPECT_FALSE(out.has_value());
}

TEST(MethodDispatcher, UnknownNotificationSilentlyDropped) {
    MethodDispatcher d;
    Notification n{Method::Unknown, "{}"};
    auto out = d.dispatch(IncomingMessage{n});
    EXPECT_FALSE(out.has_value());
}

TEST(MethodDispatcher, ReplacesPriorHandler) {
    MethodDispatcher d;
    int first = 0, second = 0;
    d.registerRequest(Method::Shutdown, [&](Request const&) {
        ++first;
        return std::optional<std::string>{"null"};
    });
    d.registerRequest(Method::Shutdown, [&](Request const&) {
        ++second;
        return std::optional<std::string>{"null"};
    });
    Request req{Method::Shutdown, LspId{std::int64_t{1}}, "{}"};
    (void)d.dispatch(IncomingMessage{req});
    EXPECT_EQ(first, 0);
    EXPECT_EQ(second, 1);
}
