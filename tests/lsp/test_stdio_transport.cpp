// Coverage for `StdioTransport`'s observable contracts that don't
// require redirecting real stdin/stdout: close-aware writeMessage
// (the post-close write must return IoError per LSP §3.6 anti-
// post-exit-traffic rule) and idempotent close.
//
// Tests that DO require stdin/stdout redirection (split-chunk
// reads, concurrent writes) are deferred — they require freopen
// or a transport refactor parameterizing over FILE*.

#include "lsp/transport.hpp"

#include <gtest/gtest.h>

using dss::lsp::StdioTransport;
using dss::lsp::TransportError;

TEST(StdioTransport, WriteMessageReturnsIoErrorAfterClose) {
    StdioTransport t;
    t.close();
    auto r = t.writeMessage("{\"jsonrpc\":\"2.0\"}");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError);
}

TEST(StdioTransport, CloseIsIdempotent) {
    StdioTransport t;
    t.close();
    t.close(); // must not crash
    auto r = t.writeMessage("ignored");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError);
}

TEST(StdioTransport, ReadMessageReturnsEofAfterClose) {
    StdioTransport t;
    t.close();
    auto r = t.readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::Eof);
}
