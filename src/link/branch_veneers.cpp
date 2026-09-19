#include "link/branch_veneers.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/fresh_symbol_ids.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lir_reg.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::linker {

namespace {

using dss::report;

void emit(DiagnosticReporter& reporter, std::string msg) {
    report(reporter, DiagnosticCode::K_RelocationKindMismatch,
           DiagnosticSeverity::Error, std::move(msg));
}

[[nodiscard]] bool inWindow(link::RelocReach r, std::int64_t d) noexcept {
    return d >= r.minDelta && d <= r.maxDelta;
}

// How far a window reaches on its NARROWER side — the distance a target may
// lie in EITHER direction. Written to survive an unbounded window
// (`[INT64_MIN, INT64_MAX]`, a body with no PC-relative field), whose negation
// would overflow.
[[nodiscard]] std::int64_t symmetricReach(link::RelocReach r) noexcept {
    std::int64_t const back = r.minDelta == std::numeric_limits<std::int64_t>::min()
                                  ? std::numeric_limits<std::int64_t>::max()
                                  : -r.minDelta;
    return std::min(r.maxDelta, back);
}

// ── THE MODEL, BUILT FROM A MODULE ─────────────────────────────────────
//
// One site per relocation whose row writes a BOUNDED BRANCH FIELD — a formula
// with a branch geometry (`call26` on arm64; x86-64 declares none, because its
// `rel32` is a plain PC-relative datum that no reference extends). The target
// is a function of the module, or an import the writer gives a call stub; a
// branch to anything else (a data item, a writer-reserved symbol) is left to
// the relocation applier, which measures it exactly and refuses it by name.
struct SiteRef {
    std::uint32_t function;
    std::uint32_t relocation;
};

struct CollectedLayout {
    VeneerLayout                layout;
    std::vector<SiteRef>        refs;          // parallel to layout.sites
    std::vector<SymbolId>       targetSymbol;  // parallel to layout.targets
    std::vector<RelocationKind> reachKind;     // parallel to layout.reaches
};

[[nodiscard]] CollectedLayout
collectLayout(AssembledModule const&            module,
              TargetSchema const&               target,
              link::ImportCallStubLayout const& stubs) {
    CollectedLayout out;
    auto& L = out.layout;
    std::size_t const n = module.functions.size();
    L.functionSizes.reserve(n);
    std::unordered_map<std::uint32_t, std::uint32_t> fnBySymbol;
    fnBySymbol.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        L.functionSizes.push_back(module.functions[i].bytes.size());
        fnBySymbol.emplace(module.functions[i].symbol.v,
                           static_cast<std::uint32_t>(i));
    }
    auto const* vocab = target.linkVeneers();

    // Targets are keyed by (symbol, addend): a branch to `f` and one to `f+8`
    // need different veneers — ld.lld keys its thunks the same way (`getThunk`:
    // symbol or section+offset, plus addend).
    struct TargetKey {
        std::uint32_t symbol;
        std::int64_t  addend;
        bool operator==(TargetKey const&) const = default;
    };
    struct TargetKeyHash {
        std::size_t operator()(TargetKey const& k) const noexcept {
            return std::hash<std::uint64_t>{}(
                (static_cast<std::uint64_t>(k.symbol) << 32)
                ^ static_cast<std::uint64_t>(k.addend));
        }
    };
    std::unordered_map<TargetKey, std::uint32_t, TargetKeyHash> targetIndex;
    std::unordered_map<std::uint32_t, std::uint32_t> reachIndex;  // kind.v -> reach

