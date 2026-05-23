#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

// LSP transport abstraction. Concrete `StdioTransport` wraps
// stdin/stdout; tests substitute an in-memory transport via the
// same interface. `writeMessage` must be internally synchronized
// (worker threads publish diagnostics concurrently). Clean stream
// end returns `TransportError::Eof`.

namespace dss::lsp {

enum class TransportError : std::uint8_t {
    Eof,            // clean stream end — initiate shutdown
    IoError,        // OS-level read/write failure
    FramingError,   // malformed Content-Length header
};

class DSS_EXPORT LspTransport {
public:
    virtual ~LspTransport() noexcept = default;

    LspTransport(LspTransport const&)            = delete;
    LspTransport& operator=(LspTransport const&) = delete;
    LspTransport(LspTransport&&)                 = delete;
    LspTransport& operator=(LspTransport&&)      = delete;

    // Block until one complete Content-Length framed message is
    // available; return its JSON body. On clean EOF returns
    // `TransportError::Eof` so the server loop terminates without
    // logging an error. Called only from the reader thread.
    [[nodiscard]] virtual std::expected<std::string, TransportError>
        readMessage() = 0;

    // Frame and write `body` to the transport. Implementations MUST
    // be internally synchronized — worker threads call this when
    // publishing diagnostics. Returns `TransportError::IoError` on
    // write failure.
    [[nodiscard]] virtual std::expected<void, TransportError>
        writeMessage(std::string_view body) = 0;

    // Signal that the server is shutting down. A `readMessage()`
    // currently blocked must return `TransportError::Eof` on the
    // next opportunity. Thread-safe; idempotent.
    virtual void close() noexcept = 0;

protected:
    LspTransport() noexcept = default;
};

// Concrete stdio implementation. On Windows, sets stdin/stdout to
// binary mode in the constructor (LSP spec mandates byte-exact
// transfer; CRLF translation would corrupt framing).
class DSS_EXPORT StdioTransport final : public LspTransport {
public:
    StdioTransport();
    ~StdioTransport() noexcept override;

    [[nodiscard]] std::expected<std::string, TransportError>
        readMessage() override;

    [[nodiscard]] std::expected<void, TransportError>
        writeMessage(std::string_view body) override;

    void close() noexcept override;

private:
    struct Impl;
    // Pimpl: keeps `std::mutex` + atomic close-flag out of the
    // header (`<mutex>` would force every consumer to include it).
    std::unique_ptr<Impl> impl_;
};

} // namespace dss::lsp
