// Coverage for `StdioTransport`. The close-aware contracts need no
// streams at all (the post-close write must return IoError per LSP
// §3.6's anti-post-exit-traffic rule; close is idempotent).
//
// ★★ THE SPLIT-CHUNK AND CONCURRENT-WRITE TESTS ARE NO LONGER DEFERRED.
// This file used to open by deferring them — "they require freopen or a
// transport refactor parameterizing over FILE*" — and the deferral was
// load-bearing in the worst way: those were EXACTLY the tests that would
// have caught `readMessage` blocking forever on any real client, because
// the replay and e2e suites drive an in-memory transport and never touch
// the real one. The refactor happened (`ByteSource` / `ByteSink` in
// `transport.hpp`), so they are written here.
//
// ⚠ WHY THE FAKE SOURCE ABORTS ON AN OVER-ASK INSTEAD OF BLOCKING. The
// defect is a read that asks a still-OPEN pipe for more bytes than the
// peer has sent, so the honest fake would BLOCK — and a blocking fake
// turns a red into a hung suite that reports nothing. `ScriptedSource`
// instead models the pipe's CONTRACT ("a request for N bytes does not
// return until N bytes exist") and FAILS LOUD the moment the transport
// asks for bytes the stream has not been given. A pin built the other
// way round — feeding a message LARGER than the old 4 KiB chunk — passes
// against the broken code, which is the vacuity this file must avoid.
//
// ⚠ AND `freopen`/`FILE*` — the remedy the old header proposed — WOULD
// NOT HAVE WORKED. A `tmpfile()` is SEEKABLE: `fread` over one returns
// short at end-of-file, so the old chunked read would have succeeded and
// the pin would have been green over the live defect. Reproducing the
// bug needs a stream that stays OPEN with fewer bytes than the ask, and
// that is a property of the SOURCE contract, not of `FILE*`.

#include "lsp/json_rpc.hpp"
#include "lsp/transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using dss::lsp::ByteSink;
using dss::lsp::ByteSource;
using dss::lsp::frameMessage;
using dss::lsp::StdioTransport;
using dss::lsp::TransportError;

namespace {

// A byte stream that hands out a fixed script and models a pipe that
// STAYS OPEN: it never reports end-of-stream unless the script says so.
//
// The `count` a caller passes is the assertion. Asking for more bytes
// than remain, while the stream is open, is precisely the shipped defect
// — so it is recorded and reported rather than served.
class ScriptedSource final : public ByteSource {
public:
    ScriptedSource(std::string script, bool closesAtEnd)
        : script_(std::move(script)), closesAtEnd_(closesAtEnd) {}

    [[nodiscard]] std::size_t read(char* destination, std::size_t count) override {
        if (count == 0) {
            overAsk_ = true;   // the contract says count >= 1
            return 0;
        }
        const std::size_t remaining = script_.size() - position_;
        if (count > remaining) {
            if (!closesAtEnd_) {
                // A real pipe would BLOCK here, forever. Record the
                // over-ask and serve nothing so the test can name it.
                overAsk_        = true;
                largestOverAsk_ = count;
                return 0;
            }
            // The script ends by closing the stream: a short read here
            // is the genuine end-of-stream a client's exit produces.
            std::memcpy(destination, script_.data() + position_, remaining);
            position_ = script_.size();
            ended_    = true;
            ++readCalls_;
            return remaining;
        }
        std::memcpy(destination, script_.data() + position_, count);
        position_ += count;
        ++readCalls_;
        largestRead_ = std::max(largestRead_, count);
        return count;
    }

    [[nodiscard]] bool atEnd() const noexcept override { return ended_; }