    for (std::size_t fi = 0; fi < n; ++fi) {
        auto const& fn = module.functions[fi];
        std::size_t const firstSite = L.sites.size();
        for (std::size_t ri = 0; ri < fn.relocations.size(); ++ri) {
            auto const& rel = fn.relocations[ri];
            auto const* row = target.relocationInfo(rel.kind);
            if (row == nullptr) continue;  // the linker's unifier reports it
            if (link::branchRelocGeometry(row->formulaKind).fieldBits == 0)
                continue;                  // not a bounded branch field
            auto const reach = link::relocFieldReach(*row);
            if (!reach.has_value()) continue;

            VeneerLayoutTarget t;
            t.addend = rel.addend;
            if (auto const it = fnBySymbol.find(rel.target.v);
                it != fnBySymbol.end()) {
                t.function = it->second;
            } else if (auto const st = stubs.maxPastTextEnd.find(rel.target);
                       st != stubs.maxPastTextEnd.end()) {
                t.pastTextEnd = st->second;
            } else {
                continue;  // neither a function nor a call stub: the applier's
            }
            auto const [tIt, tNew] = targetIndex.emplace(
                TargetKey{rel.target.v, rel.addend},
                static_cast<std::uint32_t>(L.targets.size()));
            if (tNew) {
                L.targets.push_back(t);
                out.targetSymbol.push_back(rel.target);
            }
            auto const [rIt, rNew] = reachIndex.emplace(
                rel.kind.v, static_cast<std::uint32_t>(L.reaches.size()));
            if (rNew) {
                L.reaches.push_back(*reach);
                out.reachKind.push_back(rel.kind);
            }
            VeneerLayoutSite s;
            s.function = static_cast<std::uint32_t>(fi);
            s.offset   = rel.offset;
            s.target   = tIt->second;
            s.reach    = rIt->second;
            s.routable = vocab != nullptr
                      && std::find(vocab->routableRelocations.begin(),
                                   vocab->routableRelocations.end(), rel.kind)
                         != vocab->routableRelocations.end();
            L.sites.push_back(s);
            out.refs.push_back(SiteRef{static_cast<std::uint32_t>(fi),
                                       static_cast<std::uint32_t>(ri)});
        }
        // The sweep needs text order. The assembler appends relocations in
        // emission order, so this is a CHECK that normally finds nothing to do;
        // a function that does arrive out of order is sorted locally.
        bool sorted = true;
        for (std::size_t k = firstSite + 1; k < L.sites.size(); ++k) {
            if (L.sites[k].offset < L.sites[k - 1].offset) { sorted = false; break; }
        }
        if (!sorted) {
            std::vector<std::size_t> order(L.sites.size() - firstSite);
            for (std::size_t k = 0; k < order.size(); ++k) order[k] = firstSite + k;
            std::stable_sort(order.begin(), order.end(),
                             [&](std::size_t a, std::size_t b) {
                                 return L.sites[a].offset < L.sites[b].offset;
                             });
            std::vector<VeneerLayoutSite> sitesSorted;
            std::vector<SiteRef>          refsSorted;
            for (auto const k : order) {
                sitesSorted.push_back(L.sites[k]);
                refsSorted.push_back(out.refs[k]);
            }
            std::copy(sitesSorted.begin(), sitesSorted.end(),
                      L.sites.begin() + static_cast<std::ptrdiff_t>(firstSite));
            std::copy(refsSorted.begin(), refsSorted.end(),
                      out.refs.begin() + static_cast<std::ptrdiff_t>(firstSite));
        }
    }
    return out;
}

// ── THE BODY, ASSEMBLED ONCE FROM THE DECLARED SEQUENCE ────────────────
//
// ★★ BUILT, NOT QUOTED — AND BUILT BY THE SAME ASSEMBLER THAT BUILDS
// EVERYTHING ELSE. The sequence names the target's own opcodes; the scratch
// register and the target symbol are the only operands a step may name; the
// result is a TEMPLATE whose relocations aim at a placeholder symbol and are
// re-aimed per veneer. The entry trampoline mints a function at link time the
// same way (`entry_trampoline.cpp`).
//
// ★★ AND THE TWO DECLARATIONS MUST AGREE. Each step also declares the
// relocations its encoding carries, and the assembled template is checked
// against that list: an opcode row edited so that `lea` stopped emitting its
// page relocation would otherwise turn every veneer into a jump to page 0.
constexpr std::uint32_t kTemplateSymbolV = 1;

struct ElectedBody {
    std::string               name;
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation>   relocations;  // target == kTemplateSymbolV
    link::RelocReach          reach{};      // (target - veneer start) the body encodes
};

// The shape rules `TargetSchemaData::validate()` enforces at load, re-checked
// here because a schema built in memory never passed through it — and the
// builder below aborts the process on a malformed sequence, which is a worse
// outcome than a diagnostic.
[[nodiscard]] std::string bodyShapeProblem(LinkVeneerBody const& body,
                                           TargetSchema const&   target) {
    if (body.sequence.empty()) return "its sequence is empty";
    for (std::size_t i = 0; i < body.sequence.size(); ++i) {
        auto const& step = body.sequence[i];
        auto const* info = target.opcodeInfo(step.opcode);
        if (step.opcode == 0 || info == nullptr)
            return std::format("step {} ('{}') names no opcode of this target",
                               i, step.mnemonic);
        if (info->isCall)
            return std::format("step {} ('{}') is a CALL, which overwrites the "
                               "return address a veneer must preserve", i,
                               step.mnemonic);
        bool const last = i + 1 == body.sequence.size();
        if (last && info->terminatorKind != TargetTerminatorKind::IndirectBr)
            return std::format("its last step ('{}') is not an indirect branch "
                               "(terminatorKind: indirect-br)", step.mnemonic);
        if (!last && info->isTerminator())
            return std::format("step {} ('{}') is a terminator before the last "
                               "step", i, step.mnemonic);
    }
    return {};
}

