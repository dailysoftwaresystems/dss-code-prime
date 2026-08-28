#include "lsp/transport.hpp"

#include "lsp/json_rpc.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

namespace dss::lsp {

namespace {

[[noreturn]] void transportFatal(char const* what) {
    std::fprintf(stderr, "[lsp/transport] fatal: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

// Put a `FILE*` into binary mode on Windows. LSP mandates byte-exact
// transfer; CRLF translation would corrupt both the `\r\n\r\n`
// separator and every `Content-Length`. POSIX has no such mode, so
// this is a no-op there rather than a second code path.
void setBinaryMode([[maybe_unused]] std::FILE* stream) noexcept {
#ifdef _WIN32
    (void)_setmode(_fileno(stream), _O_BINARY);
#endif
}

} // namespace

StdinByteSource::StdinByteSource() noexcept { setBinaryMode(stdin); }

std::size_t StdinByteSource::read(char* destination, std::size_t count) {
    // `std::fread` blocks until `count` bytes are stored or the stream
    // ends. That is EXACTLY the contract `ByteSource::read` states — and
    // exactly why the caller must never ask for more than the protocol
    // has already promised. See the seam comment in `transport.hpp`.
    return std::fread(destination, 1, count, stdin);
}

bool StdinByteSource::atEnd() const noexcept { return std::feof(stdin) != 0; }

StdoutByteSink::StdoutByteSink() noexcept { setBinaryMode(stdout); }

bool StdoutByteSink::write(std::string_view bytes) {
    const auto written = std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    if (written != bytes.size()) return false;
    return std::fflush(stdout) == 0;
}

struct StdioTransport::Impl {
    std::unique_ptr<ByteSource> source;
    std::unique_ptr<ByteSink>   sink;
    std::mutex                  writeMutex;
    std::string                 readBuffer;   // accumulates partial reads
    std::atomic<bool>           closed{false};
};

StdioTransport::StdioTransport()
    : StdioTransport(std::make_unique<StdinByteSource>(),
                     std::make_unique<StdoutByteSink>()) {}

StdioTransport::StdioTransport(std::unique_ptr<ByteSource> source,
                               std::unique_ptr<ByteSink>   sink)
    : impl_(std::make_unique<Impl>()) {
    if (!source || !sink) {
        transportFatal("StdioTransport constructed with a null byte stream");
    }
    impl_->source = std::move(source);
    impl_->sink   = std::move(sink);
}

StdioTransport::~StdioTransport() noexcept = default;

std::expected<std::string, TransportError> StdioTransport::readMessage() {
    std::string body;
    while (true) {
        if (impl_->closed.load(std::memory_order_acquire)) {
            return std::unexpected(TransportError::Eof);
        }
        // Try to peel a complete frame from the accumulated buffer.
        const auto scan = scanFramedMessage(impl_->readBuffer, body);
        if (scan.state == FrameScanState::Complete) {
            impl_->readBuffer.erase(0, scan.consumed);
            return body;
        }
        if (scan.state == FrameScanState::MalformedHeader) {
            return std::unexpected(TransportError::FramingError);
        }

        // ── THE READ IS DEMAND-DRIVEN, NOT CHUNKED ────────────────────
        //
        // This used to be `std::fread(chunk, 1, 4096, stdin)` with a
        // comment reasoning that "4 KiB at a time balances syscall count
        // with memory churn". ✔MEASURED against a real `dsscp --lsp`
        // over a pipe: `fread` does NOT return short on a pipe — it
        // blocks until the full count arrives or the stream ENDS — and
        // every real client (VS Code, nvim, …) holds stdin OPEN and
        // sends messages far under 4 KiB. A 130-byte `initialize` got
        // ZERO bytes of reply in 3 s; padding the stream past 4 KiB
        // released the whole 745-byte reply at once. The mode could not
        // serve any client, and the comment made the read look
        // deliberate on review.
        //
        // So: ask for exactly what the FRAME still owes us. LSP framing
        // is self-describing, so once the header is in hand that number
        // is known precisely and blocking for it is correct — those
        // bytes are already promised. Until the header terminates, the
        // requirement is not knowable and ONE byte is the only amount a
        // peer with a complete frame in flight is certain to send.
        //
        // Termination: every pass either finishes a frame, returns an
        // error, or appends at least one byte to `readBuffer` — and an
        // unterminated header is capped by `kMaxFrameHeaderBytes` inside
        // the scan. There is no path that neither blocks nor progresses,
        // so this cannot busy-spin.
        //
        // The step cap is the ONE thing this number is clamped by, and it
        // is a memory bound rather than a framing one — see
        // `kMaxReadStepBytes`. Clamping DOWN is always safe: fewer bytes
        // than the frame owes are still bytes the peer has promised.
        const std::size_t need = scan.bytesNeeded == 0 ? 1 : scan.bytesNeeded;
        const std::size_t want = need < kMaxReadStepBytes ? need : kMaxReadStepBytes;
        const std::size_t have = impl_->readBuffer.size();
        impl_->readBuffer.resize(have + want);
        const std::size_t got = impl_->source->read(impl_->readBuffer.data() + have, want);
        if (got > want) {
            // A source that overruns the buffer it was handed has already
            // corrupted memory by the time we can see it; the only honest
            // response is to stop the process rather than frame whatever
            // is now in the buffer.
            transportFatal("ByteSource::read returned more bytes than requested");
        }
        impl_->readBuffer.resize(have + got);
        if (got < want) {
            // Short of an exact, already-promised count ⇒ the stream ended
            // or failed mid-frame. A partial frame is not a message: drop
            // it and report. `atEnd()` separates a clean end (the client
            // closed the pipe — the normal way an editor stops a server)
            // from an I/O failure, which must NOT read as a clean
            // shutdown.
            return std::unexpected(impl_->source->atEnd() ? TransportError::Eof
                                                          : TransportError::IoError);
        }
    }
}

std::expected<void, TransportError> StdioTransport::writeMessage(std::string_view body) {
    // Short-circuit once close() has been observed: a stale worker
    // emitting publishDiagnostics after `exit` would violate LSP's
    // "no traffic after exit" rule. The read side already returns Eof
    // on closed; mirror that on writes.
    if (impl_->closed.load(std::memory_order_acquire)) {
        return std::unexpected(TransportError::IoError);
    }
    const auto framed = frameMessage(body);
    std::lock_guard lk{impl_->writeMutex};
    if (!impl_->sink->write(framed)) {
        return std::unexpected(TransportError::IoError);
    }
    return {};
}

void StdioTransport::close() noexcept {
    impl_->closed.store(true, std::memory_order_release);
}

} // namespace dss::lsp
