#include "core/substrate/large_stack_call.hpp"

#include "core/substrate/stack_sized_thread.hpp"

#include <cstdio>
#include <cstdlib>

// runOnLargeStack — see large_stack_call.hpp for the contract + rationale.
//
// The platform work — asking a host for a thread with a CHOSEN stack size, and
// getting an exception back off it — lives in `StackSizedThread`, which is the
// single place in the tree that knows those APIs. This function is the
// SYNCHRONOUS shape of that primitive: spawn, join, propagate.
//
// ★ It was the other way round until 2026-08-25 (cycle P34): this file owned
// the only `pthread_attr_setstacksize` / `_beginthreadex` in the tree, and it
// could only ever spawn-and-join. Anything that needed a sized thread it could
// KEEP — the LSP test harness running a server loop — had no way to ask, fell
// back to `std::async`, and inherited a 512 KiB macOS default that crashed it.
// The fix was to expose the capability, not to copy the platform arms.

namespace dss::substrate {

void runOnLargeStack(std::size_t                  stackBytes,
                     std::function<void()> const& fn) {
    // Caller-contract fail-loud: an empty callable or a zero-byte stack is a
    // bug at the call site, not a recoverable condition. Checked HERE as well
    // as in the primitive so the abort names the facility the caller used.
    if (!fn) {
        std::fputs("dss::substrate::runOnLargeStack fatal: empty callable\n",
                   stderr);
        std::abort();
    }
    if (stackBytes == 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: stackBytes == 0\n",
                   stderr);
        std::abort();
    }

    // ★ The callable is BORROWED, not copied: the worker lambda captures `fn`
    // by reference and invokes the caller's object. Copying a `std::function`
    // copies its captures, so a copy would run a stateful callable's mutations
    // against a temporary and discard them — a silent behaviour change in a
    // facility whose whole contract is "as if `fn` had run inline".
    StackSizedThread worker{stackBytes, [&fn] { fn(); }};

    // Re-throws on this thread whatever the callable threw, after the worker
    // has fully joined.
    worker.join();
}

} // namespace dss::substrate
