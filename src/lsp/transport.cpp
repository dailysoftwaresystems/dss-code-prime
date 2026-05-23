#include "lsp/transport.hpp"

#include "lsp/json_rpc.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>

#ifdef _WIN32
#  include <fcntl.h>
#  include <io.h>
#endif

namespace dss::lsp {

struct StdioTransport::Impl {
    std::mutex            writeMutex;
    std::string           readBuffer;   // accumulates partial reads
    std::atomic<bool>     closed{false};
};

StdioTransport::StdioTransport() : impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    // Binary mode so the LSP framing's `\r\n\r\n` separator isn't
    // mangled by stdio's CRLF translation. POSIX has no such mode.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

StdioTransport::~StdioTransport() noexcept = default;

std::expected<std::string, TransportError> StdioTransport::readMessage() {
    std::string body;
    while (true) {
        if (impl_->closed.load(std::memory_order_acquire)) {
            return std::unexpected(TransportError::Eof);
        }
        // Try to peel a complete frame from the accumulated buffer.
        const auto consumed = tryParseFramedMessage(impl_->readBuffer, body);
        if (consumed > 0) {
            impl_->readBuffer.erase(0, static_cast<std::size_t>(consumed));
            return body;
        }
        if (consumed < 0) {
            return std::unexpected(TransportError::FramingError);
        }
        // Incomplete frame — read more bytes from stdin. 4 KiB at a
        // time balances syscall count with memory churn.
        char  chunk[4096];
        const auto n = std::fread(chunk, 1, sizeof(chunk), stdin);
        if (n == 0) {
            if (std::feof(stdin)) {
                return std::unexpected(TransportError::Eof);
            }
            return std::unexpected(TransportError::IoError);
        }
        impl_->readBuffer.append(chunk, n);
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
    const auto written = std::fwrite(framed.data(), 1, framed.size(), stdout);
    if (written != framed.size()) {
        return std::unexpected(TransportError::IoError);
    }
    if (std::fflush(stdout) != 0) {
        return std::unexpected(TransportError::IoError);
    }
    return {};
}

void StdioTransport::close() noexcept {
    impl_->closed.store(true, std::memory_order_release);
}

} // namespace dss::lsp
