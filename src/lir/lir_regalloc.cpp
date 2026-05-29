#include "lir/lir_regalloc.hpp"

#include "lir/lir_node.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace dss {

namespace {

[[noreturn]] void regallocFatal(char const* what) {
    std::fputs("dss::LirRegAlloc fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Per-class register lists. Each class holds two sub-lists: registers
// classified as caller-saved by the active calling convention, and the
// rest (callee-saved or unclassified — treated as safe across calls).
// Both lists are seeded in target-declared ordinal order and used as
// LIFO stacks (pop/push at the back) — this is O(1) and the order is
// fully deterministic, which is all the allocator requires.
struct RegList {
    std::vector<std::uint16_t> calleeSaved;  // safe across calls
    std::vector<std::uint16_t> callerSaved;  // clobbered by call
};

// One free list per `LirRegClass`. Indexed by static_cast<int>(class).
// The array bound MUST match the enum's cardinality — locked by
// `static_assert` so adding a new class (e.g. predicates for SVE) is a
// compile error here instead of a silent skip in buildFreeLists.
constexpr std::size_t kLirRegClassCount =
    static_cast<std::size_t>(LirRegClass::Flags) + 1u;
using FreeListsByClass = std::array<RegList, kLirRegClassCount>;
static_assert(kLirRegClassCount == 5u,
              "FreeListsByClass size out of sync with LirRegClass enum; "
              "extend kLirRegClassCount when adding a new class");

// Pop the top of `regs`; std::nullopt when empty.
[[nodiscard]] std::optional<std::uint16_t>
popReg(std::vector<std::uint16_t>& regs) {
    if (regs.empty()) return std::nullopt;
    std::uint16_t const r = regs.back();
    regs.pop_back();
    return r;
}

// Build the per-class register lists from the target schema +
// chosen calling convention. Only registers that appear in the
// calling convention's saved-sets, arg-passing sets, or return sets
// are allocatable — registers absent from all of those (e.g. `rsp`,
// `rflags`) are RESERVED and silently kept out of the pools.
// Allocating `rsp` as a GPR would clobber the stack pointer mid-
// function; this guard closes that fatal silent-failure path.
//
// Sub-registers (entries with non-empty `subOf`) are excluded — only
// parent registers are allocated. Downstream encode is expected to
// pick the appropriate sub-width based on the result type.
[[nodiscard]] FreeListsByClass
buildFreeLists(TargetSchema const&            schema,
               TargetCallingConvention const& cc) {
    FreeListsByClass out{};

    // Union of every register name participating in the cc: saved-
    // sets + arg-passing + return. Anything outside this set is
    // reserved (RSP, RFLAGS, etc.).
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
            // Unreachable per `static_assert(kLirRegClassCount == 5)` +
            // the TargetRegClass↔LirRegClass synchrony assert. Fatal
            // so a future enum-cardinality drift is loud.
            regallocFatal("buildFreeLists: TargetRegClass out of range — "
                          "extend kLirRegClassCount");
        }
        if (callerSet.contains(info.name)) {
            out[classIdx].callerSaved.push_back(i);
        } else {
            out[classIdx].calleeSaved.push_back(i);
        }
    }
    return out;
}

// Scan the function for call-shaped opcodes, recording their early-
// position. Call detection is target-agnostic: walks
// `schema.opcodeInfo(op).isCall` rather than matching mnemonic
// strings. For non-register-machine ABIs (operand-stack, result-id)
// the schema may declare no opcodes with `isCall == true`, in which
// case the returned vector is empty and `rangeCrossesCall` returns
// false for every range — the correct behavior because those ABIs
// don't have a caller-saved register clobber model.
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
                out.push_back(pos);  // early-slot of the call inst
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
// itself. The fix below was previously `*lo < r.end` which fired for
// call-args-only vregs and spuriously forced them to callee-saved.
[[nodiscard]] bool
rangeCrossesCall(LirLiveRange const& r,
                 std::vector<std::uint32_t> const& callPositions) {
    auto lo = std::lower_bound(callPositions.begin(), callPositions.end(),
                               r.start);
    if (lo == callPositions.end()) return false;
    // Call at position p occupies [p, p+2). Range crosses iff it is
    // still live AT OR AFTER p+2 — i.e. r.end > p + 1 (the late slot).
    return *lo + 1u < r.end;
}

// Active-list entry: a vreg currently holding a physical reg.
struct ActiveEntry {
    LirLiveRange  range;
    LirRegClass   cls;
    std::uint16_t physOrdinal;
    bool          isCalleeSaved;  // for free-list return
};

// Expire active ranges whose end ≤ currentStart. Return their phys
// regs to the appropriate free list.
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

// Try to obtain a physical register for `r` of class `cls`. When the
// range crosses a call, only callee-saved regs are eligible. Returns
// (ordinal, is_callee_saved) or nullopt when no compatible reg is
// free.
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
    if (crossesCall) return std::nullopt;  // can't use caller-saved
    if (auto r = popReg(bucket.callerSaved); r.has_value()) {
        return AllocPick{*r, false};
    }
    return std::nullopt;
}

// Spill-at-interval: pick the active range of the same class with
// the LATEST end. When `requireCalleeSaved` is true, only callee-
// saved entries are eligible (used when the incoming range crosses
// a call and would otherwise gain nothing from evicting a caller-
// saved holder). Returns iterator-into-active or active.end().
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
    a.vreg      = vreg;
    a.physReg   = phys;
    a.spillSlot = UINT32_MAX;
    return a;
}

