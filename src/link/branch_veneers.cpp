#include "link/branch_veneers.hpp"

#include "asm/branch_island.hpp"
#include "asm/format/walker_util.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "link/branch_reloc_geometry.hpp"
#include "link/fresh_symbol_ids.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dss::linker {

namespace {

using dss::report;

void emit(DiagnosticReporter& reporter, DiagnosticCode code, std::string msg) {
    report(reporter, code, DiagnosticSeverity::Error, std::move(msg));
}

// ── WHAT A VENEER IS MADE OF, ELECTED FROM CONFIG ────────────────────────
//
// ★★★ TWO DECLARATIONS HAVE TO AGREE, AND THE ELECTION IS THAT AGREEMENT.
// A veneer is a quoted unconditional-branch body (the assembler's own island
// body — same concept, same query) PLUS a relocation that makes the linker
// write this module's callee address into that body's field. The two are
// declared independently, in the opcode table and in the relocation table, and
// they are matched HERE by their FIELD: the relocation row whose formula writes
// the same bit window, at the same scale, that the quoted body carries.
//
// ⚠ MATCHING BY FIELD RATHER THAN BY NAME IS THE POINT. Picking "the call26
// row" by its spelling would bake one ABI's relocation name into a target-blind
// pass, and — worse — would not notice if the two ever described different
// fields. A row that writes bits 0..25 scaled by 4 is the right row for a body
// whose field is bits 0..25 scaled by 4, whatever either is called.
struct VeneerPlan {
    walker_util::BranchIslandBody body;
    RelocationKind                relocKind{};
    link::BranchRelocGeometry     geometry{};
};

[[nodiscard]] std::optional<VeneerPlan>
electVeneerPlan(TargetSchema const& target) {
    auto const body = asm_island::widestDeclaredIslandBody(target);
    if (!body.declared()) return std::nullopt;
    auto const fg = walker_util::blockRelFieldGeometry(body.kind);
    for (auto const& row : target.relocations()) {
        auto const rg = link::branchRelocGeometry(row.formulaKind);
        if (rg.fieldBits == 0) continue;
        if (rg.fieldBits != fg.width)      continue;
        if (rg.scaleLog2 != fg.scaleLog2)  continue;
        if (rg.lsb != fg.lsb)              continue;
        return VeneerPlan{body, row.kind, rg};
    }
    return std::nullopt;
}

// ── THE LAYOUT EVERY WRITER WILL BUILD ───────────────────────────────────
// `.text` is the functions' bytes, concatenated in order. See the header for
// the measurement behind this and for why PE's weak-definition skip only makes
// the real distances smaller.
[[nodiscard]] std::vector<std::uint64_t>
textStarts(AssembledModule const& module) {
    std::vector<std::uint64_t> starts;
    starts.reserve(module.functions.size() + 1u);
    std::uint64_t at = 0;
    for (auto const& fn : module.functions) {
        starts.push_back(at);
        at += fn.bytes.size();
    }
    starts.push_back(at);  // one past the end — the total text size
    return starts;
}

[[nodiscard]] std::unordered_map<std::uint32_t, std::size_t>
functionIndexBySymbol(AssembledModule const& module) {
    std::unordered_map<std::uint32_t, std::size_t> byId;
    byId.reserve(module.functions.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i)
        byId.emplace(module.functions[i].symbol.v, i);
    return byId;
}

// One branch relocation that is out of reach, located.
struct Overflow {
    std::size_t   funcIndex;
    std::size_t   relocIndex;
    std::uint64_t patchOffset;   // text offset of the field being patched
    std::uint64_t targetOffset;  // text offset of the ULTIMATE callee
    std::uint32_t ultimateSymV;  // the callee this branch is really for
};

[[nodiscard]] bool fitsField(link::BranchRelocGeometry g,
                             std::int64_t              delta) {
    std::int64_t const alignMask = (std::int64_t{1} << g.scaleLog2) - 1;
    if ((delta & alignMask) != 0) return false;
    std::int64_t const disp = delta >> g.scaleLog2;
    return disp >= link::branchRelocFieldMin(g)
        && disp <= link::branchRelocFieldMax(g);
}

}  // namespace

