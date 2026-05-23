// End-to-end: drive `LspServer::run()` on a background thread,
// push canned client messages through `InMemoryTransport`, and
// inspect what the server wrote back. Uses `SynchronousExecutor`
// so parse jobs land before the next client message — the test
// is deterministic without sleeps.

#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/thread_pool.hpp"
#include "lsp/transport.hpp"
#include "lsp_test_helpers.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using dss::lsp::IExecutor;
using dss::lsp::LspServer;
using dss::lsp::SchemaCache;
using dss::lsp::SynchronousExecutor;
using dss::lsp::testing::InMemoryTransport;

namespace {

[[nodiscard]] std::string makeInitialize(int id) {
    std::string s = R"({"jsonrpc":"2.0","id":)";
    s += std::to_string(id);
    s += R"(,"method":"initialize","params":{}})";
    return s;
}

[[nodiscard]] std::string makeDidOpen(std::string_view uri,
                                       int version,
                                       std::string_view text) {
    std::string s = R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")";
    s += uri;
    s += R"(","languageId":"toy","version":)";
    s += std::to_string(version);
    s += R"(,"text":")";
    s += text;
    s += R"("}}})";
    return s;
}

[[nodiscard]] std::string makeShutdown(int id) {
    std::string s = R"({"jsonrpc":"2.0","id":)";
    s += std::to_string(id);
    s += R"(,"method":"shutdown","params":null})";
    return s;
}

constexpr std::string_view kExit =
    R"({"jsonrpc":"2.0","method":"exit","params":null})";

} // namespace

