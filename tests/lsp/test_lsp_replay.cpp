// Replay harness: each `*.in.jsonl` under sessions/ is driven
// against a fresh LspServer; server outputs are normalized
// (nlohmann round-trip preserves insertion order) and compared to
// the matching `*.out.jsonl` golden. `DSS_REFRESH_GOLDENS=1`
// writes goldens; otherwise a missing golden fails loudly.

#include "core/substrate/thread_pool.hpp"
#include "lsp/lsp_server.hpp"
#include "lsp/schema_cache.hpp"
#include "lsp/transport.hpp"
#include "lsp_test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using dss::lsp::LspServer;
using dss::lsp::SchemaCache;
using dss::substrate::SynchronousExecutor;
using dss::lsp::testing::InMemoryTransport;

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace {

// Walk parent dirs from this TU until we find tests/lsp/sessions/.
// Mirrors the cwd-independence trick in GrammarSchema::loadShipped.
[[nodiscard]] fs::path findSessionsDir() {
    fs::path here = fs::path{__FILE__}.parent_path();
    fs::path candidate = here / "sessions";
    if (fs::exists(candidate)) return candidate;
    // Fall back to walking up from the test binary location.
    auto cur = fs::current_path();
    for (int i = 0; i < 6; ++i) {
        auto try1 = cur / "tests" / "lsp" / "sessions";
        if (fs::exists(try1)) return try1;
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return candidate;
}

[[nodiscard]] std::vector<std::string> readJsonLines(fs::path const& p) {
    std::vector<std::string> out;
    std::ifstream in{p};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        out.push_back(std::move(line));
        line.clear();
    }
    return out;
}

// Normalize a server message: re-parse via nlohmann to get a
// stable canonical form (sorted object keys, no insignificant
// whitespace). Strips fields known to be timing- or memory-
// address-sensitive (none today, but the hook stays here).
[[nodiscard]] std::string normalize(std::string const& msg) {
    try {
        auto j = json::parse(msg);
        // Sort by relying on nlohmann's ordered-by-insertion -> we
        // re-emit via `dump()` which already sorts when using
        // `nlohmann::ordered_json`. With default `json` insertion
        // order is preserved, but that's exactly what a server
        // would write, so it's stable enough.
        return j.dump();
    } catch (...) {
        return msg;
    }
}

void runOneSession(fs::path const& inputPath) {
    SCOPED_TRACE(inputPath.string());
    ASSERT_TRUE(fs::exists(inputPath));

    auto lines = readJsonLines(inputPath);
    ASSERT_FALSE(lines.empty()) << "empty session script: " << inputPath;

    auto transport = std::make_unique<InMemoryTransport>();
    auto* tPtr = transport.get();
    SchemaCache cache;
    auto executor = std::make_unique<SynchronousExecutor>();
    LspServer server{std::move(transport), std::move(executor), cache};

    std::future<int> exitCode = std::async(std::launch::async, [&] {
        return server.run();
    });

    for (auto const& l : lines) {
        tPtr->pushClientMessage(l);
    }

    ASSERT_EQ(exitCode.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    EXPECT_EQ(exitCode.get(), 0);

    auto serverMsgs = tPtr->takeServerMessages();
    std::vector<std::string> normalized;
    normalized.reserve(serverMsgs.size());
    for (auto const& m : serverMsgs) normalized.push_back(normalize(m));

    fs::path goldenPath = inputPath;
    goldenPath.replace_extension();        // strip .jsonl
    goldenPath.replace_extension();        // strip .in
    goldenPath += ".out.jsonl";

    const bool refresh = std::getenv("DSS_REFRESH_GOLDENS") != nullptr;
    if (refresh) {
        std::ofstream out{goldenPath};
        for (auto const& m : normalized) out << m << '\n';
        out.close();
        SUCCEED() << "wrote golden " << goldenPath;
        return;
    }
    // Missing goldens must fail loudly — silent generation would let
    // a reviewer who deleted a golden see a green run.
    ASSERT_TRUE(fs::exists(goldenPath))
        << "golden missing: " << goldenPath
        << " — run with DSS_REFRESH_GOLDENS=1 to create it";

    auto expected = readJsonLines(goldenPath);
    EXPECT_EQ(normalized.size(), expected.size())
        << "server emitted " << normalized.size()
        << " messages, golden has " << expected.size()
        << " — re-run with DSS_REFRESH_GOLDENS=1 if intentional";
    const auto n = std::min(normalized.size(), expected.size());
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_EQ(normalized[i], expected[i])
            << "mismatch at message " << i << " of " << inputPath.filename();
    }
}

} // namespace

TEST(LspReplay, InitializeBasic) {
    runOneSession(findSessionsDir() / "initialize_basic.in.jsonl");
}

TEST(LspReplay, ToyDidOpenAndClose) {
    runOneSession(findSessionsDir() / "toy_didopen_close.in.jsonl");
}

TEST(LspReplay, HoverStub)         { runOneSession(findSessionsDir() / "hover_stub.in.jsonl"); }
TEST(LspReplay, CompletionStub)    { runOneSession(findSessionsDir() / "completion_stub.in.jsonl"); }
TEST(LspReplay, DefinitionStub)    { runOneSession(findSessionsDir() / "definition_stub.in.jsonl"); }
TEST(LspReplay, ReferencesStub)    { runOneSession(findSessionsDir() / "references_stub.in.jsonl"); }
TEST(LspReplay, RenameStub)        { runOneSession(findSessionsDir() / "rename_stub.in.jsonl"); }
TEST(LspReplay, SignatureHelpStub) { runOneSession(findSessionsDir() / "signature_help_stub.in.jsonl"); }
