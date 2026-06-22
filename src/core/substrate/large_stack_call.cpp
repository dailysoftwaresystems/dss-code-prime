#include "core/substrate/large_stack_call.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <process.h>  // _beginthreadex
#else
    #include <pthread.h>
#endif

// runOnLargeStack — see large_stack_call.hpp for the contract + rationale.
//
// Both arms share the same shape: a Context carries the borrowed callable
// and an `exception_ptr` slot; the thread entry runs the callable inside a
// try/catch that stows any exception into the slot (so NOTHING escapes the
// entry); the caller joins, then re-throws the stowed exception if there
// was one. The worker stack is RESERVED at `stackBytes` (lazily committed),
// so a shallow call touches ~one page regardless of the reserve.

namespace dss::substrate {

namespace {

struct Context {
    std::function<void()> const* fn      = nullptr;
    std::exception_ptr           captured = nullptr;
};

// Run the callable, capturing any exception into the context. Shared by
// both platform entry points — an exception must never propagate out of a
// thread entry (that is std::terminate), so this is noexcept and stows
// instead. A null `fn`/`ctx` here would be a logic error in this file
// (runOnLargeStack validates `fn` before spawning); assert defensively.
void runCapturing(Context& ctx) noexcept {
    if (ctx.fn == nullptr) {
        std::fputs("dss::substrate::runOnLargeStack fatal: null callable "
                   "reached the worker entry\n",
                   stderr);
        std::abort();
    }
    try {
        (*ctx.fn)();
    } catch (...) {
        ctx.captured = std::current_exception();
    }
}

#if defined(_WIN32)

unsigned __stdcall threadEntry(void* arg) noexcept {
    runCapturing(*static_cast<Context*>(arg));
    return 0;
}

#else

void* threadEntry(void* arg) noexcept {
    runCapturing(*static_cast<Context*>(arg));
    return nullptr;
}

#endif

} // namespace

void runOnLargeStack(std::size_t                  stackBytes,
                     std::function<void()> const& fn) {
    // Caller-contract fail-loud: an empty callable or a zero-byte stack is
    // a bug at the call site, not a recoverable condition. Aborting here
    // (rather than no-op'ing) keeps the facility honest — it never returns
    // having silently NOT run the work on a large stack.
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

    Context ctx;
    ctx.fn = &fn;

#if defined(_WIN32)
    // _beginthreadex's stack-size argument is `unsigned` (32-bit). A
    // `stackBytes` that does not fit would be silently TRUNCATED by the
    // implicit narrowing — yielding a smaller stack than asked for, which
    // would re-introduce the very overflow this facility prevents. Reject
    // it loud instead of truncating.
    if (stackBytes > static_cast<std::size_t>(
                         std::numeric_limits<unsigned>::max())) {
        std::fputs("dss::substrate::runOnLargeStack fatal: stackBytes exceeds "
                   "the 32-bit Windows stack-size limit\n",
                   stderr);
        std::abort();
    }

    // STACK_SIZE_PARAM_IS_A_RESERVATION makes the size argument a RESERVE
    // (committed lazily) rather than an initial commit — so the 64 MiB
    // reserve costs ~nothing until the stack actually grows into it.
    std::uintptr_t const handle = _beginthreadex(
        /*security=*/nullptr,
        /*stack_size=*/static_cast<unsigned>(stackBytes),
        &threadEntry,
        &ctx,
        /*initflag=*/STACK_SIZE_PARAM_IS_A_RESERVATION,
        /*thrdaddr=*/nullptr);
    if (handle == 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: _beginthreadex "
                   "failed\n",
                   stderr);
        std::abort();
    }

    HANDLE const threadHandle = reinterpret_cast<HANDLE>(handle);
    DWORD const  waitResult    = WaitForSingleObject(threadHandle, INFINITE);
    if (waitResult != WAIT_OBJECT_0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: "
                   "WaitForSingleObject did not signal completion\n",
                   stderr);
        std::abort();
    }
    if (CloseHandle(threadHandle) == 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: CloseHandle "
                   "failed\n",
                   stderr);
        std::abort();
    }
#else
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: pthread_attr_init "
                   "failed\n",
                   stderr);
        std::abort();
    }
    if (pthread_attr_setstacksize(&attr, stackBytes) != 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: "
                   "pthread_attr_setstacksize failed\n",
                   stderr);
        std::abort();
    }

    pthread_t thread{};
    int const createResult = pthread_create(&thread, &attr, &threadEntry, &ctx);
    // Destroy the attr regardless of create success — it has served its
    // purpose either way (pthread_create copies what it needs).
    pthread_attr_destroy(&attr);
    if (createResult != 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: pthread_create "
                   "failed\n",
                   stderr);
        std::abort();
    }
    if (pthread_join(thread, nullptr) != 0) {
        std::fputs("dss::substrate::runOnLargeStack fatal: pthread_join "
                   "failed\n",
                   stderr);
        std::abort();
    }
#endif

    // The worker has fully joined. If the callable threw, re-throw the
    // captured exception on THIS (the joining) thread so the caller sees it
    // exactly as if `fn` had run inline.
    if (ctx.captured) {
        std::rethrow_exception(ctx.captured);
    }
}

} // namespace dss::substrate