    [[nodiscard]] bool sawOverAsk() const noexcept { return overAsk_; }
    [[nodiscard]] std::size_t largestOverAsk() const noexcept { return largestOverAsk_; }
    [[nodiscard]] std::size_t readCalls() const noexcept { return readCalls_; }
    [[nodiscard]] std::size_t largestRead() const noexcept { return largestRead_; }
    [[nodiscard]] std::size_t consumed() const noexcept { return position_; }

private:
    std::string  script_;
    bool         closesAtEnd_    = true;
    std::size_t  position_       = 0;
    bool         ended_          = false;
    bool         overAsk_        = false;
    std::size_t  largestOverAsk_ = 0;
    std::size_t  readCalls_      = 0;
    std::size_t  largestRead_    = 0;
};

// A source that reports a hard I/O failure — short read, stream NOT
// ended. The transport must call that `IoError`, never a clean `Eof`:
// treating a failed pipe as a clean shutdown is the silent-wrong-answer
// direction, and `LspServer::run` uses exactly this distinction to pick
// its exit code.
class FailingSource final : public ByteSource {
public:
    [[nodiscard]] std::size_t read(char*, std::size_t) override { return 0; }
    [[nodiscard]] bool atEnd() const noexcept override { return false; }
};

// Records every framed write, and how many writers were inside `write`
// at once — the concurrency claim `LspTransport` makes in its header.
class RecordingSink final : public ByteSink {
public:
    [[nodiscard]] bool write(std::string_view bytes) override {
        const auto now = ++inFlight_;
        std::size_t previous = peakInFlight_.load();
        while (previous < now && !peakInFlight_.compare_exchange_weak(previous, now)) {
        }
        // Widen the window a real interleaving would need.
        std::this_thread::yield();
        {
            std::lock_guard lk{mutex_};
            writes_.emplace_back(bytes);
        }
        --inFlight_;
        return true;
    }

    [[nodiscard]] std::vector<std::string> writes() const {
        std::lock_guard lk{mutex_};
        return writes_;
    }
    [[nodiscard]] std::size_t peakInFlight() const noexcept {
        return peakInFlight_.load();
    }

private:
    mutable std::mutex       mutex_;
    std::vector<std::string> writes_;
    std::atomic<std::size_t> inFlight_{0};
    std::atomic<std::size_t> peakInFlight_{0};
};

class FailingSink final : public ByteSink {
public:
    [[nodiscard]] bool write(std::string_view) override { return false; }
};

// Build a transport over a script, keeping the source observable.
struct Rig {
    ScriptedSource* source = nullptr;
    RecordingSink*  sink   = nullptr;
    std::unique_ptr<StdioTransport> transport;
};

[[nodiscard]] Rig makeRig(std::string script, bool closesAtEnd = false) {
    auto  source = std::make_unique<ScriptedSource>(std::move(script), closesAtEnd);
    auto  sink   = std::make_unique<RecordingSink>();
    Rig   rig;
    rig.source = source.get();
    rig.sink   = sink.get();
    rig.transport =
        std::make_unique<StdioTransport>(std::move(source), std::move(sink));
    return rig;
}

// Assert one message came back AND that the source was never asked for
// bytes it did not have. The second half is the red-on-disable clause:
// the old chunked read reddens here even when the body matches.
void expectMessage(Rig& rig, std::string_view expected) {
    auto got = rig.transport->readMessage();
    ASSERT_FALSE(rig.source->sawOverAsk())
        << "the transport asked a still-open stream for "
        << rig.source->largestOverAsk()
        << " bytes it had not been sent — a real pipe would block here forever";
    ASSERT_TRUE(got.has_value())
        << "readMessage failed with error "
        << static_cast<int>(got.error());
    EXPECT_EQ(*got, expected);
}

} // namespace

// ── close-aware contracts (no streams involved) ────────────────────────

TEST(StdioTransport, WriteMessageReturnsIoErrorAfterClose) {
    auto rig = makeRig("");
    rig.transport->close();
    auto r = rig.transport->writeMessage("{\"jsonrpc\":\"2.0\"}");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError);
    EXPECT_TRUE(rig.sink->writes().empty())
        << "a post-close write reached the stream — LSP §3.6 forbids traffic after exit";
}

TEST(StdioTransport, CloseIsIdempotent) {
    auto rig = makeRig("");
    rig.transport->close();
    rig.transport->close(); // must not crash
    auto r = rig.transport->writeMessage("ignored");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError);
}

TEST(StdioTransport, ReadMessageReturnsEofAfterClose) {
    auto rig = makeRig(frameMessage(R"({"x":1})"));
    rig.transport->close();
    auto r = rig.transport->readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::Eof);
    EXPECT_EQ(rig.source->readCalls(), 0u)
        << "a closed transport still touched the stream";
}

// ── the reads that were deferred ───────────────────────────────────────