[[nodiscard]] std::optional<ElectedBody>
assembleBody(LinkVeneerBody const&       body,
             LinkVeneerVocabulary const& vocab,
             TargetSchema const&         target,
             DiagnosticReporter&         reporter) {
    if (auto const problem = bodyShapeProblem(body, target); !problem.empty()) {
        emit(reporter, std::format(
            "branch-veneer: target '{}' declares veneer body '{}', but {} — a "
            "veneer is a sequence that ends in the one indirect branch leaving it "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            std::string{target.name()}, body.name, problem));
        return std::nullopt;
    }
    auto const* scratchInfo = vocab.scratchRegisters.empty()
                                  ? nullptr
                                  : target.registerInfo(vocab.scratchRegisters.front());
    if (scratchInfo == nullptr) {
        emit(reporter, std::format(
            "branch-veneer: target '{}' declares veneer body '{}' but no scratch "
            "register resolves — a linker may clobber only what the ABI grants, "
            "so the grant must be declared "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            std::string{target.name()}, body.name));
        return std::nullopt;
    }
    LirReg const scratch = makePhysicalReg(
        vocab.scratchRegisters.front(),
        static_cast<LirRegClass>(scratchInfo->regClass));

    LirBuilder b{target};
    (void)b.addFunction(SymbolId{kTemplateSymbolV});
    auto const blk = b.createBlock();
    b.beginBlock(blk);
    for (std::size_t i = 0; i < body.sequence.size(); ++i) {
        auto const& step = body.sequence[i];
        std::vector<LirOperand> ops;
        ops.reserve(step.operands.size());
        for (auto const role : step.operands) {
            ops.push_back(role == LinkVeneerOperandRole::Scratch
                              ? LirOperand::makeReg(scratch)
                              : LirOperand::makeSymbolRef(kTemplateSymbolV));
        }
        if (i + 1 == body.sequence.size()) {
            // The branch LEAVES the function, so it has no in-function
            // successor; the assembler never reads successors.
            (void)b.addIndirectBr(step.opcode, ops, {});
        } else {
            (void)b.addInst(step.opcode,
                            step.resultIsScratch ? scratch : InvalidLirReg, ops);
        }
    }
    Lir lir = std::move(b).finish();
    std::vector<MirInstId> lirToMir(lir.instCount());
    std::size_t const errorsBefore = reporter.errorCount();
    AssembledModule am = assemble(lir, target, lirToMir, reporter);
    if (reporter.errorCount() != errorsBefore || am.functions.size() != 1
        || am.functions[0].bytes.empty()) {
        emit(reporter, std::format(
            "branch-veneer: veneer body '{}' of target '{}' did not assemble (see "
            "the diagnostic above), so nothing can be built to carry an "
            "out-of-reach branch "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            body.name, std::string{target.name()}));
        return std::nullopt;
    }
    auto& fn = am.functions[0];

    // The declared relocations, in emission order, against the assembled ones.
    std::vector<RelocationKind> declared;
    for (auto const& step : body.sequence)
        declared.insert(declared.end(), step.relocations.begin(),
                        step.relocations.end());
    bool agree = declared.size() == fn.relocations.size();
    for (std::size_t k = 0; agree && k < declared.size(); ++k) {
        agree = fn.relocations[k].kind == declared[k]
             && fn.relocations[k].target.v == kTemplateSymbolV
             && fn.relocations[k].addend == 0;
    }
    if (!agree) {
        auto const names = [&](std::vector<RelocationKind> const& kinds) {
            std::string s;
            for (auto const k : kinds) {
                auto const* row = target.relocationInfo(k);
                if (!s.empty()) s += ", ";
                s += row != nullptr ? row->name : std::to_string(k.v);
            }
            return s.empty() ? std::string{"none"} : s;
        };
        std::vector<RelocationKind> got;
        got.reserve(fn.relocations.size());
        for (auto const& r : fn.relocations) got.push_back(r.kind);
        emit(reporter, std::format(
            "branch-veneer: veneer body '{}' of target '{}' declares the "
            "relocations [{}] but its opcodes assembled [{}]. The two "
            "declarations have to agree, or every veneer would be patched through "
            "fields nobody declared "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            body.name, std::string{target.name()}, names(declared), names(got)));
        return std::nullopt;
    }

    // The body's reach: the window EVERY one of its PC-relative fields can
    // encode, measured from the veneer's own start. An absolute field (the
    // low-12 half of the pair) is not bounded by distance and adds nothing.
    link::RelocReach reach{std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max()};
    for (auto const& r : fn.relocations) {
        auto const* row = target.relocationInfo(r.kind);
        if (row == nullptr) continue;
        auto const w = link::relocFieldReach(*row);
        if (!w.has_value()) continue;
        auto const off = static_cast<std::int64_t>(r.offset);
        reach.minDelta = std::max(reach.minDelta, w->minDelta + off);
        reach.maxDelta = std::min(reach.maxDelta, w->maxDelta + off);
    }
    ElectedBody elected;
    elected.name        = body.name;
    elected.bytes       = std::move(fn.bytes);
    elected.relocations = std::move(fn.relocations);
    elected.reach       = reach;
    return elected;
}

}  // namespace

