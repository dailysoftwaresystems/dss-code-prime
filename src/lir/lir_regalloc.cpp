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
#include <span>
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

// Per-opcode implicit-clobber consumer (cycle 10q closure of the
// 10p substrate). Some opcodes (x86 idiv/div, future x86 shift-by-CL,
// future mul-1-op-for-128-bit-result) destroy specific physical
// registers as part of their semantic contract — distinct from
// caller-saved (which is target-wide, applies to all calls) and
// distinct from requires2Address (which forces ops[0]==result).
// The 10p substrate declared the constraint per-opcode JSON-side;
// 10q wires the regalloc to read + respect it.
//
// Mechanism mirrors callPositions: scan the LIR once, collect a
// (position, clobbered-ordinals) entry per opcode-with-clobbers.
// Per-range allocation then checks crossings + adds the union of
// crossed clobbers to the exclusion set passed to tryAllocate
// Excluding. Universal across CPUs: the constraint is per-opcode-
// JSON-declared; no `if (opcode == idiv)` ever.
struct ImplicitClobberAt {
    std::uint32_t              position;
    std::vector<std::uint16_t> clobberedOrdinals;
};

[[nodiscard]] std::vector<ImplicitClobberAt>
collectImplicitClobberPositions(Lir const& lir, TargetSchema const& schema,
                                LirFuncLiveness const& flow) {
    std::vector<ImplicitClobberAt> out;
    std::uint32_t pos = 0;
    for (auto const& b : flow.blockOrder) {
        std::uint32_t const n = lir.blockInstCount(b);
        for (std::uint32_t i = 0; i < n; ++i) {
            LirInstId const inst = lir.blockInstAt(b, i);
            auto const* info = schema.opcodeInfo(lir.instOpcode(inst));
            if (info != nullptr
             && info->implicitRegisters.has_value()
             && !info->implicitRegisters->clobberedOrdinals.empty()) {
                out.push_back({pos,
                               info->implicitRegisters->clobberedOrdinals});
            }
            pos += 2u;
        }
    }
    return out;
}

