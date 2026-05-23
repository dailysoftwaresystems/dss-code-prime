// MethodDispatcher: request handlers, notification handlers, and
// the LSP §3.1 fallbacks (unknown request → -32601, unknown
// notification → silent drop).

#include "lsp/method_dispatcher.hpp"
#include "lsp/protocol.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <variant>

using json = nlohmann::json;

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
    auto reply = json::parse(*out);
    EXPECT_EQ(reply.at("jsonrpc"), "2.0");
    EXPECT_EQ(reply.at("id"), 42);
    EXPECT_EQ(reply.at("result"), json::parse(R"({"ok":true})"));
    EXPECT_FALSE(reply.contains("error"));
}

TEST(MethodDispatcher, UnknownRequestReturnsMethodNotFound) {
    MethodDispatcher d;
    Request req{Method::Unknown, LspId{std::int64_t{7}}, "{}"};
    auto out = d.dispatch(IncomingMessage{req});
    ASSERT_TRUE(out.has_value());
    auto reply = json::parse(*out);
    EXPECT_EQ(reply.at("id"), 7);
    EXPECT_EQ(reply.at("error").at("code"), -32601);
}

TEST(MethodDispatcher, UnregisteredRequestReturnsMethodNotFound) {
    MethodDispatcher d;
    Request req{Method::Initialize, LspId{std::int64_t{1}}, "{}"};
    auto out = d.dispatch(IncomingMessage{req});
    ASSERT_TRUE(out.has_value());
    auto reply = json::parse(*out);
    EXPECT_EQ(reply.at("id"), 1);
    EXPECT_EQ(reply.at("error").at("code"), -32601);
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

TEST(MethodDispatcherDeathTest, DuplicateRequestRegistrationAborts) {
    EXPECT_DEATH({
        MethodDispatcher d;
        d.registerRequest(Method::Shutdown,
            [](Request const&) { return std::optional<std::string>{"null"}; });
        d.registerRequest(Method::Shutdown,
            [](Request const&) { return std::optional<std::string>{"null"}; });
    }, "duplicate request handler registration");
}

TEST(MethodDispatcherDeathTest, DuplicateNotificationRegistrationAborts) {
    EXPECT_DEATH({
        MethodDispatcher d;
        d.registerNotification(Method::Exit, [](Notification const&) {});
        d.registerNotification(Method::Exit, [](Notification const&) {});
    }, "duplicate notification handler registration");
}
