#include "core/substrate/stack_sized_thread.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <utility>

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

// StackSizedThread — see stack_sized_thread.hpp for the contract + rationale.
//
// This file is the ONE place in the tree that knows how to ask a host for a
// thread with a chosen stack size. `runOnLargeStack` is implemented on top of
// it rather than beside it, so the two cannot drift: a platform arm fixed here
// is fixed for both.

namespace dss::substrate {

namespace {

[[noreturn]] void fatal(char const* what) {
    std::fputs("dss::substrate::StackSizedThread fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

struct StackSizedThreadImpl {
    std::function<void()> fn;
    std::exception_ptr    captured = nullptr;
    bool                  joined   = false;
#if defined(_WIN32)
    HANDLE handle = nullptr;
#else
    pthread_t thread{};
#endif
};

namespace {

// Run the callable, capturing any exception into the Impl. An exception must
// never propagate out of a thread entry (that is `std::terminate`), so this
// is noexcept and stows instead.
void runCapturing(StackSizedThreadImpl& impl) noexcept {
    if (!impl.fn) fatal("null callable reached the worker entry");
    try {
        impl.fn();
    } catch (...) {
        impl.captured = std::current_exception();
    }
}

#if defined(_WIN32)

unsigned __stdcall threadEntry(void* arg) noexcept {
    runCapturing(*static_cast<StackSizedThreadImpl*>(arg));
    return 0;
}

#else

void* threadEntry(void* arg) noexcept {
    runCapturing(*static_cast<StackSizedThreadImpl*>(arg));
    return nullptr;
}

#endif

} // namespace

StackSizedThread::StackSizedThread() noexcept = default;

StackSizedThread::StackSizedThread(std::size_t stackBytes, std::function<void()> fn) {
    // Caller-contract fail-loud: an empty callable or a zero-byte stack is a
    // bug at the call site, not a recoverable condition. Aborting (rather than
    // no-op'ing) keeps the facility honest — it never returns having silently
    // NOT run the work on the stack that was asked for.
    if (!fn) fatal("empty callable");
    if (stackBytes == 0) fatal("stackBytes == 0");

    // The Impl is heap-allocated and its address is handed to the worker, so
    // MOVING this object does not move the storage the worker is reading.
    auto impl = std::make_unique<StackSizedThreadImpl>();
    impl->fn  = std::move(fn);

#if defined(_WIN32)
    // _beginthreadex's stack-size argument is `unsigned` (32-bit). A
    // `stackBytes` that does not fit would be silently TRUNCATED by the
    // implicit narrowing — yielding a SMALLER stack than asked for, which is
    // exactly the condition this type exists to remove. Reject it loud.
    if (stackBytes > static_cast<std::size_t>(std::numeric_limits<unsigned>::max())) {
        fatal("stackBytes exceeds the 32-bit Windows stack-size limit");
    }

    // STACK_SIZE_PARAM_IS_A_RESERVATION makes the size argument a RESERVE
    // (committed lazily) rather than an initial commit — so a large reserve
    // costs ~nothing until the stack actually grows into it.
    std::uintptr_t const raw = _beginthreadex(
        /*security=*/nullptr,
        /*stack_size=*/static_cast<unsigned>(stackBytes),
        &threadEntry,
        impl.get(),
        /*initflag=*/STACK_SIZE_PARAM_IS_A_RESERVATION,
        /*thrdaddr=*/nullptr);
    if (raw == 0) fatal("_beginthreadex failed");
    impl->handle = reinterpret_cast<HANDLE>(raw);
#else
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) fatal("pthread_attr_init failed");
    if (pthread_attr_setstacksize(&attr, stackBytes) != 0) {
        pthread_attr_destroy(&attr);
        fatal("pthread_attr_setstacksize failed");
    }
    int const createResult = pthread_create(&impl->thread, &attr, &threadEntry, impl.get());
    // Destroy the attr regardless of create success — it has served its
    // purpose either way (pthread_create copies what it needs).
    pthread_attr_destroy(&attr);
    if (createResult != 0) fatal("pthread_create failed");
#endif

    impl_ = std::move(impl);
}

StackSizedThread::StackSizedThread(StackSizedThread&&) noexcept            = default;
StackSizedThread& StackSizedThread::operator=(StackSizedThread&&) noexcept = default;

StackSizedThread::~StackSizedThread() noexcept {
    // Same contract as `std::thread`: destroying a still-running thread is a
    // caller bug. Detaching silently would let the worker outlive the state it
    // borrows, which turns a visible bug into a use-after-free.
    if (joinable()) fatal("destroyed while still joinable — join() first");
}

bool StackSizedThread::joinable() const noexcept {
    return impl_ != nullptr && !impl_->joined;
}

void StackSizedThread::join() {
    if (!joinable()) return;

#if defined(_WIN32)
    if (WaitForSingleObject(impl_->handle, INFINITE) != WAIT_OBJECT_0) {
        fatal("WaitForSingleObject did not signal completion");
    }
    if (CloseHandle(impl_->handle) == 0) fatal("CloseHandle failed");
    impl_->handle = nullptr;
#else
    if (pthread_join(impl_->thread, nullptr) != 0) fatal("pthread_join failed");
#endif
    impl_->joined = true;

    // The worker has fully joined. If the callable threw, re-throw the
    // captured exception on THIS (the joining) thread so the caller sees it
    // exactly as if the callable had run inline.
    if (impl_->captured) {
        auto e = impl_->captured;
        impl_->captured = nullptr;   // a second join() must not re-throw
        std::rethrow_exception(e);
    }
}

} // namespace dss::substrate
