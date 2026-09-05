#pragma once

// ── THE BOUND: a thread whose stack is DELIBERATELY SMALL ────────────────────
//
// D-COMPILER-INPUT-PROPORTIONAL-RECURSION-RESIDUE-UNCONVERTED-AND-UNCAPPED,
// closing criterion 2: "the proof runs where the gate can SEE it".
//
// ✔MEASURED 2026-09-04 (P60, lane `rc`): three deep-nesting pins that were
// green on every leg of the four-leg gate — Ninja + mingw-w64 g++ 13.2 on
// Windows, gcc on WSL and qemu-arm64, AppleClang on macOS — SEGFAULTED under
// MSVC 19.51 Debug, because a per-level host recursion that costs ~1.6 KiB per
// level on one toolchain costs 8.5 KiB on another, and a pin that merely
// COMPLETES on a ~1 MiB thread measures the margin of the toolchain it happens
// to be built with, not the property. Those greens were a margin, not a proof.
//
// So the pins run their converted stage on a thread whose stack is far SMALLER
// than any host default — small enough that ANY per-level host recursion, at
// the thinnest frame any supported toolchain produces, overflows it at the
// pin's depth, while a converted heap walk costs O(1) host stack per level and
// completes. A regression to recursion is then red on EVERY leg, the Ninja
// gate included, regardless of frame fatness; the arithmetic that makes a
// given depth discriminating is stated beside each pin.
//
// The mechanism is `substrate::StackSizedThread` — the ONE place in the tree
// that asks the host for a thread with a chosen stack size — used here with a
// small size rather than the large one `runOnLargeStack` asks for. Nothing is
// duplicated: this header only fixes the SIZE and hands the callable through.
//
// ⚠ WHAT MUST STAY OFF THIS THREAD: anything whose cost is large but NOT
// proportional to the input. Loading a shipped schema is the documented case
// (`stack_sized_thread.hpp`: `buildSchemaFromJsonText` carries a 415 KB frame
// under `clang -O0`), so every fixture loads its schemas ONCE on the main
// thread and runs only the tokenize → parse → analyze → lower stages here.
// `analyze` reserves its own worker (the fixtures hand it 1 MiB) and is not
// bounded by this thread — the semantic tier's own recursion is another row's.
//
// ⚠ AND WHAT A RED LOOKS LIKE: a stack overflow kills the process with no
// `[  FAILED  ]` line — exactly the unattributable signature every deep-nesting
// fixture is already isolated in its own binary for. `ctest` reports the
// executable as `SegFault` / `Exception`, which is a louder red than an
// assertion and is the correct signal for this class.

#include "core/substrate/stack_sized_thread.hpp"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <utility>

namespace dss::test {

// 256 KiB. ✔MEASURED 2026-09-04 (P60, lane `rc`) on BOTH Windows toolchains —
// mingw-w64 g++ 13.2 Debug (the Ninja gate) and MSVC 19.51 Debug (the fattest
// frames this project supports): every bounded pin below completes on this
// reserve with its converted stage, so the reserve is above every pin's
// non-proportional baseline on the fattest supported build, and each pin's
// depth is chosen so the recursion it replaced would need several times this
// much on the thinnest one.
inline constexpr std::size_t kBoundedStackBytes = std::size_t{256} * 1024;

// The reserve a bounded pin runs on. `DSS_STACK_BOUND_BYTES` is a MEASUREMENT
// knob only: it lets a ceiling be bisected without a rebuild, exactly like
// `DSS_TL_DEPTH`. Unset — which is what the gate runs — the constant stands.
[[nodiscard]] inline std::size_t boundedStackBytes() {
    char const* const e = std::getenv("DSS_STACK_BOUND_BYTES");
    if (e == nullptr || *e == '\0') return kBoundedStackBytes;
    long long const v = std::atoll(e);
    return v > 0 ? static_cast<std::size_t>(v) : kBoundedStackBytes;
}

// Run `fn` to completion on a thread reserving `boundedStackBytes()`, blocking
// the caller. An exception thrown by `fn` is re-thrown here, on the calling
// thread (`StackSizedThread::join` stows and re-throws), so a fixture that
// throws on a precondition miss reports it under its own case name. gtest
// assertions belong on the CALLING thread: collect results in the callable and
// assert after it returns.
inline void runOnBoundedStack(std::function<void()> fn) {
    substrate::StackSizedThread worker{boundedStackBytes(), std::move(fn)};
    worker.join();
}

} // namespace dss::test
