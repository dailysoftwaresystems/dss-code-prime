#include "core/substrate/phase_timers.hpp"

#include <atomic>
#include <thread>

namespace dss::substrate {

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t nsBetween(Clock::time_point a,
                                      Clock::time_point b) noexcept {
    auto const raw =
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
    return raw < 0 ? 0u : static_cast<std::uint64_t>(raw);
}

// ── The CPU (thread-time) accumulators ──────────────────────────────────
//
// Index = CompilePhase ordinal. Relaxed ordering: every writer is joined
// before the reader (the driver's `--time` report; tests) runs, and the join
// itself is the happens-before edge — the atomics exist to make concurrent
// writes from the driver's per-CU pools and the LSP server's pool
// well-defined, not to order them against the reader.
std::atomic<std::uint64_t> gCpuNs[kCompilePhaseCount];
std::atomic<std::uint64_t> gRuns[kCompilePhaseCount];

// ── The WALL (interval-union) bookkeeping ───────────────────────────────
//
// ★ WHY THIS NEEDS A LOCK AND THE COUNTERS ABOVE DO NOT. Opening a phase's
// wall-clock interval is the compound decision "did MY increment take the
// active count from 0 to 1, and if so stamp THIS timestamp as the interval's
// start". Split across two independent atomics that decision races: two
// threads can both observe 0->1, or a closer can read a start another opener
// has not published yet, and the union then silently gains or loses an
// interval. One lock over {count, open-stamp, accumulator} makes each
// transition atomic as a unit.
//
// ★ WHY A SPIN LOCK AND NOT `std::mutex`. Half of these transitions happen
// inside `~Scope`. `std::mutex::lock()` is permitted to throw
// `std::system_error`, and a throw escaping a destructor is
// `std::terminate` — so the primitive used here must be `noexcept` by
// construction. The critical section is a handful of integer operations and
// one duration subtraction (tens of nanoseconds), and it is entered only on
// a scope's four activity transitions (open / suspend-for-child / resume /
// close) — a few thousand times per compile, against a multi-second build.
class SpinLock {
public:
    void lock() noexcept {
        // Test-and-test-and-set: the read-only inner loop keeps a waiter off
        // the cache line's write path while the holder finishes.
        while (flag_.test_and_set(std::memory_order_acquire)) {
            while (flag_.test(std::memory_order_relaxed)) {
                std::this_thread::yield();
            }
        }
    }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

class SpinGuard {
public:
    explicit SpinGuard(SpinLock& l) noexcept : lock_(l) { lock_.lock(); }
    ~SpinGuard() { lock_.unlock(); }
    SpinGuard(SpinGuard const&)            = delete;
    SpinGuard& operator=(SpinGuard const&) = delete;

private:
    SpinLock& lock_;
};

SpinLock gIntervalLock;

// Everything below is guarded by `gIntervalLock`.
std::uint32_t     gActive[kCompilePhaseCount]{};  // self-ACTIVE scopes of this phase
std::uint32_t     gPeak[kCompilePhaseCount]{};    // high-water mark of the above
Clock::time_point gOpenAt[kCompilePhaseCount]{};  // when this phase's open interval began
std::uint64_t     gWallNs[kCompilePhaseCount]{};  // accumulated union, this phase
std::uint32_t     gActiveTotal = 0;               // self-active scopes, ALL phases
Clock::time_point gBusyOpenAt{};                  // when the global busy interval began
std::uint64_t     gBusyNs      = 0;               // accumulated union, ALL phases

// A scope becomes SELF-ACTIVE: on construction, and again when a nested
// child releases it. Guarded — caller holds `gIntervalLock`.
void beginSelfLocked(CompilePhase p, Clock::time_point now) noexcept {
    auto const i = static_cast<std::size_t>(p);
    if (gActive[i]++ == 0) gOpenAt[i] = now;
    if (gActive[i] > gPeak[i]) gPeak[i] = gActive[i];
    if (gActiveTotal++ == 0) gBusyOpenAt = now;
}

// A scope stops being SELF-ACTIVE: on destruction, and while a nested child
// holds the thread. Guarded — caller holds `gIntervalLock`.
void endSelfLocked(CompilePhase p, Clock::time_point now) noexcept {
    auto const i = static_cast<std::size_t>(p);
    if (--gActive[i] == 0) gWallNs[i] += nsBetween(gOpenAt[i], now);
    if (--gActiveTotal == 0) gBusyNs += nsBetween(gBusyOpenAt, now);
}

// Innermost live Scope on THIS thread — the exclusive-attribution chain.
// Defined here (one instance process-wide) rather than in an inline header
// function so a DLL-boundary consumer can never observe a second copy.
thread_local PhaseTimers::Scope* gCurrentScope = nullptr;

// Live Scope objects across every thread. Read by the driver before it
// renders `--time`: a nonzero count there means an interval is still open
// and the union is therefore INCOMPLETE, which the report must say out loud
// instead of printing the short number.
std::atomic<std::size_t> gLiveScopes{0};

} // namespace

std::string_view compilePhaseName(CompilePhase p) noexcept {
    switch (p) {
        case CompilePhase::Preprocess:     return "preprocess";
        case CompilePhase::PreprocessSplice:   return "preprocess-splice";
        case CompilePhase::PreprocessTokenize: return "preprocess-tokenize";
        case CompilePhase::PreprocessExpand:   return "preprocess-expand";
        case CompilePhase::Tokenize:       return "tokenize";
        case CompilePhase::Parse:          return "parse";
        case CompilePhase::Reparse:        return "reparse";
        case CompilePhase::ResolveImports: return "resolve-imports";
        case CompilePhase::Semantic:       return "semantic";
        case CompilePhase::LowerHir:       return "lower-hir";
        case CompilePhase::SynthesizeFfi:  return "synthesize-ffi";
        case CompilePhase::LowerMir:       return "lower-mir";
        case CompilePhase::Optimize:       return "optimize";
        case CompilePhase::Verify:         return "opt-verify";
        case CompilePhase::LowerLir:       return "lower-lir";
        case CompilePhase::Regalloc:       return "regalloc";
        case CompilePhase::Encode:         return "encode";
        case CompilePhase::Link:           return "link";
        case CompilePhase::kCount_:        break;
    }
    return "<invalid-phase>";
}

void PhaseTimers::addCpu(CompilePhase p, std::uint64_t ns) noexcept {
    auto const i = static_cast<std::size_t>(p);
    if (i >= kCompilePhaseCount) return;   // sentinel / corrupted enum — drop
    gCpuNs[i].fetch_add(ns, std::memory_order_relaxed);
    gRuns[i].fetch_add(1u, std::memory_order_relaxed);
}

PhaseTimers::Row PhaseTimers::read(CompilePhase p) noexcept {
    auto const i = static_cast<std::size_t>(p);
    if (i >= kCompilePhaseCount) return {};
    Row row;
    row.cpuNanoseconds = gCpuNs[i].load(std::memory_order_relaxed);
    row.runs           = gRuns[i].load(std::memory_order_relaxed);
    {
        SpinGuard const g{gIntervalLock};
        row.wallNanoseconds = gWallNs[i];
        row.peakConcurrency = gPeak[i];
    }
    return row;
}

std::uint64_t PhaseTimers::attributedCpuNanoseconds() noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kCompilePhaseCount; ++i) {
        total += gCpuNs[i].load(std::memory_order_relaxed);
    }
    return total;
}

