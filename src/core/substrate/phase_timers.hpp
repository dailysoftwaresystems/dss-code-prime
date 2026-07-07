#pragma once

#include "core/export.hpp"

#include <chrono>
#include <cstdint>
#include <string_view>

// Process-wide accumulating per-phase wall-clock timers for the compile
// pipeline (c97, compile-time-performance arc — the prior root-cause doc's
// own #1 recommendation: convert the INFERRED phase split into MEASURED
// numbers before optimizing).
//
// Every phase is a PIPELINE VERB — the stage transformation the driver runs
// — never a language / target / object-format identity (the standing
// agnosticism veto). The driver's `--time` report reads these accumulators;
// the accumulate side is ALWAYS on (a handful of steady_clock reads per
// phase per compilation unit / target — nanoseconds against a multi-second
// compile; nothing here is per-token), so no enable flag has to thread
// through the pipeline tiers.
//
// Attribution is EXCLUSIVE (self-time): a Scope nested inside another
// Scope (e.g. an import resolver's loadFile callback re-entering the
// parse path) subtracts its wall time from the enclosing scope's phase,
// so the per-phase numbers sum to at most the wall total and never
// double-count. Nesting is tracked per thread; the pipeline's large-stack
// worker joins synchronously, so a scope never spans threads.
//
// Thread-safety: accumulation is a relaxed atomic add. The pipeline itself
// is synchronous, but the LSP server owns a real thread pool — relaxed
// atomics make the accumulators safe from any thread at zero practical cost.
//
// Lifetime: process-global, monotonically accumulating. A driver process
// compiles one invocation and exits, so the totals ARE the invocation's
// totals (multi-target / multi-CU runs accumulate — the `runs` count
// disambiguates). Tests that need isolation call `reset()`.

namespace dss::substrate {

// The pipeline's phase seams, in pipeline order. Names (see
// `compilePhaseName`) are the driver-report vocabulary.
enum class CompilePhase : std::uint8_t {
    Preprocess,      // config-selected preprocessor (splice + tokenize + macro expand)
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
        std::uint64_t nanoseconds = 0;
        std::uint64_t runs        = 0;
    };

    // Accumulate `ns` nanoseconds (one run) into `p`. Relaxed atomic.
    static void add(CompilePhase p, std::uint64_t ns) noexcept;

    // Read one phase's accumulated total + run count.
    [[nodiscard]] static Row read(CompilePhase p) noexcept;

    // Sum of every phase's accumulated nanoseconds (the attributed total).
    [[nodiscard]] static std::uint64_t attributedNanoseconds() noexcept;

    // Zero every accumulator. Test isolation only — the driver never resets.
    static void reset() noexcept;

    // RAII accumulation scope: measures construction -> destruction on the
    // steady clock; the destructor adds the SELF time (wall time minus any
    // nested Scope's wall time on the same thread) to `p`. Early returns /
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
};

} // namespace dss::substrate
