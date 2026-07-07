#include "core/substrate/phase_timers.hpp"

#include <atomic>

namespace dss::substrate {

namespace {

// Process-global accumulators. Index = CompilePhase ordinal. Relaxed
// ordering: the readers (the driver's --time report; tests) run strictly
// after every writer joined (synchronous pipeline / joined worker), so no
// acquire/release pairing is needed — the atomics exist to make concurrent
// LSP-pool writers well-defined, not to order them.
std::atomic<std::uint64_t> gNs[kCompilePhaseCount];
std::atomic<std::uint64_t> gRuns[kCompilePhaseCount];

// Innermost live Scope on THIS thread — the exclusive-attribution chain.
// Defined here (one instance process-wide) rather than in an inline header
// function so a DLL-boundary consumer can never observe a second copy.
thread_local PhaseTimers::Scope* gCurrentScope = nullptr;

} // namespace

std::string_view compilePhaseName(CompilePhase p) noexcept {
    switch (p) {
        case CompilePhase::Preprocess:     return "preprocess";
        case CompilePhase::Tokenize:       return "tokenize";
        case CompilePhase::Parse:          return "parse";
        case CompilePhase::Reparse:        return "reparse";
        case CompilePhase::ResolveImports: return "resolve-imports";
        case CompilePhase::Semantic:       return "semantic";
        case CompilePhase::LowerHir:       return "lower-hir";
        case CompilePhase::SynthesizeFfi:  return "synthesize-ffi";
        case CompilePhase::LowerMir:       return "lower-mir";
        case CompilePhase::Optimize:       return "optimize";
        case CompilePhase::LowerLir:       return "lower-lir";
        case CompilePhase::Regalloc:       return "regalloc";
        case CompilePhase::Encode:         return "encode";
        case CompilePhase::Link:           return "link";
        case CompilePhase::kCount_:        break;
    }
    return "<invalid-phase>";
}

void PhaseTimers::add(CompilePhase p, std::uint64_t ns) noexcept {
    auto const i = static_cast<std::size_t>(p);
    if (i >= kCompilePhaseCount) return;   // sentinel / corrupted enum — drop
    gNs[i].fetch_add(ns, std::memory_order_relaxed);
    gRuns[i].fetch_add(1u, std::memory_order_relaxed);
}

PhaseTimers::Row PhaseTimers::read(CompilePhase p) noexcept {
    auto const i = static_cast<std::size_t>(p);
    if (i >= kCompilePhaseCount) return {};
    return Row{gNs[i].load(std::memory_order_relaxed),
               gRuns[i].load(std::memory_order_relaxed)};
}

std::uint64_t PhaseTimers::attributedNanoseconds() noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kCompilePhaseCount; ++i) {
        total += gNs[i].load(std::memory_order_relaxed);
    }
    return total;
}

void PhaseTimers::reset() noexcept {
    for (std::size_t i = 0; i < kCompilePhaseCount; ++i) {
        gNs[i].store(0, std::memory_order_relaxed);
        gRuns[i].store(0, std::memory_order_relaxed);
    }
}

PhaseTimers::Scope::Scope(CompilePhase p) noexcept
    : p_(p)
    , t0_(std::chrono::steady_clock::now())
    , parent_(gCurrentScope) {
    gCurrentScope = this;
}

PhaseTimers::Scope::~Scope() {
    auto const raw = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0_).count();
    std::uint64_t const total = raw < 0 ? 0u : static_cast<std::uint64_t>(raw);
    gCurrentScope = parent_;
    // Self time = wall time minus nested scopes' wall time (never negative —
    // a child's clock reads nest strictly inside the parent's, but guard the
    // subtraction against clock-granularity jitter anyway).
    add(p_, total > childNs_ ? total - childNs_ : 0u);
    if (parent_ != nullptr) parent_->childNs_ += total;
}

} // namespace dss::substrate