std::uint64_t PhaseTimers::busyWallNanoseconds() noexcept {
    SpinGuard const g{gIntervalLock};
    return gBusyNs;
}

std::size_t PhaseTimers::liveScopeCount() noexcept {
    return gLiveScopes.load(std::memory_order_acquire);
}

void PhaseTimers::reset() noexcept {
    SpinGuard const g{gIntervalLock};
    for (std::size_t i = 0; i < kCompilePhaseCount; ++i) {
        gCpuNs[i].store(0, std::memory_order_relaxed);
        gRuns[i].store(0, std::memory_order_relaxed);
        gWallNs[i] = 0;
        gPeak[i]   = 0;
        gActive[i] = 0;
    }
    gBusyNs      = 0;
    gActiveTotal = 0;
}

PhaseTimers::Scope::Scope(CompilePhase p) noexcept
    : p_(p)
    , t0_(Clock::now())
    , parent_(gCurrentScope) {
    gCurrentScope = this;
    gLiveScopes.fetch_add(1u, std::memory_order_release);
    // ★ ORDER IS LOAD-BEARING: the CHILD opens BEFORE the parent suspends, so
    // `gActiveTotal` never dips to zero across the handoff. Reversing these
    // two lines would close and immediately reopen the GLOBAL busy interval
    // at the same instant — harmless for the accumulated total, but it would
    // make the "was anything running" predicate momentarily false, and a
    // future reader of that predicate would see a hole that never existed.
    // Both transitions take ONE timestamp (`t0_`) under ONE lock, so the
    // suspend and the open are simultaneous by construction rather than
    // separated by whatever the second clock read happened to cost.
    SpinGuard const g{gIntervalLock};
    beginSelfLocked(p_, t0_);
    if (parent_ != nullptr) endSelfLocked(parent_->p_, t0_);
}

PhaseTimers::Scope::~Scope() {
    auto const t1 = Clock::now();
    gCurrentScope = parent_;
    {
        // Mirror of the ctor: the parent RESUMES before this scope closes, so
        // the global busy interval stays open across the handoff.
        SpinGuard const g{gIntervalLock};
        if (parent_ != nullptr) beginSelfLocked(parent_->p_, t1);
        endSelfLocked(p_, t1);
    }
    gLiveScopes.fetch_sub(1u, std::memory_order_release);

    std::uint64_t const total = nsBetween(t0_, t1);
    // Self time = own time minus nested scopes' time (never negative — a
    // child's clock reads nest strictly inside the parent's, but guard the
    // subtraction against clock-granularity jitter anyway).
    addCpu(p_, total > childNs_ ? total - childNs_ : 0u);
    if (parent_ != nullptr) parent_->childNs_ += total;
}

} // namespace dss::substrate
