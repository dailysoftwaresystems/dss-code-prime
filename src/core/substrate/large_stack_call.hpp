#pragma once

#include "core/export.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

// runOnLargeStack — the standard compiler technique for deep recursion:
// run a recursive callable on a DEDICATED worker thread that owns a large
// RESERVED stack, JOIN it (the caller blocks — this is synchronous, there
// is NO concurrency), and propagate any exception the callable threw.
//
// WHY this exists (host/OS utility — NOT a language/CPU/format concept):
// the frontend's expression-tree walks (semantic `analyze`; potentially
// HIR/MIR lowering) recurse once per nesting level. On the host's MAIN
// thread (a ~1 MB stack on Windows) a deeply-nested expression overflows
// the stack at ~25 levels — a raw crash (rc-127), no diagnostic
// (`D-PARSE-DEEP-FRONTEND-STACK`). Re-running that recursion on a worker
// thread with a multi-MiB reserved stack lifts the ceiling so a deep-but-
// legal expression compiles; the parser's `maxExpressionDepth` cap then
// becomes a real SEMANTIC limit rather than a host-stack artifact.
//
// AGNOSTIC: this facility names no language, no CPU, no object format. The
// `_WIN32` / POSIX `#ifdef` is pure host-portability (which thread API the
// host offers), exactly like `std::thread`'s own platform split.
//
// FAIL-LOUD, NO SILENT FALLBACK: every thread-API failure aborts with a
// stderr message, and so does every caller bug (empty `fn`, `stackBytes ==
// 0`, or — on Windows, where the stack-size argument is 32-bit — a
// `stackBytes` that would not fit in `unsigned`). A silent inline fallback
// is deliberately NOT offered: running the recursion inline on the small
// host stack is exactly the overflow this facility exists to prevent.

namespace dss::substrate {

// Reserved worker-thread stack for the frontend's deep-recursion stages.
//
// Sizing rationale: the parser caps expression nesting at 256 levels and
// the heaviest downstream walk costs on the order of ~40 KB of stack per
// level, so the worst legal tree needs ~256 * 40 KB ≈ 10 MB. 64 MiB is a
// generous headroom multiple. A *reserved* stack of this size costs almost
// nothing until touched — the OS commits pages lazily as the stack grows —
// so a shallow expression (the overwhelming common case) pays ~one page,
// not 64 MiB.
inline constexpr std::size_t kDeepRecursionStackBytes =
    std::size_t{64} * 1024 * 1024;

// Run `fn` to completion on a worker thread whose stack reserves
// `stackBytes`, blocking the caller until it finishes. If `fn` throws, the
// exception is captured on the worker and RE-THROWN here (an exception must
// NEVER escape the thread entry point — that is `std::terminate`). Aborts
// loud on any thread-API failure or caller-contract violation (see header
// comment); never returns having silently run `fn` somewhere smaller.
DSS_EXPORT void runOnLargeStack(std::size_t                  stackBytes,
                                std::function<void()> const& fn);

// Value-returning convenience wrapper over `runOnLargeStack`: invokes
// `f()` on the large stack and returns its result. The result is staged in
// an `optional` filled by the worker; a never-filled optional (which would
// mean the worker returned without running `f`, a contract violation that
// `runOnLargeStack` already aborts on) fails loud here too rather than
// default-constructing a bogus `R`.
template <class F>
[[nodiscard]] auto callOnLargeStack(std::size_t bytes, F&& f)
    -> std::invoke_result_t<F&> {
    using R = std::invoke_result_t<F&>;
    static_assert(!std::is_void_v<R>,
                  "callOnLargeStack requires a value-returning callable; "
                  "use runOnLargeStack for a void callable");

    std::optional<R> result;
    runOnLargeStack(bytes, [&] { result.emplace(f()); });
    if (!result.has_value()) {
        std::fputs("dss::substrate::callOnLargeStack fatal: worker returned "
                   "without producing a result\n",
                   stderr);
        std::abort();
    }
    return std::move(*result);
}

} // namespace dss::substrate
