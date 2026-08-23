#pragma once

#include "core/export.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

// Process-wide accumulating per-phase timers for the compile pipeline (c97,
// compile-time-performance arc — the prior root-cause doc's own #1
// recommendation: convert the INFERRED phase split into MEASURED numbers
// before optimizing).
//
// Every phase is a PIPELINE VERB — the stage transformation the driver runs
// — never a language / target / object-format identity (the standing
// agnosticism veto). The driver's `--time` report reads these accumulators;
// the accumulate side is ALWAYS on (a handful of steady_clock reads per
// phase per compilation unit / target — nanoseconds against a multi-second
// compile; nothing here is per-token), so no enable flag has to thread
// through the pipeline tiers.
//
// ══ TWO CLOCKS, AND THEY ARE NOT THE SAME NUMBER ═══════════════════════════
//
// ★★★ THE PIPELINE IS CONCURRENT, SO ONE COLUMN CANNOT TELL THE TRUTH. Both
// halves of the driver run per-CU work on a thread pool — the front half
// (preprocess / tokenize / parse / resolve-imports, one job per translation
// unit) and the back half (per-CU MIR build). Sixteen threads inside `parse`
// for one wall second produce SIXTEEN CPU-seconds of `parse`. Reporting that
// as if it were elapsed time is how `[other]` — defined as "wall minus the
// attributed sum" — went NEGATIVE on every real build and got clamped to a
// printed `0ms` that was simply false.
//
// So every phase carries TWO independent measurements:
//
//   • `cpuNanoseconds`  — Σ SELF-TIME OVER EVERY THREAD. Thread-time, not
//     elapsed time: it may exceed the process wall clock, and on a parallel
//     build it routinely does. This is the number that answers "how much
//     total work did this phase cost".
//
//   • `wallNanoseconds` — the UNION of this phase's self-intervals projected
//     onto the wall-clock timeline: the elapsed time during which AT LEAST
//     ONE thread was inside this phase. Overlap is counted ONCE, gaps are
//     not counted at all. This is the number that answers "how much of the
//     build's duration did this phase occupy". It is a UNION, deliberately
//     not a first-start→last-end ENVELOPE: an envelope would charge a phase
//     for every gap between its bursts (a phase that runs 144 times across a
//     20 s build would report ~20 s of "wall"), which is the same species of
//     false number this design exists to remove.
//
// By construction `wallNanoseconds <= cpuNanoseconds` for every phase, with
// EQUALITY exactly when the phase never overlapped ITSELF — so on a serial
// build the two columns agree, and the ratio `cpu / wall` IS the phase's
// achieved average concurrency. `peakConcurrency` reports the high-water
// mark of simultaneously-active scopes, which separates "the workers were
// not there" from "the workers were there and idle".
//
// `busyWallNanoseconds()` is the same union taken over ALL phases at once:
// elapsed time during which the pipeline was inside ANY instrumented phase
// on ANY thread. The driver's `[other]` row is `processWall - busy`, and
// THAT subtraction cannot underflow — a union of intervals measured on the
// process's own steady clock is a subset of the process's lifetime. If it
// ever does underflow, the accounting is broken and the report must SAY SO
// rather than clamp (see the driver's `--time` report).
//
// Attribution is EXCLUSIVE (self-time): a Scope nested inside another Scope
// (e.g. an import resolver's loadFile callback re-entering the parse path)
// subtracts its time from the enclosing scope's phase, so the per-phase
// numbers never double-count. Nesting is tracked PER THREAD, and a scope
// never spans threads: each `callOnLargeStack` / pool job runs its scopes
// start-to-finish on the one thread that opened them.
//
// Thread-safety: the cpu/run accumulators are relaxed atomic adds. The
// INTERVAL bookkeeping (which thread-transitions open and close a phase's
// wall-clock union) is guarded by a spin lock, because "did this transition
// take the active count from 0 to 1" cannot be decided from an independent
// atomic counter and an independent timestamp. The lock is taken only on a
// scope's four activity TRANSITIONS (open / suspend-for-child / resume /
// close) — a few thousand times per compile, tens of nanoseconds each — and
// it is a spin lock rather than a `std::mutex` because half of those
// transitions happen in `~Scope`, where a throwing `lock()` would be
// `std::terminate`.
//
// Lifetime: process-global, monotonically accumulating. A driver process
// compiles one invocation and exits, so the totals ARE the invocation's
// totals (multi-target / multi-CU runs accumulate — the `runs` count
// disambiguates). Tests that need isolation call `reset()`.