// ── THE PLANNER ────────────────────────────────────────────────────────

VeneerPlan planBranchVeneers(VeneerLayout const& in) {
    VeneerPlan plan;
    std::size_t const n = in.functionSizes.size();
    plan.siteVeneer.assign(in.sites.size(), -1);

    // A model that does not describe a layout is an internal-invariant breach,
    // never something to plan around.
    for (std::size_t si = 0; si < in.sites.size(); ++si) {
        auto const& s = in.sites[si];
        bool const bad = s.function >= n || s.target >= in.targets.size()
                      || s.reach >= in.reaches.size()
                      || s.offset >= in.functionSizes[s.function]
                      || (si > 0 && (s.function < in.sites[si - 1].function
                                     || (s.function == in.sites[si - 1].function
                                         && s.offset < in.sites[si - 1].offset)));
        if (bad) {
            plan.status     = VeneerPlanStatus::Malformed;
            plan.failedSite = static_cast<std::uint32_t>(si);
            return plan;
        }
    }
    for (auto const& t : in.targets) {
        if (t.function != kVeneerTargetIsStub && t.function >= n) {
            plan.status = VeneerPlanStatus::Malformed;
            return plan;
        }
    }

    std::vector<std::uint64_t> start(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i) start[i + 1] = start[i] + in.functionSizes[i];
    std::uint64_t const textEnd = start[n];
    auto const posOf = [&](VeneerLayoutTarget const& t) -> std::int64_t {
        std::uint64_t const base = t.function == kVeneerTargetIsStub
                                       ? textEnd + t.pastTextEnd
                                       : start[t.function];
        return static_cast<std::int64_t>(base) + t.addend;
    };
    auto const siteAt = [&](VeneerLayoutSite const& s) -> std::int64_t {
        return static_cast<std::int64_t>(start[s.function] + s.offset);
    };

    // (1) THE MARGIN. Only a site farther than half its reach can ever receive
    //     a veneer, so the F far ones together add at most F × veneerSize bytes
    //     anywhere in the image. Taking every decision against the reach minus
    //     that much makes each one immune to every insertion that follows it.
    //     Past half the narrowest reach the margin is CAPPED there, and the
    //     exact check the caller runs on the plan decides (see the header).
    std::int64_t halfMin = std::numeric_limits<std::int64_t>::max();
    for (auto const& r : in.reaches) halfMin = std::min(halfMin, symmetricReach(r) / 2);
    std::uint64_t far = 0;
    for (auto const& s : in.sites) {
        std::int64_t const d = posOf(in.targets[s.target]) - siteAt(s);
        std::int64_t const h = symmetricReach(in.reaches[s.reach]) / 2;
        if (d > h || d < -h) ++far;
    }
    plan.work.farSites = far;
    if (far == 0) return plan;  // nothing can be out of reach
    if (halfMin <= 0 || in.veneerSize == 0) {
        plan.status = VeneerPlanStatus::Malformed;
        return plan;
    }
    std::uint64_t margin = far * in.veneerSize;
    if (margin > static_cast<std::uint64_t>(halfMin)) {
        margin = static_cast<std::uint64_t>(halfMin);
        plan.marginCapped = true;
    }
    plan.margin = margin;
    auto const m = static_cast<std::int64_t>(margin);
    link::RelocReach const bodyWindow{
        in.veneerReach.minDelta == std::numeric_limits<std::int64_t>::min()
            ? in.veneerReach.minDelta : in.veneerReach.minDelta + m,
        in.veneerReach.maxDelta == std::numeric_limits<std::int64_t>::max()
            ? in.veneerReach.maxDelta : in.veneerReach.maxDelta - m};

    // (2) THE SWEEP. One visit per site, in text order.
    std::vector<std::int32_t>  latest(in.targets.size(), -1);  // highest-boundary veneer per target
    std::vector<std::uint32_t> ahead(in.reaches.size(), 0);   // monotone pointer per reach class
    for (std::size_t si = 0; si < in.sites.size(); ++si) {
        auto const& s = in.sites[si];
        ++plan.work.sitesExamined;
        std::int64_t const c = siteAt(s);
        auto const& tgt      = in.targets[s.target];
        std::int64_t const d = posOf(tgt) - c;
        auto const& R        = in.reaches[s.reach];
        link::RelocReach const w{R.minDelta + m, R.maxDelta - m};
        if (inWindow(w, d)) continue;  // reaches directly, and will after insertion
        if (!s.routable) {
            plan.status      = VeneerPlanStatus::NotRoutable;
            plan.failedSite  = static_cast<std::uint32_t>(si);
            plan.failedDelta = d;
            return plan;
        }
        // REUSE the veneer this target already has, if it is in reach.
        if (std::int32_t const v = latest[s.target]; v >= 0) {
            auto const vpos = static_cast<std::int64_t>(
                start[plan.veneers[static_cast<std::size_t>(v)].boundary]);
            if (inWindow(w, vpos - c)) {
                plan.siteVeneer[si] = v;
                ++plan.work.veneersReused;
                continue;
            }
        }
        // The FURTHEST boundary AHEAD within reach — ld64.lld's rule, and the
        // greedy choice that serves the most later sites. The pointer only moves
        // forward, because sites arrive in text order.
        auto& p = ahead[s.reach];
        while (p < n && static_cast<std::int64_t>(start[p + 1]) - c <= w.maxDelta) {
            ++p;
            ++plan.work.pointerAdvances;
        }
        std::uint32_t boundary = 0;
        if (p > s.function) {
            boundary = p;
        } else if (static_cast<std::int64_t>(start[s.function]) - c >= w.minDelta) {
            // None ahead: the boundary BEHIND the branch, its own function's
            // start — ld.lld's fallback (✔MEASURED: it links a call at the top
            // of a function longer than the reach; GNU ld and ld64.lld refuse).
            boundary = s.function;
            ++plan.work.behindPlacements;
        } else {
            plan.status      = VeneerPlanStatus::NoBoundaryInReach;
            plan.failedSite  = static_cast<std::uint32_t>(si);
            plan.failedDelta = d;
            return plan;
        }
        std::int64_t const vd = posOf(tgt) - static_cast<std::int64_t>(start[boundary]);
        if (!inWindow(bodyWindow, vd)) {
            plan.status      = VeneerPlanStatus::BodyOutOfReach;
            plan.failedSite  = static_cast<std::uint32_t>(si);
            plan.failedDelta = vd;
            return plan;
        }
        auto const idx = static_cast<std::int32_t>(plan.veneers.size());
        plan.veneers.push_back(PlannedVeneer{boundary, s.target});
        plan.siteVeneer[si] = idx;
        ++plan.work.veneersPlaced;
        if (latest[s.target] < 0
            || plan.veneers[static_cast<std::size_t>(latest[s.target])].boundary
                   <= boundary)
            latest[s.target] = idx;
    }
    return plan;
}