// Returns the union of clobbered-register ordinals across every
// implicit-clobber opcode the range crosses (range.start <= pos+1 <
// range.end — same "strictly past" semantics as rangeCrossesCall).
// The result is written into `out` and the size returned via
// reference so callers can chain into the existing fixed-size
// exclusion array without heap allocation per range.
template <std::size_t N>
[[nodiscard]] std::size_t
implicitClobbersCrossedBy(LirLiveRange const& r,
                          std::vector<ImplicitClobberAt> const& clobbers,
                          std::array<std::uint16_t, N>& out,
                          std::size_t outAlreadyFilled) {
    std::size_t filled = outAlreadyFilled;
    for (auto const& c : clobbers) {
        if (c.position < r.start) continue;
        if (c.position + 1u >= r.end) continue;
        for (std::uint16_t const ord : c.clobberedOrdinals) {
            // Dedup against what's already in `out` (the
            // requires2Address pass populated the leading slice;
            // multiple implicit-clobber positions may repeat the
            // same ordinal).
            bool already = false;
            for (std::size_t k = 0; k < filled; ++k) {
                if (out[k] == ord) { already = true; break; }
            }
            if (already) continue;
            if (filled >= out.size()) {
                regallocFatal(
                    "implicit-clobber + 2-addr exclusion union "
                    "exceeds fixed exclusion buffer size — extend "
                    "excludedStorage in lir_regalloc.cpp OR re-shape "
                    "the schema so the high-pressure case lands "
                    "differently (D-OPT-REGALLOC-EXCLUSION-BUFFER)");
            }
            out[filled++] = ord;
        }
    }
    return filled;
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

// Pop a free register, SKIPPING any ordinal in `excluded`. Matches
// the `tryAllocate` policy (callee-saved first, then caller-saved
// unless crossesCall) but removes the picked entry only when it's
// admissible. Excluded entries stay in the bucket (will be returned
// to circulation when an unfettered call site asks for them).
//
// Closes D-CSUBSET-BINOP-RIGHT-CLOBBER (2026-06-02): when allocating
// the result of a `requires2Address` instruction, operand[1..N]'s
// physical registers must not be selected — the 2-addr legalize
// would otherwise emit `mov result, ops[0]` and CLOBBER ops[N]'s
// value before the binary op reads it. Universal across CPUs +
// commutativity (the alias is a regalloc-tier invariant, not a
// per-op special case).
[[nodiscard]] std::optional<AllocPick>
tryAllocateExcluding(FreeListsByClass& free,
                     LirRegClass cls,
                     bool crossesCall,
                     std::span<std::uint16_t const> excluded) {
    // Empty-excluded fast path falls back to the standard policy
    // (preserves existing allocation traces for tests).
    if (excluded.empty()) {
        return tryAllocate(free, cls, crossesCall);
    }
    auto isExcluded = [&](std::uint16_t ord) noexcept {
        for (auto e : excluded) if (e == ord) return true;
        return false;
    };
    auto popFiltered = [&](std::vector<std::uint16_t>& regs)
        -> std::optional<std::uint16_t> {
        // Scan back-to-front (LIFO, matching popReg's order). The
        // first non-excluded ordinal is returned and erased.
        for (auto it = regs.rbegin(); it != regs.rend(); ++it) {
            if (!isExcluded(*it)) {
                std::uint16_t const ord = *it;
                regs.erase(std::next(it).base());
                return ord;
            }
        }
        return std::nullopt;
    };
    auto& bucket = free[static_cast<std::size_t>(cls)];
    if (auto r = popFiltered(bucket.calleeSaved); r.has_value()) {
        return AllocPick{*r, true};
    }
    if (crossesCall) return std::nullopt;
    if (auto r = popFiltered(bucket.callerSaved); r.has_value()) {
        return AllocPick{*r, false};
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<ActiveEntry>::iterator
findSpillCandidate(std::vector<ActiveEntry>& active, LirRegClass cls,
                   bool requireCalleeSaved,
                   std::span<std::uint16_t const> excluded = {}) {
    // D-CSUBSET-BINOP-RIGHT-CLOBBER spill-aware closure (silent-
    // failure audit HIGH-1, 2026-06-02): when the caller is
    // resolving a `requires2Address` result whose `tryAllocate
    // Excluding` returned nullopt, the spill fallback MUST NOT
    // pick an evictee whose physical ordinal is in the excluded
    // set — otherwise the freed register lands on operand[k>=1]'s
    // ordinal and the clobber bug recurs under register pressure
    // (just-freed-reg → result-vreg → mov clobbers source). Pass
    // the same excluded span used for tryAllocateExcluding so the
    // exclusion contract holds end-to-end across the alloc + spill
    // arms.
    auto const isExcluded = [&](std::uint16_t ord) noexcept {
        for (auto e : excluded) if (e == ord) return true;
        return false;
    };
    auto best = active.end();
    std::uint32_t bestEnd = 0;
    for (auto it = active.begin(); it != active.end(); ++it) {
        if (it->cls != cls) continue;
        if (requireCalleeSaved && !it->isCalleeSaved) continue;
        if (isExcluded(it->physOrdinal)) continue;
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
    // Cycle 10q closure of 10p substrate: per-opcode implicit
    // clobbers (e.g., x86 idiv/div clobber RDX). One scan, consumed
    // by every range that crosses an implicit-clobber position.
    std::vector<ImplicitClobberAt> const implicitClobbers =
        collectImplicitClobberPositions(lir, schema, flow);

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

        // D-CSUBSET-BINOP-RIGHT-CLOBBER closure (2026-06-02): when
        // this range is the result of a `requires2Address` opcode,
        // the legalize pass will emit `mov result, ops[0]` to
        // satisfy the 2-addr constraint when `result != ops[0]`.
        // That mov CLOBBERS the destination register's prior value
        // — if the allocator assigned `result` to a register that
        // also holds operand[k>=1]'s value, the second source is
        // destroyed before the binary op reads it (`add result,
        // [result, result]` instead of `add result, [result,
        // ops[k]]`). Prevent by EXCLUDING operand[1..N]'s physical
        // registers from this allocation. Operand[0] alias remains
        // permitted (and preferred — the coalesce case where
        // legalize emits no mov at all).
        //
        // Universal across CPUs: the schema's `requires2Address`
        // flag drives the exclusion; no `if (target == X)` branch.
        // Universal across commutativity: the bug fires for both
        // commutative and non-commutative 2-addr ops; both want
        // the same exclusion.
        // Exclusion buffer holds the union of (a) requires2Address
        // operand[1..N] clobber-prevention + (b) cycle-10q implicit-
        // register clobbers from crossed opcodes (e.g., x86 idiv's
        // RDX). Size 8 = 4 (existing 2-addr headroom) + 4 (current
        // max implicit-clobber set on any one op; idiv/div clobber 1
        // each, future mul-1-op clobbers 1, future shift-by-CL
        // clobbers 0). A schema row that pushes the union past 8
        // fails loud via `regallocFatal` in
        // `implicitClobbersCrossedBy` (D-OPT-REGALLOC-EXCLUSION-
        // BUFFER anchored there).
        std::array<std::uint16_t, 8> excludedStorage{};
        std::size_t excludedCount = 0;
        if (LirInstId const producingInst =
                (r.start < flow.positionToInst.size())
                    ? flow.positionToInst[r.start]
                    : LirInstId{};
            producingInst.valid()) {
            auto const opcode = lir.instOpcode(producingInst);
            auto const* info  = schema.opcodeInfo(opcode);
            // HIGH-3 silent-failure fold (2026-06-02): verify the
            // looked-up instruction actually DEFINES `r.vreg`. The
            // liveness builder produces `start = 0` for use-only
            // vregs (a verifier-rejected shape, but defense-in-
            // depth here): `positionToInst[0]` returns the first
            // inst in the function, which is unrelated to r.vreg.
            // Without this check, an unrelated 2-addr op's
            // operands would silently drive the exclusion set and
            // misallocate r.vreg. Skip the exclusion when the
            // looked-up inst isn't this range's definer.
            if (info != nullptr && info->requires2Address
                && lir.instResult(producingInst) == r.vreg) {
                auto const ops = lir.instOperands(producingInst);
                // HIGH-2 silent-failure fold (2026-06-02): fail
                // loud BEFORE the loop instead of silently
                // truncating mid-loop when a future N-ary
                // `requires2Address` op exceeds the fixed-size
                // exclusion buffer. Today's 2-operand binops
                // produce exactly 1 excluded entry (4× headroom);
                // a future schema row with 5+ source operands at
                // indices ≥1 would silently regress the clobber-
                // prevention contract under the prior loop-guard
                // shape. Hard-abort + clear message names the
                // path for the schema author.
                if (ops.size() > excludedStorage.size() + 1u) {
                    regallocFatal(
                        "requires2Address opcode has more source "
                        "operands than the 2-addr exclusion buffer "
                        "can hold — extend excludedStorage in "
                        "lir_regalloc.cpp OR re-shape the schema "
                        "so the high-arity case is encoded as a "
                        "non-2-addr opcode (D-CSUBSET-BINOP-RIGHT-"
                        "CLOBBER buffer overflow)");
                }
                // Skip operand[0] (legitimate coalesce target).
                for (std::size_t k = 1; k < ops.size(); ++k) {
                    if (ops[k].kind != LirOperandKind::Reg) continue;
                    LirReg const opReg = ops[k].reg;
                    // Source reg may be already-physical (e.g. from
                    // an `arg` lowering pre-coalesced to a phys reg)
                    // or a vreg we've assigned earlier in this loop.
                    // LirReg's `id` field holds the ordinal in BOTH
                    // forms — for physical regs the id IS the
                    // physical ordinal; for vregs it's the vreg id
                    // and we route through the assignments table.
                    std::uint16_t ord = 0;
                    if (opReg.isPhysical) {
                        ord = static_cast<std::uint16_t>(opReg.id);
                    } else {
                        if (opReg.id == 0
                            || opReg.id >= out.assignments.size()) {
                            continue;
                        }
                        auto const& a = out.assignments[opReg.id];
                        if (a.isSpilled()) continue;
                        // Skip if the assignment was never set
                        // (default-constructed sentinel has zero
                        // classKind, hence `!valid()`).
                        if (!a.vreg.valid()) continue;
                        ord = static_cast<std::uint16_t>(
                            a.physReg().id);
                    }
                    excludedStorage[excludedCount++] = ord;
                }
            }
        }
        // Augment exclusion with implicit-register clobbers from any
        // opcode this range crosses (cycle 10q substrate consumer).
        // Universal across CPUs — driven entirely by the per-opcode
        // schema declaration; no `if (opcode == idiv)` branch.
        excludedCount = implicitClobbersCrossedBy(
            r, implicitClobbers, excludedStorage, excludedCount);

        std::span<std::uint16_t const> const excluded{
            excludedStorage.data(), excludedCount};

        if (auto pick = tryAllocateExcluding(free, cls, crossesCall, excluded);
            pick.has_value()) {
            LirReg const phys = makePhysicalReg(pick->ordinal, cls);
            out.assignments[r.vreg.id] =
                LirRegAssignment::makePhys(r.vreg, phys);
            active.push_back({r, cls, pick->ordinal, pick->isCalleeSaved});
            continue;
        }

        // Invariant: every spill emits exactly one slot increment via
        // `mintSlot` and contributes to one `SpillStats` counter.
        // The `excluded` set is propagated so the evictee's physical
        // ordinal is never in operand[1..N]'s set — closes the
        // silent-failure HIGH-1 audit fold: without this, the spill
        // fallback could free a register the exclusion explicitly
        // forbids, recreating the clobber bug under register pressure.
        auto const spillIt = findSpillCandidate(active, cls, crossesCall,
                                                 excluded);
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
