#pragma once

#include "core/export.hpp"

#include <cstddef>
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

// ── THE BYTE SEAM ──────────────────────────────────────────────────────
//
// `StdioTransport` owns FRAMING, not bytes. The bytes arrive through the
// two interfaces below, so the framing/blocking behaviour can be driven
// from a test over a scripted stream instead of only over the process's
// real stdin — which is what left `readMessage` untested, and what let a
// read that can never return on a pipe ship as a shipped mode.
//
// ⚠ WHY `readExact` AND NOT A POSIX-SHAPED "read whatever is available".
// The latter is what a transport would ideally want, and stdio CANNOT
// portably provide it: `std::fread` blocks until the requested count is
// filled or the stream ends, and there is no standard way to ask a
// `FILE*` how much it already holds. Rather than fork on the platform,
// the contract here is the one stdio can honour — "block until EXACTLY
// this many" — and the decision of HOW MANY moves to the layer that
// actually knows: LSP framing is self-describing, so `scanFramedMessage`
// reports the exact remainder and the transport asks for precisely that.
// A source implementation must therefore never be asked for bytes the
// protocol has not already promised, and a test source enforces it.

// The largest single `ByteSource::read` the transport will issue. It is
// NOT a framing bound and it does not reintroduce a fixed chunk: the
// transport still never asks for more than the frame owes, so it never
// waits on a byte the peer has not promised. This caps the OTHER
// direction — a peer declaring `Content-Length: 4000000000` would
// otherwise have the buffer resized to four gigabytes before a single
// byte of it arrived. With the cap, an oversized frame is read in steps
// and the buffer grows only as bytes actually turn up.
inline constexpr std::size_t kMaxReadStepBytes = 1024 * 1024;

class DSS_EXPORT ByteSource {
public:
    virtual ~ByteSource() noexcept = default;

    ByteSource(ByteSource const&)            = delete;
    ByteSource& operator=(ByteSource const&) = delete;
    ByteSource(ByteSource&&)                 = delete;
    ByteSource& operator=(ByteSource&&)      = delete;

    // Store exactly `count` bytes at `destination`, blocking until they
    // arrive. `count` is always >= 1. Returns the number actually
    // stored; a return < `count` means the stream ENDED or FAILED before
    // the count was met — `atEnd()` distinguishes the two, and the
    // caller must treat a failure as an error rather than retrying,
    // because a source that returns short without ending would otherwise
    // spin the reader.
    [[nodiscard]] virtual std::size_t read(char*       destination,
                                           std::size_t count) = 0;

    // True once the stream has been observed to end. Only meaningful
    // after a short `read`.
    [[nodiscard]] virtual bool atEnd() const noexcept = 0;

protected:
    ByteSource() noexcept = default;
};

class DSS_EXPORT ByteSink {
public:
    virtual ~ByteSink() noexcept = default;

    ByteSink(ByteSink const&)            = delete;
    ByteSink& operator=(ByteSink const&) = delete;
    ByteSink(ByteSink&&)                 = delete;
    ByteSink& operator=(ByteSink&&)      = delete;

    // Write ALL of `bytes` and flush. Returns false on any failure —
    // a partial write is a failure, never a success with a shorter
    // count, because a half-written frame desynchronises the peer.
    // Callers hold the transport's write mutex; implementations need
    // no lock of their own.
    [[nodiscard]] virtual bool write(std::string_view bytes) = 0;

protected:
    ByteSink() noexcept = default;
};

// The production source/sink: the process's own `stdin` / `stdout`.
//
// On Windows each puts its stream into BINARY mode on construction —
// LSP mandates byte-exact transfer and CRLF translation would corrupt
// the `\r\n\r\n` separator and every `Content-Length`. POSIX has no
// such mode. ★ The mode change lives HERE rather than in the transport
// so that constructing a transport over a scripted source does not
// reach out and reconfigure the process's real standard streams.
class DSS_EXPORT StdinByteSource final : public ByteSource {
public:
    StdinByteSource() noexcept;
    [[nodiscard]] std::size_t read(char* destination, std::size_t count) override;
    [[nodiscard]] bool atEnd() const noexcept override;
};

class DSS_EXPORT StdoutByteSink final : public ByteSink {
public:
    StdoutByteSink() noexcept;
    [[nodiscard]] bool write(std::string_view bytes) override;
};

// Concrete stream implementation, named for its DEFAULT binding: the
// no-argument constructor reads `stdin` and writes `stdout`, which is
// what `--lsp` runs. The injecting constructor exists for tests.
//
// ⓘ The name is kept deliberately rather than by omission: the type is
// spelled in `src/program/program.cpp` (`runLspMode`), so renaming it to
// `StreamTransport` is a cross-file change, and the default binding it
// names is still the only one production uses.
class DSS_EXPORT StdioTransport final : public LspTransport {
public:
    StdioTransport();

    // Take the byte streams explicitly. Neither may be null.
    StdioTransport(std::unique_ptr<ByteSource> source,
                   std::unique_ptr<ByteSink>   sink);

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