namespace dss::substrate {

// The pipeline's phase seams, in pipeline order. Names (see
// `compilePhaseName`) are the driver-report vocabulary.
enum class CompilePhase : std::uint8_t {
    Preprocess,        // config-selected preprocessor — the RESIDUAL self-time after the
                       // three sub-phases below (define prologues, buffer freezes, repackage)
    PreprocessSplice,  // build the synth buffer: recursive concat of main + quote-#includes + line-map
    PreprocessTokenize,// tokenize the synth buffer once
    PreprocessExpand,  // macro table build + stream expansion + conditional elision
    Tokenize,        // standalone tokenize (languages without a preprocess block)
    Parse,           // first parse of each tree
    Reparse,         // the type-name-oracle reparse (counted SEPARATELY so its 2x shows)
    ResolveImports,  // per-schema import resolution (both passes)
    Semantic,        // semantic analysis (symbols / types / diagnostics)
    LowerHir,        // CST -> HIR
    SynthesizeFfi,   // FFI metadata synthesis for source-declared externs
    LowerMir,        // HIR -> MIR
    Optimize,        // MIR optimizer pipeline passes (excl. the after-pass verify below)
    Verify,          // the after-every-pass MirVerifier (D-OPT1-VERIFY-AFTER-EVERY-PASS) — split out to measure its share
    LowerLir,        // MIR -> LIR (+ wide-call arg materialization)
    Regalloc,        // liveness + allocation + rewrite + 2-addr legalize + callconv
    Encode,          // assemble to bytes + globals/jump-table/sign-mask data items
    Link,            // link + image write
    kCount_          // sentinel — array bound, never a phase
};

inline constexpr std::size_t kCompilePhaseCount =
    static_cast<std::size_t>(CompilePhase::kCount_);

// Stable report name for a phase (a pipeline verb; lowercase, dash-joined).
[[nodiscard]] DSS_EXPORT std::string_view compilePhaseName(CompilePhase p) noexcept;

class DSS_EXPORT PhaseTimers {
public:
    struct Row {
        // Σ self-time over EVERY thread (thread-time; may exceed process wall).
        std::uint64_t cpuNanoseconds  = 0;
        // Union of this phase's self-intervals on the wall-clock timeline
        // (elapsed time during which >=1 thread was inside this phase).
        // ALWAYS <= cpuNanoseconds; equal iff the phase never overlapped itself.
        std::uint64_t wallNanoseconds = 0;
        std::uint64_t runs            = 0;
        // High-water mark of simultaneously-active scopes of this phase.
        // 1 ⇒ the phase never ran concurrently with itself, so the two time
        // columns above are directly comparable.
        std::uint32_t peakConcurrency = 0;
    };

    // Read one phase's accumulated row.
    [[nodiscard]] static Row read(CompilePhase p) noexcept;

    // Σ of every phase's `cpuNanoseconds` — the attributed THREAD-time. On a
    // parallel build this legitimately EXCEEDS the process wall clock; it is
    // never a valid subtrahend for a wall-clock remainder.
    [[nodiscard]] static std::uint64_t attributedCpuNanoseconds() noexcept;

    // The union of EVERY phase's self-intervals: elapsed wall time during
    // which the pipeline was inside ANY instrumented phase on ANY thread.
    // This IS a valid subtrahend for a wall-clock remainder — it is a subset
    // of the process's lifetime by construction.
    [[nodiscard]] static std::uint64_t busyWallNanoseconds() noexcept;

    // How many Scopes are live right now (any thread). MUST be 0 when the
    // driver renders its report: a nonzero count means a worker outlived the
    // measurement window, so `busyWallNanoseconds()` is missing an interval
    // that is still open. Callers report that as an invariant violation
    // rather than printing the short number as if it were complete.
    [[nodiscard]] static std::size_t liveScopeCount() noexcept;

    // Zero every accumulator. Test isolation only — the driver never resets.
    // Undefined to call with a live Scope anywhere (`liveScopeCount() != 0`);
    // the interval bookkeeping would then close an interval it never opened.
    static void reset() noexcept;

    // RAII accumulation scope: measures construction -> destruction on the
    // steady clock. The destructor adds the SELF time (its own time minus any
    // nested Scope's time on the same thread) to the phase's `cpu` column,
    // and the same self-INTERVALS feed the `wall` union. Early returns /
    // exceptions included. Ctor/dtor live in the .cpp so the thread-local
    // nesting chain has exactly one instance process-wide.
    class DSS_EXPORT Scope {
    public:
        explicit Scope(CompilePhase p) noexcept;
        Scope(Scope const&)            = delete;
        Scope& operator=(Scope const&) = delete;
        Scope(Scope&&)                 = delete;
        Scope& operator=(Scope&&)      = delete;
        ~Scope();

    private:
        CompilePhase                                p_;
        std::chrono::steady_clock::time_point const t0_;
        Scope* const                                parent_;
        std::uint64_t                               childNs_ = 0;
    };

private:
    // Accumulate `ns` nanoseconds (one run) into `p`'s CPU column. Private:
    // a caller that bumped `cpu` without the matching interval bookkeeping
    // would publish a row whose `wall` silently understated its `cpu`, which
    // is the exact class of half-true number this substrate exists to
    // prevent. `Scope` is the only writer.
    static void addCpu(CompilePhase p, std::uint64_t ns) noexcept;
};

} // namespace dss::substrate
