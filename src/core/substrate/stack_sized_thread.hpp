#pragma once

#include "core/export.hpp"

#include <cstddef>
#include <functional>
#include <memory>

// StackSizedThread — a thread whose stack size DSS STATES, rather than
// inheriting whatever the host happens to hand out.
//
// WHY this exists (host/OS utility — NOT a language/CPU/format concept):
// `std::thread` — and `std::async(std::launch::async)`, which is one
// underneath — takes the host's DEFAULT secondary-thread stack, and that
// default is not a portable quantity. It spans 16x across the hosts this
// project gates on (DOCUMENTED platform defaults):
//
//     Linux   (glibc pthread)      8 MiB
//     Windows (image default)      1 MiB
//     macOS   (pthread)          512 KiB
//
// ★ A 16x spread means a thread that is comfortable on the leg you develop
// on can be a hard crash on the leg you do not run — and it crashes as
// SIGBUS/SIGSEGV inside the compiler's own stack probe, with no diagnostic
// and no failing assertion. That is the least fail-loud failure available.
//
// ⚠ ✔MEASURED 2026-08-25 (cycle P34,
// D-TEST-LSP-HARNESS-RAN-THE-SERVER-LOOP-ON-A-HOST-DEFAULT-STACK): four LSP
// tests died `Bus error` on macOS and nowhere else. The crash report named it
// exactly — `EXC_BAD_ACCESS (SIGBUS)`, "Thread stack size exceeded", innermost
// frame `___chkstk_darwin` — on a thread whose outermost frames were
// `__thread_proxy` / `_pthread_start`, i.e. a plain `std::thread`. The
// faulting call was ONE function with a **415,360-byte** frame under
// `clang -O0` (`buildSchemaFromJsonText`; the same function is 31,568 bytes
// under `gcc`, which is why no Linux leg ever saw it). 405 KiB of a 512 KiB
// stack, and the thread was standing in for a MAIN thread that has 8 MiB.
//
// ★ THE POINT IS NOT "ASK FOR MORE" — it is that the amount becomes a
// PROPERTY OF THE CODE instead of a property of the machine it happened to
// run on. A leg that passes because its host is generous is not evidence.
//
// AGNOSTIC: this facility names no language, no CPU, no object format. The
// `_WIN32` / POSIX split is pure host-portability — which thread API the host
// offers — exactly as `std::thread`'s own implementation splits.
//
// FAIL-LOUD, NO SILENT FALLBACK: every thread-API failure aborts with a
// stderr message, and so does every caller bug (empty callable, `stackBytes
// == 0`, or a `stackBytes` that would not fit the 32-bit Windows stack-size
// argument). Silently falling back to a default-sized thread is deliberately
// NOT offered: that is precisely the condition this type exists to remove,
// and a fallback would make the failure reappear as a crash on one host.

namespace dss::substrate {

// Defined entirely inside stack_sized_thread.cpp: it holds the platform thread
// handle, so naming it here would drag <pthread.h> / <windows.h> into every
// translation unit that spawns a thread. Namespace-scope rather than a nested
// `Impl` so the file-local thread entry points can name it without friendship.
struct StackSizedThreadImpl;

// Spawn-and-join-later. The callable runs on a thread whose stack is RESERVED
// at `stackBytes` — reserved, not committed, so a shallow call touches about
// one page no matter how large the reserve.
//
// Exceptions: nothing escapes the thread entry (that would be
// `std::terminate`). A throwing callable has its exception stowed and
// RE-THROWN out of `join()`, on the joining thread — so a caller sees it
// exactly as if the callable had run inline.
class DSS_EXPORT StackSizedThread final {
public:
    // Empty — not joinable, join() is a no-op. Exists so the type can be a
    // default-constructed member that a later assignment fills in.
    StackSizedThread() noexcept;

    // Spawns immediately. Aborts (does not throw) on a caller-contract
    // violation or a thread-API failure: see the fail-loud note above.
    StackSizedThread(std::size_t stackBytes, std::function<void()> fn);

    // ⚠ ABORTS if the thread is still joinable, mirroring `std::thread`'s
    // own "destroying a joinable thread is a bug" contract. Silently
    // detaching would let a worker outlive the state it borrows.
    ~StackSizedThread() noexcept;

    StackSizedThread(StackSizedThread&&) noexcept;
    StackSizedThread& operator=(StackSizedThread&&) noexcept;
    StackSizedThread(StackSizedThread const&)            = delete;
    StackSizedThread& operator=(StackSizedThread const&) = delete;

    [[nodiscard]] bool joinable() const noexcept;

    // Blocks until the callable returns, then re-throws whatever it threw.
    // Idempotent: joining an already-joined (or empty) thread does nothing.
    void join();

private:
    std::unique_ptr<StackSizedThreadImpl> impl_;
};

} // namespace dss::substrate
