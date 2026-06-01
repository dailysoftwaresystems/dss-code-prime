#include "lir/lir_regalloc.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_node.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

// Producer-side invariants (factory misuse). Genuine programmer
// errors — substrate-tier consumers route data-driven failures
// through `DiagnosticReporter` instead.
[[noreturn]] void regallocFatal(char const* what) {
    std::fputs("dss::LirRegAlloc fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// `report()` shim hoisted to `core/types/diagnostic_reporter.hpp` as
// `dss::report` at LK10 cycle 3 post-fold #2 (D-LK10-8). Call sites
// below resolve to the canonical free function via ADL.

// Per-class register lists. The naming is conservative: `calleeSaved`
// here means "treated as call-safe for allocation" — populated with
// every register in the cc's `allocatable` set that is NOT in
// `cc.callerSaved`. A target that declares an arg-only or return-only
// register without also placing it in `callerSaved` will land that
// register in this bucket; downstream cross-call ranges will use it.
// The conservatism assumes any register a producer deliberately omits
// from `callerSaved` is safe to keep live across a call.
struct RegList {
    std::vector<std::uint16_t> calleeSaved;
    std::vector<std::uint16_t> callerSaved;
};

// kLirRegClassCount derives from `LirRegClass::Flags + 1` — extending
// the enum past `Flags` auto-widens the constant. The literal lock
// (`static_assert(kLirRegClassCount == 5u)`) pins the count so adding
// a tail entry (e.g. predicates for SVE) trips here and forces an
// audit of `buildFreeLists`, the bucket layout, and downstream
// consumers.
constexpr std::size_t kLirRegClassCount =
    static_cast<std::size_t>(LirRegClass::Flags) + 1u;
using FreeListsByClass = std::array<RegList, kLirRegClassCount>;
static_assert(kLirRegClassCount == 5u,
              "FreeListsByClass size out of sync with LirRegClass enum; "
              "audit buildFreeLists when adding a new class");

[[nodiscard]] std::optional<std::uint16_t>
popReg(std::vector<std::uint16_t>& regs) {
    if (regs.empty()) return std::nullopt;
    std::uint16_t const r = regs.back();
    regs.pop_back();
    return r;
}

[[nodiscard]] FreeListsByClass
buildFreeLists(TargetSchema const&            schema,
               TargetCallingConvention const& cc) {
    FreeListsByClass out{};

    std::unordered_set<std::string_view> allocatable;
    auto absorb = [&](std::vector<std::string> const& names) {
        for (auto const& n : names) allocatable.insert(n);
    };
    absorb(cc.callerSaved);
    absorb(cc.calleeSaved);
    absorb(cc.argGprs);
    absorb(cc.argFprs);
    absorb(cc.returnGprs);
    absorb(cc.returnFprs);

    std::unordered_set<std::string_view> callerSet;
    callerSet.reserve(cc.callerSaved.size());
    for (auto const& n : cc.callerSaved) callerSet.insert(n);

    auto const regs = schema.registers();
    for (std::uint16_t i = 0; i < regs.size(); ++i) {
        auto const& info = regs[i];
        if (info.regClass == TargetRegClass::None) continue;
        if (!info.subOf.empty()) continue;
        if (!allocatable.contains(info.name)) continue;  // reserved (rsp / rflags / …)
        std::size_t const classIdx = static_cast<std::size_t>(info.regClass);
        if (classIdx >= out.size()) {
            regallocFatal("buildFreeLists: TargetRegClass out of range — "
                          "audit kLirRegClassCount");
        }
        if (callerSet.contains(info.name)) {
            out[classIdx].callerSaved.push_back(i);
        } else {
            out[classIdx].calleeSaved.push_back(i);
        }
    }
    return out;
}

// Returns the EARLY slot (`pos`) of each call instruction, scaled to
// liveness's 2-slot-per-inst convention (see lir_liveness.cpp). The
// `pos += 2u` arithmetic is coupled to `rangeCrossesCall`'s `p + 1`
// (= late slot) test AND to liveness's slot scale — these three must
// move together.
[[nodiscard]] std::vector<std::uint32_t>
collectCallPositions(Lir const& lir, TargetSchema const& schema,
                     LirFuncLiveness const& flow) {
    std::vector<std::uint32_t> out;
    std::uint32_t pos = 0;
    for (auto const& b : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
            if (info != nullptr && info->isCall) {
                out.push_back(pos);
            }
            pos += 2u;
        }
    }
    return out;
}

// True iff the range is live STRICTLY PAST the call's late slot —
// i.e. survives the call's caller-saved clobber. A vreg used only as
// a call argument has `range.end == call.early + 1 == call.late`
// (the use is at `call.early`, range end is `lastUse + 1`); that
// case is NOT a crossing because the value is consumed by the call
// itself.
[[nodiscard]] bool
rangeCrossesCall(LirLiveRange const& r,
                 std::vector<std::uint32_t> const& callPositions) {
    auto lo = std::lower_bound(callPositions.begin(), callPositions.end(),
                               r.start);
    if (lo == callPositions.end()) return false;
    return *lo + 1u < r.end;
}

struct ActiveEntry {
    LirLiveRange  range;
    LirRegClass   cls;
    std::uint16_t physOrdinal;
    bool          isCalleeSaved;
};

void expireActive(std::vector<ActiveEntry>& active,
                  FreeListsByClass&         free,
                  std::uint32_t             currentStart) {
    auto it = active.begin();
    while (it != active.end()) {
        if (it->range.end <= currentStart) {
            auto& bucket = free[static_cast<std::size_t>(it->cls)];
            auto& list   = it->isCalleeSaved ? bucket.calleeSaved
                                             : bucket.callerSaved;
            list.push_back(it->physOrdinal);
            it = active.erase(it);
        } else {
            ++it;
        }
    }
}

struct AllocPick {
    std::uint16_t ordinal;
    bool          isCalleeSaved;
};

[[nodiscard]] std::optional<AllocPick>
tryAllocate(FreeListsByClass& free, LirRegClass cls, bool crossesCall) {
    auto& bucket = free[static_cast<std::size_t>(cls)];
    if (auto r = popReg(bucket.calleeSaved); r.has_value()) {
        return AllocPick{*r, true};
    }
    if (crossesCall) return std::nullopt;
    if (auto r = popReg(bucket.callerSaved); r.has_value()) {
        return AllocPick{*r, false};
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<ActiveEntry>::iterator
findSpillCandidate(std::vector<ActiveEntry>& active, LirRegClass cls,
                   bool requireCalleeSaved) {
    auto best = active.end();
    std::uint32_t bestEnd = 0;
    for (auto it = active.begin(); it != active.end(); ++it) {
        if (it->cls != cls) continue;
        if (requireCalleeSaved && !it->isCalleeSaved) continue;
        if (it->range.end > bestEnd) {
            bestEnd = it->range.end;
            best    = it;
        }
    }
    return best;
}

// Per-function spill bookkeeping. Aggregated and emitted as a single
// `R_SpillSummary` note at end-of-function so the reporter's per-code
// cap (50) cannot silently drop notes on highly-pressured functions
// (the per-vreg-note design would lose data past the 50th spill with
// no visible signal).
struct SpillStats {
    std::uint32_t pressure       = 0;
    std::uint32_t crossCallExhaustion = 0;
};

void emitSpillSummary(DiagnosticReporter& reporter, LirFuncId fn,
                      SpillStats const& s) {
    if (s.pressure == 0 && s.crossCallExhaustion == 0) return;
    DiagnosticCode const code =
        (s.crossCallExhaustion > 0)
            ? DiagnosticCode::R_SpilledDueToCrossCallExhaustion
            : DiagnosticCode::R_SpilledDueToPressure;
    report(reporter, code, DiagnosticSeverity::Info,
           std::format("func {} spilled {} vreg(s) ({} pressure, "
                       "{} cross-call exhaustion)",
                       fn.v,
                       s.pressure + s.crossCallExhaustion,
                       s.pressure, s.crossCallExhaustion));
}

} // namespace

// ── LirRegAssignment ────────────────────────────────────────────────

LirRegAssignment LirRegAssignment::makePhys(LirReg vreg, LirReg phys) {
    if (vreg.isPhysical != 0) {
        regallocFatal("makePhys: input vreg must be virtual");
    }
    if (phys.isPhysical != 1) {
        regallocFatal("makePhys: output must be a physical register");
    }
    if (vreg.regClass() != phys.regClass()) {
        regallocFatal("makePhys: class mismatch between vreg and physReg");
    }
    LirRegAssignment a{};
    a.vreg       = vreg;
    a.assignment = phys;
    return a;
}

LirRegAssignment LirRegAssignment::makeSpill(LirReg vreg, LirSpillSlot slot) {
    if (vreg.isPhysical != 0) {
        regallocFatal("makeSpill: input vreg must be virtual");
    }
    if (!slot.valid()) {
        regallocFatal("makeSpill: slot must be valid (v != 0)");
    }
    LirRegAssignment a{};
    a.vreg       = vreg;
    a.assignment = slot;
    return a;
}

// ── LirFuncAllocation / LirAllocation ──────────────────────────────

LirRegAssignment const*
LirFuncAllocation::forVReg(std::uint32_t vregId) const noexcept {
    // id 0 is the sentinel slot — never a valid lookup target.
    // Out-of-range ids return nullptr rather than UB.
    if (vregId == 0 || vregId >= assignments.size()) return nullptr;
    auto const& a = assignments[vregId];
    if (a.vreg.id == 0) return nullptr;  // unfilled slot
    return &a;
}

bool LirAllocation::ok() const noexcept {
    for (auto const& f : perFunc) {
        if (!f.ok) return false;
    }
    return true;
}

LirFuncAllocation const* LirAllocation::forFunc(LirFuncId fn) const noexcept {
    for (auto const& f : perFunc) {
        if (f.fn.v == fn.v && f.fn.arenaTag == fn.arenaTag) return &f;
    }
    return nullptr;
}

// ── allocate ───────────────────────────────────────────────────────

namespace {

// Per-function core. Wraps the linear-scan loop with `ok` derivation
// via reporter delta + emits the per-function spill summary at the
// end. `schemaOk` is the pre-checked schema-wide validity (≥1 cc) —
// false short-circuits to an empty result with `ok = false`.
LirFuncAllocation allocateOneFunc(Lir const& lir,
                                  TargetSchema const& schema,
                                  LirFuncLiveness const& flow,
                                  std::uint16_t callingConventionIndex,
                                  DiagnosticReporter& reporter,
                                  bool schemaOk) {
    LirFuncAllocation out;
    out.fn = flow.fn;
    out.originalSymbol = SymbolId{lir.funcArena().at(flow.fn).symbol};
    auto const baseline = reporter.errorCount();
    if (!schemaOk) {
        // Schema-wide error already reported by the caller; mark this
        // func failed without re-emitting (avoids per-func duplication
        // that the reporter's dedup-window would silently swallow).
        out.ok = false;
        return out;
    }

    // D-FF3-3 post-fold #5: callingConventionIndex now comes from
    // `resolveAbi(target, format)` resolution at compileOneTarget,
    // threaded through compileSingleUnit. The previous hardcoded
    // `0` silently dispatched non-ELF targets (e.g. PE64+x86_64)
    // to the first cc (sysv_amd64) instead of the correct cc
    // (ms_x64) — a real miscompile surface, not a substrate
    // placeholder.
    out.callingConventionIndex = callingConventionIndex;
    auto const* cc = schema.callingConvention(callingConventionIndex);
    if (cc == nullptr) {
        report(reporter, DiagnosticCode::R_CallingConventionLookupFailed,
               DiagnosticSeverity::Error,
               std::format("calling convention index {} lookup returned "
                           "nullptr (target schema declares {} cc rows)",
                           static_cast<unsigned>(callingConventionIndex),
                           schema.callingConventionCount()));
        out.ok = false;
        return out;
    }

    FreeListsByClass free = buildFreeLists(schema, *cc);
    std::vector<std::uint32_t> const callPositions =
        collectCallPositions(lir, schema, flow);

    std::uint32_t maxVRegId = 0;
    for (auto const& r : flow.ranges) {
        if (r.vreg.id > maxVRegId) maxVRegId = r.vreg.id;
    }
    out.assignments.assign(maxVRegId + 1u, LirRegAssignment{});

    std::vector<ActiveEntry> active;
    active.reserve(flow.ranges.size());

    SpillStats spills;
    // Slots start at 1; slot 0 is the LirSpillSlot invalid sentinel.
    std::uint32_t nextSlotV = 1;

    auto mintSlot = [&]() -> LirSpillSlot {
        LirSpillSlot const s{nextSlotV++};
        ++out.numSpillSlots;
        return s;
    };

    for (auto const& r : flow.ranges) {
        if (r.vreg.id == 0) continue;
        LirRegClass const cls = r.vreg.regClass();
        if (cls == LirRegClass::None) {
            report(reporter, DiagnosticCode::R_VRegHasNoClass,
                   DiagnosticSeverity::Error,
                   std::format("func {} vreg id {} has LirRegClass::None — "
                               "run LirVerifier before allocator",
                               flow.fn.v,
                               static_cast<std::uint32_t>(r.vreg.id)));
            continue;
        }

        expireActive(active, free, r.start);

        bool const crossesCall = rangeCrossesCall(r, callPositions);

        if (auto pick = tryAllocate(free, cls, crossesCall); pick.has_value()) {
            LirReg const phys = makePhysicalReg(pick->ordinal, cls);
            out.assignments[r.vreg.id] =
                LirRegAssignment::makePhys(r.vreg, phys);
            active.push_back({r, cls, pick->ordinal, pick->isCalleeSaved});
            continue;
        }

        // Invariant: every spill emits exactly one slot increment via
        // `mintSlot` and contributes to one `SpillStats` counter.
        auto const spillIt = findSpillCandidate(active, cls, crossesCall);
        bool const evictCandidate =
            spillIt != active.end() && spillIt->range.end > r.end;

        if (!evictCandidate) {
            // Spill r itself.
            LirSpillSlot const slot = mintSlot();
            out.assignments[r.vreg.id] =
                LirRegAssignment::makeSpill(r.vreg, slot);
            if (crossesCall) ++spills.crossCallExhaustion;
            else             ++spills.pressure;
            continue;
        }

        // Evict spillIt: its vreg goes to a new spill slot; r gets its
        // physical register. The evicted range's spill cause is its
        // OWN crossesCall status, not r's — they may differ.
        LirSpillSlot const slot = mintSlot();
        out.assignments[spillIt->range.vreg.id] =
            LirRegAssignment::makeSpill(spillIt->range.vreg, slot);
        bool const evictedCrossesCall =
            rangeCrossesCall(spillIt->range, callPositions);
        if (evictedCrossesCall) ++spills.crossCallExhaustion;
        else                    ++spills.pressure;

        std::uint16_t const freedOrdinal = spillIt->physOrdinal;
        bool const freedIsCalleeSaved    = spillIt->isCalleeSaved;
        active.erase(spillIt);

        LirReg const phys = makePhysicalReg(freedOrdinal, cls);
        out.assignments[r.vreg.id] =
            LirRegAssignment::makePhys(r.vreg, phys);
        active.push_back({r, cls, freedOrdinal, freedIsCalleeSaved});
    }

    emitSpillSummary(reporter, flow.fn, spills);
    out.ok = (reporter.errorCount() == baseline);
    return out;
}

} // namespace

LirFuncAllocation
allocateFuncRegisters(Lir const&             lir,
                      TargetSchema const&    schema,
                      LirFuncLiveness const& flow,
                      std::uint16_t          callingConventionIndex,
                      DiagnosticReporter&    reporter) {
    bool const schemaOk = (schema.callingConventionCount() > 0);
    if (!schemaOk) {
        report(reporter, DiagnosticCode::R_NoCallingConventions,
               DiagnosticSeverity::Error,
               "target schema declares no calling conventions");
    }
    return allocateOneFunc(lir, schema, flow,
                           callingConventionIndex,
                           reporter, schemaOk);
}

LirAllocation
allocateRegisters(Lir const&          lir,
                  TargetSchema const& schema,
                  LirLiveness const&  liveness,
                  std::uint16_t       callingConventionIndex,
                  DiagnosticReporter& reporter) {
    LirAllocation out;
    bool const schemaOk = (schema.callingConventionCount() > 0);
    if (!schemaOk) {
        // Emit ONCE at module level rather than re-emitting per-
        // function (which would hit the reporter's dedup window after
        // the 4th identical message).
        report(reporter, DiagnosticCode::R_NoCallingConventions,
               DiagnosticSeverity::Error,
               "target schema declares no calling conventions");
    }
    out.perFunc.reserve(liveness.perFunc.size());
    for (auto const& flow : liveness.perFunc) {
        out.perFunc.push_back(
            allocateOneFunc(lir, schema, flow,
                            callingConventionIndex, reporter, schemaOk));
    }
    return out;
}

} // namespace dss