// THE DEFECT ITSELF. One small message on a stream that stays open —
// what every real client does on its very first `initialize`. The
// transport must answer it without another byte arriving.
TEST(StdioTransport, ReadsASmallMessageFromAStreamThatStaysOpen) {
    const auto framed = frameMessage(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
    ASSERT_LT(framed.size(), 4096u)
        << "the pin must be SMALLER than the old chunk size or it passes "
           "against the defect it exists to catch";
    auto rig = makeRig(framed);
    expectMessage(rig, R"({"jsonrpc":"2.0","id":1,"method":"initialize"})");
    EXPECT_EQ(rig.source->consumed(), framed.size())
        << "the transport left bytes of its own frame unread";
}

// A frame arriving ONE BYTE AT A TIME. The scripted source serves at
// most what it has, so this also proves the transport never asks for
// more than the framing has promised.
TEST(StdioTransport, ReassemblesAFrameDeliveredOneByteAtATime) {
    const auto framed = frameMessage(R"({"m":"didOpen"})");
    auto rig = makeRig(framed);
    expectMessage(rig, R"({"m":"didOpen"})");
    // Header bytes are read singly (the length is unknowable until the
    // separator lands); the body is then taken in ONE exact read.
    EXPECT_GE(rig.source->readCalls(), 2u);
    EXPECT_EQ(rig.source->largestRead(), std::string_view{R"({"m":"didOpen"})"}.size())
        << "the body read was not sized to the frame";
}

// TWO frames in one delivery. The second must survive in the buffer and
// come back from the next call without any further bytes arriving.
TEST(StdioTransport, DeliversTwoFramesArrivingTogether) {
    auto rig = makeRig(frameMessage(R"({"a":1})") + frameMessage(R"({"b":2})"));
    expectMessage(rig, R"({"a":1})");
    expectMessage(rig, R"({"b":2})");
}

// Split ACROSS the header/body boundary: the header is complete, the
// body is not. The exact-remainder read is what makes this terminate.
TEST(StdioTransport, ReadsAFrameSplitAcrossTheHeaderBodyBoundary) {
    const std::string body = R"({"method":"textDocument/didChange"})";
    auto rig = makeRig(frameMessage(body));
    expectMessage(rig, body);
}

// A body far LARGER than the old 4 KiB chunk still round-trips — the fix
// must not have traded one blocking size for another.
TEST(StdioTransport, ReadsABodyLargerThanTheOldChunkSize) {
    const std::string body =
        std::string{R"({"text":")"} + std::string(20000, 'x') + R"("})";
    ASSERT_GT(body.size(), 4096u);
    auto rig = makeRig(frameMessage(body));
    expectMessage(rig, body);
}

// Interleaved: read a message, then the NEXT one, from a stream whose
// remaining bytes are always fewer than a chunk. This is the shape of a
// real editing session.
TEST(StdioTransport, ReadsASequenceOfSmallMessages) {
    std::string script;
    for (int i = 0; i < 8; ++i) {
        script += frameMessage(R"({"n":)" + std::to_string(i) + "}");
    }
    auto rig = makeRig(script);
    for (int i = 0; i < 8; ++i) {
        expectMessage(rig, R"({"n":)" + std::to_string(i) + "}");
    }
}

// `\n\n` separator — some clients emit it, and the framing accepts it.
// It must survive the byte-at-a-time header phase too.
TEST(StdioTransport, ReadsAFrameWithAnLfOnlySeparator) {
    auto rig = makeRig("Content-Length: 7\n\n{\"x\":1}");
    expectMessage(rig, R"({"x":1})");
}

// ── stream end and failure ─────────────────────────────────────────────

TEST(StdioTransport, ReturnsEofWhenTheStreamClosesBetweenMessages) {
    auto rig = makeRig(frameMessage(R"({"x":1})"), /*closesAtEnd=*/true);
    expectMessage(rig, R"({"x":1})");
    auto second = rig.transport->readMessage();
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error(), TransportError::Eof);
}

TEST(StdioTransport, ReturnsEofWhenTheStreamClosesMidFrame) {
    // Header promises 40 bytes; the client dies after 4 of them.
    auto rig = makeRig("Content-Length: 40\r\n\r\n{\"x\"", /*closesAtEnd=*/true);
    auto r = rig.transport->readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::Eof)
        << "a truncated frame must not be delivered as a message";
}