std::optional<VeneerMisfit>
findVeneerPlanMisfit(VeneerLayout const& in, VeneerPlan const& plan,
                     BranchVeneerWork* work) {
    std::size_t const n = in.functionSizes.size();
    std::vector<std::uint64_t> islandCount(n + 1, 0);
    for (auto const& v : plan.veneers) ++islandCount[v.boundary];
    // islandBase[b]: where boundary b's island begins; fstart[k]: function k's
    // final start, after its own boundary's island.
    std::vector<std::uint64_t> islandBase(n + 1, 0), fstart(n + 1, 0);
    std::uint64_t before = 0, inserted = 0;
    for (std::size_t b = 0; b <= n; ++b) {
        islandBase[b] = before + inserted;
        inserted += islandCount[b] * in.veneerSize;
        fstart[b] = before + inserted;
        if (b < n) before += in.functionSizes[b];
    }
    std::uint64_t const textEnd = fstart[n];
    std::vector<std::uint64_t> slot(n + 1, 0);
    std::vector<std::uint64_t> vpos(plan.veneers.size(), 0);
    for (std::size_t i = 0; i < plan.veneers.size(); ++i) {
        auto const b = plan.veneers[i].boundary;
        vpos[i] = islandBase[b] + in.veneerSize * slot[b]++;
    }
    auto const posOf = [&](VeneerLayoutTarget const& t) -> std::int64_t {
        std::uint64_t const base = t.function == kVeneerTargetIsStub
                                       ? textEnd + t.pastTextEnd
                                       : fstart[t.function];
        return static_cast<std::int64_t>(base) + t.addend;
    };
    for (std::size_t si = 0; si < in.sites.size(); ++si) {
        auto const& s = in.sites[si];
        if (work != nullptr) ++work->sitesVerified;
        auto const c = static_cast<std::int64_t>(fstart[s.function] + s.offset);
        std::int32_t const v = si < plan.siteVeneer.size() ? plan.siteVeneer[si] : -1;
        std::int64_t const aim = v < 0 ? posOf(in.targets[s.target])
                                       : static_cast<std::int64_t>(
                                             vpos[static_cast<std::size_t>(v)]);
        if (!inWindow(in.reaches[s.reach], aim - c))
            return VeneerMisfit{false, static_cast<std::uint32_t>(si), aim - c};
    }
    for (std::size_t i = 0; i < plan.veneers.size(); ++i) {
        if (work != nullptr) ++work->sitesVerified;
        std::int64_t const d = posOf(in.targets[plan.veneers[i].target])
                             - static_cast<std::int64_t>(vpos[i]);
        if (!inWindow(in.veneerReach, d))
            return VeneerMisfit{true, static_cast<std::uint32_t>(i), d};
    }
    return std::nullopt;
}