TEST(LspServerE2E, InitializeRespondsWithUtf16PositionEncoding) {
    auto transport = std::make_unique<InMemoryTransport>();
    auto* tPtr = transport.get();
    SchemaCache cache;
    auto executor = std::make_unique<SynchronousExecutor>();
    LspServer server{std::move(transport), std::move(executor), cache};

    std::future<int> exitCode = std::async(std::launch::async, [&] {
        return server.run();
    });

    tPtr->pushClientMessage(makeInitialize(1));
    tPtr->pushClientMessage(makeShutdown(2));
    tPtr->pushClientMessage(std::string{kExit});

    ASSERT_EQ(exitCode.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(exitCode.get(), 0);

    auto msgs = tPtr->takeServerMessages();
    // Exactly two server messages: initialize response + shutdown ack.
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_NE(msgs[0].find(R"("id":1)"), std::string::npos);
    EXPECT_NE(msgs[0].find(R"("positionEncoding":"utf-16")"), std::string::npos);
    EXPECT_NE(msgs[0].find(R"("textDocumentSync":1)"), std::string::npos);
    EXPECT_NE(msgs[1].find(R"("id":2)"), std::string::npos);
    EXPECT_NE(msgs[1].find(R"("result":null)"), std::string::npos);
}

TEST(LspServerE2E, MalformedJsonReturnsParseErrorWithNullId) {
    auto transport = std::make_unique<InMemoryTransport>();
    auto* tPtr = transport.get();
    SchemaCache cache;
    auto executor = std::make_unique<SynchronousExecutor>();
    LspServer server{std::move(transport), std::move(executor), cache};

    std::future<int> exitCode = std::async(std::launch::async, [&] {
        return server.run();
    });

    tPtr->pushClientMessage(makeInitialize(1));
    tPtr->pushClientMessage("{not valid json");
    tPtr->pushClientMessage(makeShutdown(2));
    tPtr->pushClientMessage(std::string{kExit});

    ASSERT_EQ(exitCode.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(exitCode.get(), 0);

    auto msgs = tPtr->takeServerMessages();
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_NE(msgs[1].find(R"("code":-32700)"), std::string::npos);
    EXPECT_NE(msgs[1].find(R"("id":null)"), std::string::npos);
}

TEST(LspServerE2E, DidChangeRepublishesWithMonotonicVersions) {
    auto transport = std::make_unique<InMemoryTransport>();
    auto* tPtr = transport.get();
    SchemaCache cache;
    auto executor = std::make_unique<SynchronousExecutor>();
    LspServer server{std::move(transport), std::move(executor), cache};

    std::future<int> exitCode = std::async(std::launch::async, [&] {
        return server.run();
    });

    tPtr->pushClientMessage(makeInitialize(1));
    tPtr->pushClientMessage(makeDidOpen("file:///e.toy", 1, "1;"));
    tPtr->pushClientMessage(
        R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{)"
        R"("textDocument":{"uri":"file:///e.toy","version":2},)"
        R"("contentChanges":[{"text":"2;"}]}})");
    tPtr->pushClientMessage(makeShutdown(2));
    tPtr->pushClientMessage(std::string{kExit});

    ASSERT_EQ(exitCode.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(exitCode.get(), 0);

    auto msgs = tPtr->takeServerMessages();
    // Exact ordering: init-response, publishDiag(v1), publishDiag(v2), shutdown-ack.
    ASSERT_EQ(msgs.size(), 4u);
    EXPECT_NE(msgs[1].find(R"("version":1)"), std::string::npos);
    EXPECT_NE(msgs[2].find(R"("version":2)"), std::string::npos);
    // didChange's parse must run on the new text, not the old.
    EXPECT_NE(msgs[2].find(R"("uri":"file:///e.toy")"), std::string::npos);
}

TEST(LspServerE2E, DidOpenPublishesDiagnosticsForToySource) {
    auto transport = std::make_unique<InMemoryTransport>();
    auto* tPtr = transport.get();
    SchemaCache cache;
    auto executor = std::make_unique<SynchronousExecutor>();
    LspServer server{std::move(transport), std::move(executor), cache};

    std::future<int> exitCode = std::async(std::launch::async, [&] {
        return server.run();
    });

    tPtr->pushClientMessage(makeInitialize(1));
    // "1 + 2;" — toy grammar expects Identifier or VarKeyword to
    // start a statement, so `1` and `;` each emit one diagnostic.
    tPtr->pushClientMessage(makeDidOpen("file:///hello.toy", 1, "1 + 2;"));
    tPtr->pushClientMessage(makeShutdown(2));
    tPtr->pushClientMessage(std::string{kExit});

    ASSERT_EQ(exitCode.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(exitCode.get(), 0);

    auto msgs = tPtr->takeServerMessages();
    // Exact ordering: initialize-response, publishDiagnostics,
    // shutdown-ack. The synchronous executor guarantees the
    // diagnostics land before the next client message is read.
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_NE(msgs[0].find(R"("id":1)"), std::string::npos);
    EXPECT_NE(msgs[1].find(R"("method":"textDocument/publishDiagnostics")"),
              std::string::npos);
    EXPECT_NE(msgs[1].find(R"("uri":"file:///hello.toy")"), std::string::npos);
    EXPECT_NE(msgs[1].find(R"("version":1)"), std::string::npos);
    EXPECT_NE(msgs[1].find("P_NoAlternativeMatched"), std::string::npos);
    EXPECT_NE(msgs[2].find(R"("id":2)"), std::string::npos);
}

TEST(LspServerE2E, UnknownRequestReturnsMethodNotFound) {
    auto transport = std::make_unique<InMemoryTransport>();
    auto* tPtr = transport.get();
    SchemaCache cache;
    auto executor = std::make_unique<SynchronousExecutor>();
    LspServer server{std::move(transport), std::move(executor), cache};

    std::future<int> exitCode = std::async(std::launch::async, [&] {
        return server.run();
    });

    tPtr->pushClientMessage(makeInitialize(1));
    tPtr->pushClientMessage(
        R"({"jsonrpc":"2.0","id":99,"method":"made/up/method","params":{}})");
    tPtr->pushClientMessage(makeShutdown(2));
    tPtr->pushClientMessage(std::string{kExit});

    ASSERT_EQ(exitCode.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(exitCode.get(), 0);

    auto msgs = tPtr->takeServerMessages();
    bool sawMnf = false;
    for (auto const& m : msgs) {
        if (m.find(R"("id":99)") != std::string::npos &&
            m.find(R"("code":-32601)") != std::string::npos) {
            sawMnf = true;
            break;
        }
    }
    EXPECT_TRUE(sawMnf) << "unknown methods must produce -32601";
}