TEST(StdioTransport, ReturnsIoErrorWhenTheSourceFailsWithoutEnding) {
    StdioTransport t{std::make_unique<FailingSource>(),
                     std::make_unique<RecordingSink>()};
    auto r = t.readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError)
        << "a failed stream reported as clean Eof would make the server exit 0";
}

// A peer declaring a body it will never send must not make the server
// allocate that much up front. The transport reads in steps, so the
// largest ask is bounded even when `Content-Length` is absurd.
//
// ⓘ 256 MiB rather than a truly absurd figure: the number has to be one
// the machine can still survive allocating when the step cap is REMOVED,
// or the red-on-disable arm for this pin becomes a crash instead of a
// failure. 256 × the cap is decisive without being hostile.
TEST(StdioTransport, DoesNotAllocateAnAbsurdContentLengthUpFront) {
    auto rig = makeRig("Content-Length: 268435456\r\n\r\n{}");
    auto r = rig.transport->readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError);
    EXPECT_TRUE(rig.source->sawOverAsk());
    EXPECT_LE(rig.source->largestOverAsk(), dss::lsp::kMaxReadStepBytes)
        << "the transport tried to size a read to a body no peer has sent";
}

TEST(StdioTransport, ReturnsFramingErrorOnAHeaderWithoutContentLength) {
    auto rig = makeRig("Content-Type: application/json\r\n\r\n{}");
    auto r = rig.transport->readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::FramingError);
}

// An unterminated header must not buffer without bound. Before the cap
// this loop had no exit at all: a peer that never sends `\r\n\r\n` grew
// the buffer until the process died, with no error anyone could report.
TEST(StdioTransport, ReturnsFramingErrorOnAnUnterminatedHeader) {
    auto rig = makeRig(std::string(dss::lsp::kMaxFrameHeaderBytes + 64, 'A'));
    auto r = rig.transport->readMessage();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::FramingError);
    EXPECT_LE(rig.source->consumed(), dss::lsp::kMaxFrameHeaderBytes + 1)
        << "the transport read past the header cap before refusing";
}

// ── writes ─────────────────────────────────────────────────────────────

TEST(StdioTransport, WriteMessageFramesTheBody) {
    auto rig = makeRig("");
    ASSERT_TRUE(rig.transport->writeMessage(R"({"x":1})").has_value());
    auto const written = rig.sink->writes();
    ASSERT_EQ(written.size(), 1u);
    EXPECT_EQ(written[0], "Content-Length: 7\r\n\r\n{\"x\":1}");
}

TEST(StdioTransport, WriteMessageReturnsIoErrorWhenTheSinkFails) {
    StdioTransport t{std::make_unique<ScriptedSource>("", false),
                     std::make_unique<FailingSink>()};
    auto r = t.writeMessage(R"({"x":1})");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error(), TransportError::IoError);
}

// The other deferred test: `writeMessage` must be internally
// synchronized, because worker threads publish diagnostics while the
// run loop answers requests. Every frame must arrive WHOLE and exactly
// once, and no two writers may be inside the sink at the same moment.
TEST(StdioTransport, ConcurrentWritesAreSerializedAndNeverInterleave) {
    auto rig = makeRig("");
    constexpr int kThreads  = 8;
    constexpr int kPerThread = 32;

    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&rig, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto body = R"({"t":)" + std::to_string(t) + R"(,"i":)"
                                  + std::to_string(i) + "}";
                EXPECT_TRUE(rig.transport->writeMessage(body).has_value());
            }
        });
    }
    for (auto& w : writers) w.join();

    auto const written = rig.sink->writes();
    ASSERT_EQ(written.size(), static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(rig.sink->peakInFlight(), 1u)
        << "two writers were inside the sink at once — the transport's "
           "own header promises internal synchronization";
    // Every recorded write is one whole, well-formed frame.
    for (auto const& frame : written) {
        std::string body;
        EXPECT_EQ(dss::lsp::tryParseFramedMessage(frame, body),
                  static_cast<std::int64_t>(frame.size()))
            << "a write reached the stream as something other than one frame";
    }
}