bool branchVeneersNeeded(AssembledModule const& module,
                         TargetSchema const&    target) {
    auto const starts = textStarts(module);
    auto const byId   = functionIndexBySymbol(module);
    for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
        for (auto const& rel : module.functions[fi].relocations) {
            auto const* row = target.relocationInfo(rel.kind);
            if (row == nullptr) continue;  // applyExecRelocations reports it
            auto const g = link::branchRelocGeometry(row->formulaKind);
            if (g.fieldBits == 0) continue;
            auto const tIt = byId.find(rel.target.v);
            if (tIt == byId.end()) continue;  // extern / import: a different tier
            std::int64_t const delta =
                static_cast<std::int64_t>(starts[tIt->second])
              - static_cast<std::int64_t>(starts[fi] + rel.offset);
            if (!fitsField(g, delta)) return true;
        }
    }
    return false;
}

bool injectBranchVeneers(AssembledModule&    module,
                         TargetSchema const& target,
                         DiagnosticReporter& reporter) {
    auto const plan = electVeneerPlan(target);
    if (!plan.has_value()) {
        emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
             std::format(
                 "branch-veneer: a call in this image is beyond its "
                 "relocation field's reach, and target '{}' declares nothing "
                 "to build a veneer from. A veneer is TWO declarations that "
                 "have to agree: an opcode with `terminatorKind: br` carrying "
                 "one self-contained unconditional-branch word (or opcode "
                 "bytes plus a trailing block-relative field), and a "
                 "relocation row whose formula writes THAT SAME field at the "
                 "same bit position and scale. Declare the missing one and "
                 "the election finds it "
                 "(D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER)",
                 std::string{target.name()}));
        return false;
    }

    // Every veneer this pass has minted, and the callee it ULTIMATELY stands in
    // for. The ultimate target is recorded once, at creation, and never
    // rewritten: a veneer may be re-aimed at ANOTHER veneer as the layout
    // grows, and "which call is this chain for" must survive that.
    std::unordered_map<std::uint32_t, std::uint32_t> veneerUltimate;
    std::uint32_t nextSymV = maxExistingSymbolIdV(module) + 1u;

    // ── THE VENEER BOUND, AS ARITHMETIC ─────────────────────────────────
    //   Let R be the field's byte reach and A = R/4 the minimum ground a hop
    //   must take (see `minAdvance` below). The pass only ever aims at a veneer
    //   STRICTLY CLOSER to the callee than the call site is, and only ever
    //   places one that either finishes the chain or advances at least A, so no
    //   chain outlasts the `.text` it crosses. One call's chain therefore holds
    //   at most ceil(N / A) veneers, and over the module's B branch relocations
    //   — B counting the PROGRAM's branches, never the veneers' own:
    //
    //       veneerBound = B * (ceil(N / A) + 1)
    //
    //   The `+ 1` is SLACK, not arithmetic — it is there so a bound re-derived
    //   from a layout that just grew cannot refuse a placement the previous
    //   pass had already justified. Saying which part is the argument and which
    //   is the cushion matters: a comment that states a bound the code does not
    //   compute is the same defect, one level out, that this pass's own
    //   veneer-counting bound already paid for.
    //
    //   Re-evaluated each pass against that pass's N and B, and refused LOUDLY
    //   if exceeded — the same posture `relaxBound` takes one tier down, and
    //   for the same reason: a termination argument true of today's code is not
    //   a guarantee about tomorrow's, and a linker that spins is worse than one
    //   that refuses.
    std::int64_t const reach =
        link::branchRelocFieldMax(plan->geometry) << plan->geometry.scaleLog2;
    // ⓘ TWO DERIVED LENGTHS, EACH A HALVING, EACH WITH A REASON.
    //   `stride`     — how far from the patch site a veneer may stand: HALF the
    //                  reach, leaving the other half as slack against the bytes
    //                  that later veneers add to the layout.
    //   `minAdvance` — how much ground one hop must take to count: half of
    //                  `stride`. A hop that gains four bytes is strictly closer
    //                  and still leaves a chain nobody can finish, so a hop
    //                  either lands within REACH of the callee (finishing the
    //                  chain) or takes at least this much. That is what makes
    //                  the chain length `ceil(distance / minAdvance)` instead of
    //                  unbounded, and it is why the bound below divides by it.
    std::int64_t const stride     = reach / 2 > 0 ? reach / 2 : 1;
    std::int64_t const minAdvance = stride / 2 > 0 ? stride / 2 : 1;

    for (std::uint64_t pass = 0; ; ++pass) {
        auto const starts = textStarts(module);
        auto const byId   = functionIndexBySymbol(module);
        std::uint64_t const textBytes = starts.back();

        auto const ultimateOf = [&](std::uint32_t symV) {
            auto const it = veneerUltimate.find(symV);
            return it == veneerUltimate.end() ? symV : it->second;
        };

        // (1) Find every branch relocation that cannot reach where it is aimed.
        //
        // ⚠ `branchRelocs` COUNTS ONLY THE PROGRAM'S OWN BRANCHES, NOT THE
        // VENEERS'. Each veneer carries a branch relocation of its own, so
        // counting all of them would make the bound grow by one every time a
        // veneer is placed — a bound that chases its own tail is not a bound,
        // and ✔MEASURED it is not a theoretical worry: with the veneers
        // counted, a module the pass could not place a veneer in spun for 315
        // seconds without ever reaching its own refusal.
        std::vector<Overflow> overflows;
        std::uint64_t branchRelocs = 0;
        for (std::size_t fi = 0; fi < module.functions.size(); ++fi) {
            auto const& fn = module.functions[fi];
            bool const isVeneer =
                veneerUltimate.find(fn.symbol.v) != veneerUltimate.end();
            for (std::size_t ri = 0; ri < fn.relocations.size(); ++ri) {
                auto const& rel = fn.relocations[ri];
                auto const* row = target.relocationInfo(rel.kind);
                if (row == nullptr) continue;
                if (link::branchRelocGeometry(row->formulaKind).fieldBits == 0)
                    continue;
                if (!isVeneer) ++branchRelocs;
                auto const aimedIt = byId.find(rel.target.v);
                if (aimedIt == byId.end()) continue;  // extern: another tier
                std::uint64_t const patchOffset = starts[fi] + rel.offset;
                std::int64_t const delta =
                    static_cast<std::int64_t>(starts[aimedIt->second])
                  - static_cast<std::int64_t>(patchOffset);
                if (fitsField(plan->geometry, delta)) continue;
                auto const ultimate = ultimateOf(rel.target.v);
                auto const ultIt    = byId.find(ultimate);
                if (ultIt == byId.end()) continue;
                overflows.push_back(Overflow{fi, ri, patchOffset,
                                             starts[ultIt->second], ultimate});
            }
        }
        if (overflows.empty()) return true;

        // (2) Where every veneer currently stands, by the callee it serves.
        struct Standing { std::uint64_t at; std::uint32_t symV; };
        std::vector<Standing> standing;
        for (auto const& [symV, ultimate] : veneerUltimate) {
            auto const it = byId.find(symV);
            if (it != byId.end())
                standing.push_back(Standing{starts[it->second], symV});
        }

        // (3) Re-aim what can be re-aimed; request a veneer for what cannot.
        struct Request { std::uint64_t desired; std::uint32_t ultimate; };
        std::vector<Request> requests;
        for (auto const& ov : overflows) {
            // ⚠ STRICTLY CLOSER TO THE CALLEE, exactly as the assembler's
            // island rule: it excludes a veneer aiming at itself and makes a
            // chain terminate, because each hop decreases a non-negative
            // integer and therefore cannot cycle.
            std::int64_t bestGap =
                static_cast<std::int64_t>(ov.targetOffset)
                    > static_cast<std::int64_t>(ov.patchOffset)
                ? static_cast<std::int64_t>(ov.targetOffset - ov.patchOffset)
                : static_cast<std::int64_t>(ov.patchOffset - ov.targetOffset);
            std::optional<std::uint32_t> bestSym;
            for (auto const& s : standing) {
                if (ultimateOf(s.symV) != ov.ultimateSymV) continue;
                std::int64_t const gap =
                    static_cast<std::int64_t>(ov.targetOffset) > static_cast<std::int64_t>(s.at)
                        ? static_cast<std::int64_t>(ov.targetOffset - s.at)
                        : static_cast<std::int64_t>(s.at - ov.targetOffset);
                if (gap >= bestGap) continue;
                if (!fitsField(plan->geometry,
                               static_cast<std::int64_t>(s.at)
                             - static_cast<std::int64_t>(ov.patchOffset)))
                    continue;
                bestSym = s.symV;
                bestGap = gap;
            }
            if (bestSym.has_value()) {
                module.functions[ov.funcIndex].relocations[ov.relocIndex]
                    .target = SymbolId{*bestSym};
                continue;
            }
            // ── WHERE A NEW VENEER CAN STAND ────────────────────────────
            //
            // ★★★ A VENEER IS A WHOLE FUNCTION, SO IT CAN ONLY STAND AT A
            // FUNCTION BOUNDARY — and THAT is the difference between this tier
            // and the assembler's. An island goes between two INSTRUCTIONS, so
            // a site always exists; a veneer needs a gap between two
            // FUNCTIONS, and a module can fail to have one in the right place.
            // ✔MEASURED (2026-09-17, the fixture that found it): a module whose
            // caller and callee are separated by ONE function larger than the
            // field's reach has no admissible boundary at all, and the first
            // draft of this pass answered by placing a veneer four bytes
            // further along, forever — 315 seconds without reaching its own
            // bound. The candidate rule below is what makes that case a LOUD
            // REFUSAL naming the real cause, which is that the oversized
            // function is the ASSEMBLER's tier and not this one.
            //
            // A boundary is admissible when it is no further than one STRIDE
            // from the patch site — half the reach, leaving the other half as
            // slack against the bytes later veneers add — AND it either
            //
            //   * lands within REACH of the callee, finishing the chain, or
            //   * advances at least `minAdvance` towards it.
            //
            // ⚠ "ANY PROGRESS AT ALL" IS NOT ENOUGH, AND THAT IS THE PART THE
            // FIRST DRAFT GOT WRONG. A boundary four bytes nearer the callee is
            // strictly closer and still leaves a chain that needs thirty
            // million more of them. Requiring a full stride — except on the
            // hop that finishes — is what makes the chain length
            // `ceil(distance / stride)` rather than unbounded, and it is what
            // turns "this image has no usable boundary" into a refusal instead
            // of a search. Among the admissible ones the CLOSEST TO THE CALLEE
            // wins, so each hop takes as much ground as the field allows.
            std::int64_t const from = static_cast<std::int64_t>(ov.patchOffset);
            std::int64_t const to   = static_cast<std::int64_t>(ov.targetOffset);
            std::int64_t const ownGap = to > from ? to - from : from - to;
            std::optional<std::uint64_t> site;
            std::int64_t siteGap = ownGap;
            for (auto const boundary : starts) {
                auto const at = static_cast<std::int64_t>(boundary);
                std::int64_t const step = at > from ? at - from : from - at;
                if (step > stride) continue;
                std::int64_t const gap = to > at ? to - at : at - to;
                if (gap >= siteGap) continue;
                if (gap > reach && ownGap - gap < minAdvance) continue;
                site    = boundary;
                siteGap = gap;
            }
            if (!site.has_value()) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format(
                         "branch-veneer: a call in function symbol id {} is {} "
                         "bytes from its callee, beyond this relocation "
                         "field's reach, and there is NO FUNCTION BOUNDARY "
                         "between them that a veneer could stand at and make "
                         "progress. A veneer is a whole function, so it can "
                         "only be placed BETWEEN functions; a caller and "
                         "callee separated by a single function larger than "
                         "the field's reach cannot be bridged here. That case "
                         "belongs to the assembler, whose branch islands go "
                         "between INSTRUCTIONS (D-CSUBSET-LONG-BRANCH)",
                         module.functions[ov.funcIndex].symbol.v, ownGap));
                return false;
            }
            requests.push_back(Request{*site, ov.ultimateSymV});
        }
        if (requests.empty()) {
            // Everything that could be re-aimed was, and nothing new is
            // needed — but at least one relocation was out of reach when this
            // pass began, so re-measure it on the next one.
            if (pass > module.functions.size() + 1u) {
                emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                     std::format("branch-veneer: re-aiming stopped making "
                                 "progress with {} branch(es) still out of "
                                 "range — an internal-invariant violation of "
                                 "the veneer placement "
                                 "(D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER)",
                                 overflows.size()));
                return false;
            }
            continue;
        }

        // (4) The bound, evaluated, before anything is inserted.
        std::uint64_t const hops =
            (textBytes + static_cast<std::uint64_t>(minAdvance) - 1u)
            / static_cast<std::uint64_t>(minAdvance);
        std::uint64_t const veneerBound = branchRelocs * (hops + 1u);
        if (veneerUltimate.size() + requests.size() > veneerBound) {
            emit(reporter, DiagnosticCode::K_RelocationKindMismatch,
                 std::format("branch-veneer: placement did not settle — {} "
                             "veneer(s) against a bound of {} ({} branch "
                             "relocation(s) x ceil({} bytes / {} per hop)). The "
                             "set is monotone and each hop advances at least "
                             "one stride, so exceeding this bound is an "
                             "internal-invariant violation, not an oversized "
                             "image "
                             "(D-LK-AARCH64-CALL26-BEYOND-RANGE-HAS-NO-VENEER)",
                             veneerUltimate.size() + requests.size(),
                             veneerBound, branchRelocs, textBytes,
                             minAdvance));
            return false;
        }

        // (5) Insert. Highest desired offset first, so each insertion's index
        //     is still valid when the next one is computed against the
        //     unchanged prefix of the layout.
        // Two calls to one callee from the same region ask for the same
        // boundary; that is ONE veneer, and placing two would be a second
        // answer to a question with one.
        std::sort(requests.begin(), requests.end(),
                  [](Request const& a, Request const& b) {
                      if (a.desired != b.desired) return a.desired > b.desired;
                      return a.ultimate < b.ultimate;
                  });
        requests.erase(std::unique(requests.begin(), requests.end(),
                                   [](Request const& a, Request const& b) {
                                       return a.desired == b.desired
                                           && a.ultimate == b.ultimate;
                                   }),
                       requests.end());
        // ⚠ RESERVE BEFORE INSERTING, AND NOT FOR SPEED. A `std::vector`
        // reallocation moves its elements only when their move constructor is
        // `noexcept`; otherwise it COPIES them for the strong guarantee — and
        // an `AssembledFunction` owns the function's whole byte vector, so a
        // copy here would duplicate the entire image's text. Reserving the
        // final size makes every insertion a tail shift by move-ASSIGNMENT,
        // which steals buffers unconditionally.
        module.functions.reserve(module.functions.size() + requests.size());
        for (auto const& req : requests) {
            // The function boundary nearest the desired offset: a veneer is a
            // whole function, so it can only be placed BETWEEN functions.
            std::size_t insertAt = module.functions.size();
            std::uint64_t bestDelta = ~std::uint64_t{0};
            for (std::size_t i = 0; i < starts.size(); ++i) {
                std::uint64_t const d = starts[i] > req.desired
                                      ? starts[i] - req.desired
                                      : req.desired - starts[i];
                if (d < bestDelta) { bestDelta = d; insertAt = i; }
            }
            if (insertAt > module.functions.size())
                insertAt = module.functions.size();

            AssembledFunction veneer;
            veneer.symbol = SymbolId{nextSymV};
            veneer.bytes.assign(plan->body.bytes.begin(),
                                plan->body.bytes.begin() + plan->body.byteCount);
            veneer.relocations.push_back(Relocation{
                plan->body.fieldOffset, SymbolId{req.ultimate},
                plan->relocKind, 0});
            veneerUltimate.emplace(nextSymV, req.ultimate);
            ++nextSymV;

            module.functions.insert(
                module.functions.begin()
                    + static_cast<std::ptrdiff_t>(insertAt),
                std::move(veneer));
            ++module.expectedFuncCount;
            // ⚠ `imageEntryOverride` IS AN INDEX, NOT A SYMBOL. The entry
            // trampoline prepends itself and records index 0; a veneer
            // inserted at or before that index moves the entry without moving
            // the number, which would make the image start executing a veneer.
            if (module.imageEntryOverride.has_value()
                && *module.imageEntryOverride >= insertAt)
                module.imageEntryOverride = *module.imageEntryOverride + 1u;
        }
        // The layout moved; measure it again from scratch.
    }
}

} // namespace dss::linker