bool branchVeneersNeeded(AssembledModule const&            module,
                         TargetSchema const&               target,
                         link::ImportCallStubLayout const& stubs) {
    auto const collected = collectLayout(module, target, stubs);
    VeneerPlan none;
    none.siteVeneer.assign(collected.layout.sites.size(), -1);
    return findVeneerPlanMisfit(collected.layout, none).has_value();
}

bool injectBranchVeneers(AssembledModule&                  module,
                         TargetSchema const&               target,
                         link::ImportCallStubLayout const& stubs,
                         DiagnosticReporter&               reporter,
                         BranchVeneerWork*                 work) {
    auto collected = collectLayout(module, target, stubs);
    auto& L = collected.layout;
    auto const describeSite = [&](std::size_t si, std::int64_t d) {
        auto const& ref = collected.refs[si];
        auto const& rel = module.functions[ref.function].relocations[ref.relocation];
        auto const* row = target.relocationInfo(rel.kind);
        auto const& R   = L.reaches[L.sites[si].reach];
        return std::format(
            "relocation '{}' at byte {} of function symbol id {} is {} bytes from "
            "its target (symbol id {}), beyond the field's reach of [{}, {}] bytes",
            row != nullptr ? row->name : std::string{"?"}, rel.offset,
            module.functions[ref.function].symbol.v, d, rel.target.v,
            R.minDelta, R.maxDelta);
    };

    // Nothing out of reach on the layout as it stands: nothing to do.
    VeneerPlan none;
    none.siteVeneer.assign(L.sites.size(), -1);
    auto const initialMisfit = findVeneerPlanMisfit(L, none);
    if (!initialMisfit.has_value()) return true;

    auto const* vocab = target.linkVeneers();
    if (vocab == nullptr) {
        emit(reporter, std::format(
            "branch-veneer: {}, and target '{}' declares no `linkVeneers` "
            "vocabulary: nothing states which register a linker may clobber at "
            "this branch or what a veneer is made of. That is an ABI contract "
            "(on AArch64, AAPCS64 grants a linker IP0, IP1 and the flags), so a "
            "target DECLARES it; it is never inferred from a register's name "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            describeSite(initialMisfit->index, initialMisfit->delta),
            std::string{target.name()}));
        return false;
    }

    // ── ELECT THE BODY: the first declared (cheapest) whose reach covers the
    //    whole image; failing that the widest, and the planner then decides
    //    veneer by veneer whether it reaches.
    std::uint64_t maxPast = 0;
    for (auto const& t : L.targets)
        if (t.function == kVeneerTargetIsStub) maxPast = std::max(maxPast, t.pastTextEnd);
    std::uint64_t textSize = 0;
    for (auto const s : L.functionSizes) textSize += s;
    std::optional<ElectedBody> elected;
    for (auto const& body : vocab->bodies) {
        auto candidate = assembleBody(body, *vocab, target, reporter);
        if (!candidate.has_value()) return false;
        auto const span = static_cast<std::int64_t>(
            textSize + maxPast + L.sites.size() * candidate->bytes.size());
        bool const covers = symmetricReach(candidate->reach) >= span;
        if (!elected.has_value()
            || symmetricReach(candidate->reach) > symmetricReach(elected->reach))
            elected = std::move(candidate);
        if (covers) break;
    }
    if (!elected.has_value()) {
        emit(reporter, std::format(
            "branch-veneer: target '{}' declares a `linkVeneers` vocabulary with "
            "no body "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            std::string{target.name()}));
        return false;
    }
    // A veneer is inserted between functions, so its size must keep every
    // following function on the branch fields' own instruction alignment.
    for (std::size_t r = 0; r < L.reaches.size(); ++r) {
        auto const* row = target.relocationInfo(collected.reachKind[r]);
        auto const g = link::branchRelocGeometry(row->formulaKind);
        std::size_t const align = std::size_t{1} << g.scaleLog2;
        if (elected->bytes.size() % align != 0) {
            emit(reporter, std::format(
                "branch-veneer: veneer body '{}' is {} bytes, which is not a "
                "multiple of relocation '{}''s {}-byte instruction alignment, so "
                "inserting it would misalign every function after it "
                "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
                elected->name, elected->bytes.size(), row->name, align));
            return false;
        }
    }
    L.veneerSize  = elected->bytes.size();
    L.veneerReach = elected->reach;

    // ── PLAN, THEN MEASURE THE PLAN EXACTLY ──────────────────────────
    VeneerPlan plan = planBranchVeneers(L);
    if (work != nullptr) *work = plan.work;
    switch (plan.status) {
        case VeneerPlanStatus::Ok: break;
        case VeneerPlanStatus::NotRoutable: {
            auto const& ref = collected.refs[plan.failedSite];
            auto const* row = target.relocationInfo(
                module.functions[ref.function].relocations[ref.relocation].kind);
            emit(reporter, std::format(
                "branch-veneer: {}, and target '{}' does not list relocation '{}' "
                "in `linkVeneers.routableRelocations`. The ABI lets a linker route "
                "only call- and jump-class branches through a veneer, and GNU ld "
                "2.42 and ld.lld 18.1.3 both refuse an out-of-reach conditional "
                "branch (MEASURED: 'relocation truncated to fit' / 'out of range'), "
                "as DSS does "
                "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
                describeSite(plan.failedSite, plan.failedDelta),
                std::string{target.name()},
                row != nullptr ? row->name : std::string{"?"}));
            return false;
        }
        case VeneerPlanStatus::NoBoundaryInReach: {
            auto const& s = L.sites[plan.failedSite];
            emit(reporter, std::format(
                "branch-veneer: {}, and NO FUNCTION BOUNDARY stands within that "
                "reach of the branch on either side: the function holding it is "
                "{} bytes long and the branch sits {} bytes from its start. A "
                "veneer is a whole function, so it can only stand BETWEEN "
                "functions, and no veneer body of any shape changes that. Every "
                "reference refuses this shape too (MEASURED: GNU ld 2.42 "
                "'relocation truncated to fit'; ld.lld 18.1.3 'InputSection too "
                "large for range extension thunk') "
                "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
                describeSite(plan.failedSite, plan.failedDelta),
                L.functionSizes[s.function], s.offset));
            return false;
        }
        case VeneerPlanStatus::BodyOutOfReach:
            emit(reporter, std::format(
                "branch-veneer: {}, and veneer body '{}', the widest this target "
                "declares, encodes only [{}, {}] bytes from where the veneer must "
                "stand while its target is {} bytes away. ld.lld 18.1.3 refuses "
                "the same span in PIE and shared output (MEASURED: 'relocation "
                "R_AARCH64_ADR_PREL_PG_HI21 out of range'); GNU ld 2.42 links it "
                "with a PC-relative literal body (ldr x16, lit; adr x17, 0; add "
                "x16, x16, x17; br x16; .xword), which needs a 64-bit PC-relative "
                "data relocation. Declaring one and a body built on it is what "
                "goes further "
                "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
                describeSite(plan.failedSite, plan.failedDelta), elected->name,
                elected->reach.minDelta, elected->reach.maxDelta,
                plan.failedDelta));
            return false;
        case VeneerPlanStatus::Malformed:
            emit(reporter, std::format(
                "branch-veneer: the placement model is inconsistent at site {} — "
                "an internal-invariant violation of the veneer pass "
                "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
                plan.failedSite));
            return false;
    }
    if (auto const misfit = findVeneerPlanMisfit(L, plan, work)) {
        emit(reporter, std::format(
            "branch-veneer: after placing {} veneer(s) a field does not reach "
            "({} {}, {} bytes). {} "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            plan.veneers.size(), misfit->isVeneer ? "veneer" : "site",
            misfit->index, misfit->delta,
            plan.marginCapped
                ? std::format("This image has {} branches farther than half their "
                              "reach, so the one-pass margin was capped at {} "
                              "bytes and the veneers it placed outgrew it; this "
                              "pass refuses that regime rather than iterating",
                              plan.work.farSites, plan.margin)
                : std::string{"The one-pass margin covered every byte the veneers "
                              "could add, so this is an internal-invariant "
                              "violation of the placement arithmetic"}));
        return false;
    }
    if (plan.veneers.empty()) return true;

    // ── MATERIALIZE, IN ONE MERGE ─────────────────────────────────────
    std::uint32_t const maxV = maxExistingSymbolIdV(module);
    if (static_cast<std::uint64_t>(maxV) + plan.veneers.size()
        > std::numeric_limits<std::uint32_t>::max()) {
        emit(reporter, std::format(
            "branch-veneer: SymbolId space exhausted: the module's max SymbolId is "
            "{} and {} veneer(s) need fresh ids "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            maxV, plan.veneers.size()));
        return false;
    }
    std::uint32_t const firstId = maxV + 1u;

    // Re-aim every routed branch at its veneer. The addend moves INTO the
    // veneer, which aims at target+addend; the branch aims at the veneer's start.
    for (std::size_t si = 0; si < L.sites.size(); ++si) {
        std::int32_t const v = plan.siteVeneer[si];
        if (v < 0) continue;
        auto const& ref = collected.refs[si];
        auto& rel = module.functions[ref.function].relocations[ref.relocation];
        rel.target = SymbolId{firstId + static_cast<std::uint32_t>(v)};
        rel.addend = 0;
    }

    // Bucket the veneers by boundary — stable, so the order within an island is
    // the placement order the exact check measured.
    std::size_t const n = module.functions.size();
    std::vector<std::uint32_t> firstAt(n + 2, 0);
    for (auto const& v : plan.veneers) ++firstAt[v.boundary + 1];
    for (std::size_t b = 1; b < firstAt.size(); ++b) firstAt[b] += firstAt[b - 1];
    std::vector<std::uint32_t> order(plan.veneers.size());
    {
        auto fill = firstAt;
        for (std::size_t i = 0; i < plan.veneers.size(); ++i)
            order[fill[plan.veneers[i].boundary]++] = static_cast<std::uint32_t>(i);
    }

    auto const makeVeneer = [&](std::uint32_t i) {
        auto const& pv = plan.veneers[i];
        AssembledFunction vf;
        vf.symbol = SymbolId{firstId + i};
        vf.bytes  = elected->bytes;
        vf.relocations.reserve(elected->relocations.size());
        for (auto r : elected->relocations) {
            r.target = collected.targetSymbol[pv.target];
            r.addend = L.targets[pv.target].addend;
            vf.relocations.push_back(r);
        }
        return vf;
    };

    std::vector<AssembledFunction> merged;
    merged.reserve(n + plan.veneers.size());
    std::optional<std::size_t> newEntry;
    for (std::size_t b = 0; b <= n; ++b) {
        for (std::uint32_t k = firstAt[b]; k < firstAt[b + 1]; ++k)
            merged.push_back(makeVeneer(order[k]));
        if (b < n) {
            if (module.imageEntryOverride.has_value()
                && *module.imageEntryOverride == b)
                newEntry = merged.size();
            merged.push_back(std::move(module.functions[b]));
        }
    }
    module.functions = std::move(merged);
    module.expectedFuncCount += plan.veneers.size();
    // ⚠ `imageEntryOverride` IS AN INDEX, NOT A SYMBOL. The entry trampoline
    // records index 0; a veneer standing before it moves the entry without
    // moving the number, which would make the image start executing a veneer.
    if (newEntry.has_value()) module.imageEntryOverride = *newEntry;
    return true;
}

} // namespace dss::linker