LirRegAssignment LirRegAssignment::makeSpill(LirReg vreg, std::uint32_t slot) {
    if (vreg.isPhysical != 0) {
        regallocFatal("makeSpill: input vreg must be virtual");
    }
    if (slot == UINT32_MAX) {
        regallocFatal("makeSpill: slot must not be UINT32_MAX sentinel");
    }
    LirRegAssignment a{};
    a.vreg      = vreg;
    a.physReg   = InvalidLirReg;
    a.spillSlot = slot;
    return a;
}

// ── LirFuncAllocation / LirAllocation ──────────────────────────────

LirRegAssignment const*
LirFuncAllocation::forVReg(std::uint32_t vregId) const noexcept {
    if (vregId == 0 || vregId >= assignments.size()) return nullptr;
    auto const& a = assignments[vregId];
    if (a.vreg.id == 0) return nullptr;  // unfilled slot
    return &a;
}

LirFuncAllocation const* LirAllocation::forFunc(LirFuncId fn) const noexcept {
    for (auto const& f : perFunc) {
        if (f.fn.v == fn.v && f.fn.arenaTag == fn.arenaTag) return &f;
    }
    return nullptr;
}

// ── allocate ───────────────────────────────────────────────────────

LirFuncAllocation
allocateFuncRegisters(Lir const&             lir,
                      TargetSchema const&    schema,
                      LirFuncLiveness const& flow) {
    LirFuncAllocation out;
    out.fn = flow.fn;

    // Always use calling-convention index 0. Per-function cc
    // selection requires functions to carry an explicit cc attribute;
    // not yet implemented. Reordering CCs in the target JSON will
    // change every function's allocation.
    if (schema.callingConventionCount() == 0) {
        regallocFatal("allocateFuncRegisters: target schema declares no "
                      "calling conventions");
    }
    out.callingConventionIndex = 0;
    auto const* cc = schema.callingConvention(0);
    if (cc == nullptr) regallocFatal("allocateFuncRegisters: cc lookup failed");

    FreeListsByClass free = buildFreeLists(schema, *cc);
    std::vector<std::uint32_t> const callPositions =
        collectCallPositions(lir, schema, flow);

    // Pre-size the assignments vector to cover every vreg id observed
    // in the flow. The cycle-1 substrate emits ranges keyed by vreg
    // id; we size to one past the largest id encountered.
    std::uint32_t maxVRegId = 0;
    for (auto const& r : flow.ranges) {
        if (r.vreg.id > maxVRegId) maxVRegId = r.vreg.id;
    }
    out.assignments.assign(maxVRegId + 1u, LirRegAssignment{});

    std::vector<ActiveEntry> active;
    active.reserve(flow.ranges.size());

    // Linear-scan over ranges sorted by start (the substrate
    // guarantees this).
    for (auto const& r : flow.ranges) {
        if (r.vreg.id == 0) continue;  // sentinel; substrate skips these too
        LirRegClass const cls = r.vreg.regClass();
        if (cls == LirRegClass::None) {
            // Substrate violation: a None-class vreg reached liveness.
            // The LirVerifier (cycle 3e rule 3) catches this; if we see
            // it here, the verifier didn't run or has a hole. Loud
            // failure now is better than a silent unassigned vreg in
            // the rewrite pass.
            regallocFatal("allocateFuncRegisters: vreg has LirRegClass::None "
                          "— run LirVerifier before allocator");
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

        // Spill heuristic: pick the same-class active range with the
        // latest end. If that end is later than r's end, evict it and
        // hand r the freed register; otherwise spill r itself. When r
        // crosses a call, the candidate must also be callee-saved —
        // evicting a caller-saved holder gains nothing for r since r
        // would not be allowed to reuse the caller-saved slot anyway.
        auto const spillIt = findSpillCandidate(active, cls, crossesCall);
        bool const evictCandidate =
            spillIt != active.end() && spillIt->range.end > r.end;

        if (!evictCandidate) {
            std::uint32_t const slot = out.numSpillSlots++;
            out.assignments[r.vreg.id] =
                LirRegAssignment::makeSpill(r.vreg, slot);
            continue;
        }

        // Evict spillIt: its vreg goes to a new spill slot; r gets its
        // physical register.
        std::uint32_t const slot = out.numSpillSlots++;
        out.assignments[spillIt->range.vreg.id] =
            LirRegAssignment::makeSpill(spillIt->range.vreg, slot);

        std::uint16_t const freedOrdinal = spillIt->physOrdinal;
        bool const freedIsCalleeSaved    = spillIt->isCalleeSaved;
        active.erase(spillIt);

        LirReg const phys = makePhysicalReg(freedOrdinal, cls);
        out.assignments[r.vreg.id] =
            LirRegAssignment::makePhys(r.vreg, phys);
        active.push_back({r, cls, freedOrdinal, freedIsCalleeSaved});
    }

    return out;
}

LirAllocation
allocateRegisters(Lir const&          lir,
                  TargetSchema const& schema,
                  LirLiveness const&  liveness) {
    LirAllocation out;
    out.perFunc.reserve(liveness.perFunc.size());
    for (auto const& flow : liveness.perFunc) {
        out.perFunc.push_back(allocateFuncRegisters(lir, schema, flow));
    }
    return out;
}

} // namespace dss
