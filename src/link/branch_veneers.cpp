#include "link/branch_veneers.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/fresh_symbol_ids.hpp"
#include "link/object_format_schema.hpp"
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

// A window narrowed by `m` on both sides — except a side that is unbounded,
// which no amount of drift can bring into range.
[[nodiscard]] link::RelocReach shrink(link::RelocReach r, std::int64_t m) noexcept {
    return link::RelocReach{
        r.minDelta == std::numeric_limits<std::int64_t>::min() ? r.minDelta
                                                               : r.minDelta + m,
        r.maxDelta == std::numeric_limits<std::int64_t>::max() ? r.maxDelta
                                                               : r.maxDelta - m};
}

[[nodiscard]] constexpr std::uint64_t lowestSetBit(std::uint64_t v) noexcept {
    return v & (~v + 1u);
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

// ── THE BODIES, ASSEMBLED ONCE FROM THE DECLARED SEQUENCES ─────────────
//
// ★★ BUILT, NOT QUOTED — AND BUILT BY THE SAME ASSEMBLER THAT BUILDS
// EVERYTHING ELSE. A sequence names the target's own opcodes; a step may name
// only the granted scratch registers, the veneer's target and the body's own
// data word. The result is a TEMPLATE whose relocations aim at two placeholder
// symbols — the target, and the body's literal — re-aimed per veneer. The
// entry trampoline mints a function at link time the same way
// (`entry_trampoline.cpp`).
//
// ★★ AND THE TWO DECLARATIONS MUST AGREE. Each step also declares the
// relocations its encoding carries, and the assembled template is checked
// against that list, placeholder by placeholder: an opcode row edited so that
// `lea` stopped emitting its page relocation would otherwise turn every veneer
// into a jump to page 0.
//
// ★★ A DATA WORD IS NOT ASSEMBLED — nothing executes it. The pass appends it
// after the instructions, with slack enough to land it on its natural
// alignment from any instruction boundary a veneer can stand at, and writes it
// through the one PC-relative relocation the step declares
// ([[D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB]]).
constexpr std::uint32_t kTemplateTargetV  = 1;  // relocations aimed at the veneer's target
constexpr std::uint32_t kTemplateLiteralV = 2;  // relocations aimed at the body's data word

struct BodyTemplate {
    std::string                 name;
    std::vector<std::uint8_t>   code;            // the assembled instructions
    std::vector<Relocation>     relocations;     // aimed at the two placeholders
    std::uint8_t                dataBytes = 0;   // 0: the body carries no data word
    RelocationKind              dataRelocation{};
    std::uint64_t               dataAlign = 1;   // the word's natural alignment
    std::uint64_t               slack     = 0;   // bytes that let the word land aligned
    std::uint64_t               size      = 0;   // code + slack + data: what a veneer occupies
    link::RelocReach            reach{};         // (target - veneer start) every field encodes
    std::vector<RelocationKind> kinds;           // every kind the body is written through
};

// Builds the template for a body that ALREADY satisfies the linkVeneers shape
// rules: `injectBranchVeneers` asks the schema for `linkVeneerProblems()` —
// the one rule set `validate()` also applies at load — and refuses any
// problem before a body gets here, so the builder never sees a malformed one.
[[nodiscard]] std::optional<BodyTemplate>
assembleBody(LinkVeneerBody const& body,
             TargetSchema const&   target,
             std::uint64_t         granule,
             DiagnosticReporter&   reporter) {
    // Every register a step names is a declared, granted scratch register: the
    // shape rules the caller applied guarantee it. A register the target has
    // no row for would therefore be an internal-invariant violation, refused
    // below rather than dereferenced.
    std::optional<std::string> badRegister;
    auto const physical = [&](LinkVeneerOperand const& op) {
        auto const* info = target.registerInfo(op.reg);
        if (info == nullptr) {
            badRegister = op.registerName;
            return InvalidLirReg;
        }
        return makePhysicalReg(op.reg, static_cast<LirRegClass>(info->regClass));
    };

    LirBuilder b{target};
    (void)b.addFunction(SymbolId{kTemplateTargetV});
    auto const blk = b.createBlock();
    b.beginBlock(blk);
    std::size_t lastInstruction = 0;
    for (std::size_t i = 0; i < body.sequence.size(); ++i)
        if (!body.sequence[i].isData()) lastInstruction = i;
    // The declared relocations, in emission order, each with the placeholder
    // its step's symbol operand stands for.
    std::vector<std::pair<RelocationKind, std::uint32_t>> declared;
    BodyTemplate tpl;
    tpl.name = body.name;
    for (std::size_t i = 0; i < body.sequence.size(); ++i) {
        auto const& step = body.sequence[i];
        if (step.isData()) {
            tpl.dataBytes      = step.dataBytes;
            tpl.dataRelocation = step.relocations.front();
            continue;
        }
        std::vector<LirOperand> ops;
        ops.reserve(step.operands.size() + 2);
        std::uint32_t placeholder = 0;
        for (auto const& op : step.operands) {
            switch (op.kind) {
                case LinkVeneerOperandKind::Register:
                    ops.push_back(LirOperand::makeReg(physical(op)));
                    break;
                case LinkVeneerOperandKind::Memory:
                    ops.push_back(LirOperand::makeReg(physical(op)));
                    ops.push_back(LirOperand::makeMemBase(1));
                    ops.push_back(LirOperand::makeMemOffset(op.offset));
                    break;
                case LinkVeneerOperandKind::Target:
                    placeholder = kTemplateTargetV;
                    ops.push_back(LirOperand::makeSymbolRef(kTemplateTargetV));
                    break;
                case LinkVeneerOperandKind::Literal:
                    placeholder = kTemplateLiteralV;
                    ops.push_back(LirOperand::makeSymbolRef(kTemplateLiteralV));
                    break;
            }
        }
        for (auto const k : step.relocations) declared.emplace_back(k, placeholder);
        if (i == lastInstruction) {
            // The branch LEAVES the function, so it has no in-function
            // successor; the assembler never reads successors.
            (void)b.addIndirectBr(step.opcode, ops, {});
        } else {
            LirReg result = InvalidLirReg;
            if (step.writesRegister()) {
                LinkVeneerOperand r;
                r.reg          = step.resultRegister;
                r.registerName = step.resultName;
                result         = physical(r);
            }
            (void)b.addInst(step.opcode, result, ops);
        }
    }
    if (badRegister.has_value()) {
        emit(reporter, std::format(
            "branch-veneer: veneer body '{}' of target '{}' names register '{}', "
            "which the target does not declare, although the shape rules were "
            "applied before the body was built — an internal-invariant violation "
            "(D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB)",
            body.name, std::string{target.name()}, *badRegister));
        return std::nullopt;
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

    // The declared relocations against the assembled ones: kind AND the
    // placeholder each aims at, in emission order.
    bool agree = declared.size() == fn.relocations.size();
    for (std::size_t k = 0; agree && k < declared.size(); ++k) {
        agree = fn.relocations[k].kind == declared[k].first
             && fn.relocations[k].target.v == declared[k].second
             && fn.relocations[k].addend == 0;
    }
    if (!agree) {
        auto const names = [&](auto const& list, auto kindOf) {
            std::string s;
            for (auto const& e : list) {
                auto const k = kindOf(e);
                auto const* row = target.relocationInfo(k);
                if (!s.empty()) s += ", ";
                s += row != nullptr ? row->name : std::to_string(k.v);
            }
            return s.empty() ? std::string{"none"} : s;
        };
        emit(reporter, std::format(
            "branch-veneer: veneer body '{}' of target '{}' declares the "
            "relocations [{}] but its opcodes assembled [{}]. The two "
            "declarations have to agree — each relocation aimed at the operand its "
            "step names — or every veneer would be patched through fields nobody "
            "declared "
            "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
            body.name, std::string{target.name()},
            names(declared, [](auto const& e) { return e.first; }),
            names(fn.relocations, [](Relocation const& r) { return r.kind; })));
        return std::nullopt;
    }
    tpl.code        = std::move(fn.bytes);
    tpl.relocations = std::move(fn.relocations);
    for (auto const& r : tpl.relocations) tpl.kinds.push_back(r.kind);

    // The data word's slot. A veneer starts on some multiple of `granule`, so
    // the word's distance from alignment depends on WHERE the veneer lands;
    // `slack` is the most that distance can be, and every veneer of this body
    // reserves it (the slot is picked per veneer at materialization).
    std::uint64_t const c = tpl.code.size();
    if (tpl.dataBytes != 0) {
        tpl.kinds.push_back(tpl.dataRelocation);
        tpl.dataAlign = tpl.dataBytes;
        std::uint64_t const a = tpl.dataAlign;
        std::uint64_t const step = std::min(granule, a);
        for (std::uint64_t j = 0; j * step < a; ++j) {
            std::uint64_t const r = (c + j * granule) % a;
            tpl.slack = std::max(tpl.slack, (a - r) % a);
        }
    }
    tpl.size = c + tpl.slack + tpl.dataBytes;

    // The body's reach: the window EVERY field aimed at the target can encode,
    // measured from the veneer's own start. An absolute field (the low-12 half
    // of the pair) is not bounded by distance and adds nothing. The data word
    // may sit anywhere in [c, c + slack], so its window is taken at both ends.
    link::RelocReach reach{std::numeric_limits<std::int64_t>::min(),
                           std::numeric_limits<std::int64_t>::max()};
    auto const narrow = [&](RelocationKind kind, std::int64_t lo, std::int64_t hi) {
        auto const* row = target.relocationInfo(kind);
        if (row == nullptr) return;
        auto const w = link::relocFieldReach(*row);
        if (!w.has_value()) return;
        reach.minDelta = std::max(reach.minDelta, w->minDelta + hi);
        reach.maxDelta = std::min(reach.maxDelta, w->maxDelta + lo);
    };
    for (auto const& r : tpl.relocations) {
        auto const off = static_cast<std::int64_t>(r.offset);
        if (r.target.v == kTemplateTargetV) {
            narrow(r.kind, off, off);
            continue;
        }
        // Aimed at the body's own literal: a fixed, tiny distance, which the
        // field must hold wherever the slot lands.
        auto const* row = target.relocationInfo(r.kind);
        auto const w = row != nullptr ? link::relocFieldReach(*row) : std::nullopt;
        std::int64_t const nearest  = static_cast<std::int64_t>(c) - off;
        std::int64_t const farthest = static_cast<std::int64_t>(c + tpl.slack) - off;
        if (w.has_value() && !(inWindow(*w, nearest) && inWindow(*w, farthest))) {
            emit(reporter, std::format(
                "branch-veneer: veneer body '{}' of target '{}' addresses its own "
                "data word through relocation '{}', which cannot reach it "
                "({}..{} bytes away) "
                "(D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB)",
                body.name, std::string{target.name()},
                row != nullptr ? row->name : std::string{"?"}, nearest, farthest));
            return std::nullopt;
        }
    }
    if (tpl.dataBytes != 0)
        narrow(tpl.dataRelocation, static_cast<std::int64_t>(c),
               static_cast<std::int64_t>(c + tpl.slack));
    tpl.reach = reach;
    return tpl;
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
    //     a veneer, so the F far ones together add at most F × (the largest
    //     candidate body) bytes anywhere in the image. Taking every decision
    //     against the reach minus that much makes each one immune to every
    //     insertion that follows it. Past half the narrowest reach the margin
    //     is CAPPED there, and the exact check the caller runs on the plan
    //     decides (see the header).
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
    std::uint64_t largestBody = 0;
    for (auto const& body : in.bodies) {
        if (body.size == 0) {
            plan.status = VeneerPlanStatus::Malformed;
            return plan;
        }
        largestBody = std::max(largestBody, body.size);
    }
    if (halfMin <= 0 || largestBody == 0) {
        plan.status = VeneerPlanStatus::Malformed;
        return plan;
    }
    std::uint64_t margin = far * largestBody;
    if (margin > static_cast<std::uint64_t>(halfMin)) {
        margin = static_cast<std::uint64_t>(halfMin);
        plan.marginCapped = true;
    }
    plan.margin = margin;
    auto const m = static_cast<std::int64_t>(margin);
    // Each body's window, shrunk by the same margin: the distance from a veneer
    // to its target moves by at most `m` as later veneers are inserted.
    std::vector<link::RelocReach> bodyWindow;
    bodyWindow.reserve(in.bodies.size());
    for (auto const& body : in.bodies) bodyWindow.push_back(shrink(body.reach, m));

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
        // THE BODY: the first candidate that reaches the target from there —
        // GNU ld's per-stub rule (the long form only where the ADRP one can't).
        std::int64_t const vd = posOf(tgt) - static_cast<std::int64_t>(start[boundary]);
        std::size_t body = in.bodies.size();
        for (std::size_t k = 0; k < in.bodies.size(); ++k) {
            ++plan.work.bodyProbes;
            if (inWindow(bodyWindow[k], vd)) {
                body = k;
                break;
            }
        }
        if (body == in.bodies.size()) {
            plan.status            = VeneerPlanStatus::BodyOutOfReach;
            plan.failedSite        = static_cast<std::uint32_t>(si);
            plan.failedDelta       = d;
            plan.failedVeneerDelta = vd;
            return plan;
        }
        auto const idx = static_cast<std::int32_t>(plan.veneers.size());
        plan.veneers.push_back(PlannedVeneer{boundary, s.target,
                                             static_cast<std::uint32_t>(body)});
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
    // A veneer of an unknown body, or at a boundary past the last, is not a
    // plan for this layout; say where rather than index past the end.
    for (std::size_t i = 0; i < plan.veneers.size(); ++i) {
        if (plan.veneers[i].body >= in.bodies.size() || plan.veneers[i].boundary > n)
            return VeneerMisfit{true, static_cast<std::uint32_t>(i), 0};
    }
    auto const sizeOf = [&](PlannedVeneer const& v) { return in.bodies[v.body].size; };
    std::vector<std::uint64_t> islandBytes(n + 1, 0);
    for (auto const& v : plan.veneers) islandBytes[v.boundary] += sizeOf(v);
    // islandBase[b]: where boundary b's island begins; fstart[k]: function k's
    // final start, after its own boundary's island.
    std::vector<std::uint64_t> islandBase(n + 1, 0), fstart(n + 1, 0);
    std::uint64_t before = 0, inserted = 0;
    for (std::size_t b = 0; b <= n; ++b) {
        islandBase[b] = before + inserted;
        inserted += islandBytes[b];
        fstart[b] = before + inserted;
        if (b < n) before += in.functionSizes[b];
    }
    std::uint64_t const textEnd = fstart[n];
    // Within an island, veneers stand in placement order.
    std::vector<std::uint64_t> filled(n + 1, 0);
    std::vector<std::uint64_t> vpos(plan.veneers.size(), 0);
    for (std::size_t i = 0; i < plan.veneers.size(); ++i) {
        auto const b = plan.veneers[i].boundary;
        vpos[i] = islandBase[b] + filled[b];
        filled[b] += sizeOf(plan.veneers[i]);
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
        if (!inWindow(in.bodies[plan.veneers[i].body].reach, d))
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
                         ObjectFormatSchema const&         format,
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

    // ── THE SHAPE RULES, ONCE PER LINK ─────────────────────────────────────
    //
    // The same rule set `validate()` applies at load — one function, asked of
    // the schema. A schema the JSON loader built has passed it already; one
    // built in memory never went through the loader. Every problem is reported
    // at its JSON path, and no body is built from a vocabulary that has one.
    if (auto const problems = target.linkVeneerProblems(); !problems.empty()) {
        for (auto const& p : problems) {
            emit(reporter, std::format(
                "branch-veneer: target '{}' declares a `linkVeneers` block that "
                "breaks its shape rules at {}: {} — no veneer is built from it, "
                "so this branch cannot be carried: {} "
                "(D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB)",
                std::string{target.name()}, p.path, p.message,
                describeSite(initialMisfit->index, initialMisfit->delta)));
        }
        return false;
    }

    // ── THE BODIES: assemble every declared one, keep the USABLE ones ──────
    //
    // A veneer starts on an instruction boundary — the branch fields' own
    // alignment, which every function and every body preserves.
    std::uint64_t granule = 1;
    for (auto const kind : collected.reachKind) {
        auto const* row = target.relocationInfo(kind);
        granule = std::max<std::uint64_t>(
            granule, std::uint64_t{1} << link::branchRelocGeometry(row->formulaKind).scaleLog2);
    }
    // What the output format promises for `.text`'s first byte: its declared
    // alignment, and never more than its declared address actually has.
    std::uint64_t textStartAlign = 1;
    if (auto const* text = format.sectionByKind(SectionKind::Text)) {
        textStartAlign = std::max<std::uint64_t>(1, text->addrAlign);
        if (text->virtualAddress != 0)
            textStartAlign = std::min(textStartAlign, lowestSetBit(text->virtualAddress));
    }
    std::vector<BodyTemplate> usable;
    std::vector<std::string>  unusable;  // "'<body>': <why not in this output>"
    for (auto const& body : vocab->bodies) {
        auto tpl = assembleBody(body, target, granule, reporter);
        if (!tpl.has_value()) return false;
        // A veneer is inserted between functions, so its size must keep every
        // following function on the branch fields' own instruction alignment.
        if (tpl->size % granule != 0) {
            emit(reporter, std::format(
                "branch-veneer: veneer body '{}' is {} bytes, which is not a "
                "multiple of the {}-byte instruction alignment of the branches it "
                "carries, so inserting it would misalign every function after it "
                "(D-LK-SYNTHETIC-ENTRY-IMPORT-CALL-OVERFLOWS-PAST-THE-BRANCH-REACH)",
                tpl->name, tpl->size, granule));
            return false;
        }
        // USABLE IN THIS OUTPUT: the format must declare every relocation the
        // body is written through (the linker's unifier refuses any other), and
        // a data word needs `.text` to start at least as aligned as the word.
        std::string why;
        for (auto const k : tpl->kinds) {
            if (format.relocationByKind(k) != nullptr) continue;
            auto const* row = target.relocationInfo(k);
            why = std::format("object format '{}' declares no relocation '{}' "
                              "(kind {}), which the body is written through",
                              std::string{format.name()},
                              row != nullptr ? row->name : std::string{"?"}, k.v);
            break;
        }
        if (why.empty() && tpl->dataBytes != 0 && tpl->dataAlign > textStartAlign) {
            why = std::format("its {}-byte data word must land {}-aligned, and object "
                              "format '{}' places `.text` only {}-aligned",
                              tpl->dataBytes, tpl->dataAlign,
                              std::string{format.name()}, textStartAlign);
        }
        if (!why.empty()) {
            unusable.push_back(std::format("'{}': {}", tpl->name, why));
            continue;
        }
        usable.push_back(std::move(*tpl));
    }
    auto const unusableText = [&] {
        std::string s;
        for (auto const& u : unusable) {
            if (!s.empty()) s += "; ";
            s += u;
        }
        return s;
    };
    if (usable.empty()) {
        emit(reporter, std::format(
            "branch-veneer: {}, and no veneer body target '{}' declares can be "
            "built in object format '{}': {} "
            "(D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB)",
            describeSite(initialMisfit->index, initialMisfit->delta),
            std::string{target.name()}, std::string{format.name()}, unusableText()));
        return false;
    }

    // ── THE CANDIDATES: the usable bodies up to the first whose reach covers
    //    the whole image. A body past it would never be elected (every veneer
    //    tries the cheaper ones first and that one always reaches), so it
    //    would only inflate the margin — and an image under ~4 GiB therefore
    //    plans exactly as it did when the ADRP body was the only one.
    std::uint64_t maxPast = 0;
    for (auto const& t : L.targets)
        if (t.function == kVeneerTargetIsStub) maxPast = std::max(maxPast, t.pastTextEnd);
    std::uint64_t textSize = 0;
    for (auto const s : L.functionSizes) textSize += s;
    std::vector<std::size_t> candidate;  // indices into `usable`, cheapest first
    std::uint64_t largest = 0;
    for (std::size_t i = 0; i < usable.size(); ++i) {
        candidate.push_back(i);
        largest = std::max(largest, usable[i].size);
        auto const span = static_cast<std::int64_t>(
            textSize + maxPast + L.sites.size() * largest);
        if (symmetricReach(usable[i].reach) >= span) break;
    }
    for (auto const i : candidate)
        L.bodies.push_back(VeneerBodyModel{usable[i].size, usable[i].reach});

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
        case VeneerPlanStatus::BodyOutOfReach: {
            std::size_t widest = 0;
            for (std::size_t k = 1; k < L.bodies.size(); ++k)
                if (symmetricReach(L.bodies[k].reach) > symmetricReach(L.bodies[widest].reach))
                    widest = k;
            auto const& wb = usable[candidate[widest]];
            emit(reporter, std::format(
                "branch-veneer: {}, and no veneer body this output can carry reaches "
                "the target from where the veneer must stand: the widest, '{}', "
                "encodes [{}, {}] bytes from the veneer and the target is {} bytes "
                "away. {} "
                "(D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB)",
                describeSite(plan.failedSite, plan.failedDelta), wb.name,
                wb.reach.minDelta, wb.reach.maxDelta, plan.failedVeneerDelta,
                unusable.empty()
                    ? std::string{"GNU ld 2.42 goes further with a PC-relative "
                                  "literal body; a target that declares one, in a "
                                  "format that declares the relocation its word is "
                                  "written through, goes further here too."}
                    : "Declared by the target but not usable in this output: "
                          + unusableText() + "."));
            return false;
        }
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
    auto const templateOf = [&](PlannedVeneer const& pv) -> BodyTemplate const& {
        return usable[candidate[pv.body]];
    };

    // WHERE EACH VENEER LANDS — the merge below, walked once without moving
    // anything, so a veneer that could not land its data word aligned is
    // refused before the module is touched. `.text` starts `textStartAlign`-
    // aligned, so a word's text offset decides its address's alignment.
    std::vector<std::uint64_t> literalAt(plan.veneers.size(), 0);
    {
        std::uint64_t at = 0;
        for (std::size_t b = 0; b <= n; ++b) {
            for (std::uint32_t k = firstAt[b]; k < firstAt[b + 1]; ++k) {
                auto const v   = order[k];
                auto const& tp = templateOf(plan.veneers[v]);
                if (tp.dataBytes != 0) {
                    std::uint64_t const c = tp.code.size();
                    std::uint64_t const lit =
                        c + (tp.dataAlign - (at + c) % tp.dataAlign) % tp.dataAlign;
                    if (lit - c > tp.slack) {
                        emit(reporter, std::format(
                            "branch-veneer: veneer {} ('{}') lands at text offset "
                            "{}, which leaves its data word {} bytes from "
                            "alignment and only {} bytes of slack — every function "
                            "before it should be a whole number of {}-byte "
                            "instructions, so this is an internal-invariant "
                            "violation (D-LK-AARCH64-VENEER-CANNOT-REACH-PAST-FOUR-GIB)",
                            v, tp.name, at, lit - c, tp.slack, granule));
                        return false;
                    }
                    literalAt[v] = lit;
                }
                at += tp.size;
            }
            if (b < n) at += module.functions[b].bytes.size();
        }
    }

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

    auto const makeVeneer = [&](std::uint32_t i) {
        auto const& pv  = plan.veneers[i];
        auto const& tp  = templateOf(pv);
        SymbolId const self{firstId + i};
        SymbolId const aim         = collected.targetSymbol[pv.target];
        std::int64_t const aimPlus = L.targets[pv.target].addend;
        AssembledFunction vf;
        vf.symbol = self;
        vf.bytes  = tp.code;
        vf.bytes.resize(tp.size, 0u);  // the slack and the word; the word is relocated
        vf.relocations.reserve(tp.relocations.size() + 1);
        for (auto r : tp.relocations) {
            if (r.target.v == kTemplateTargetV) {
                r.target = aim;
                r.addend = aimPlus;
            } else {
                // The body's own literal: this veneer's symbol, plus the slot.
                r.target = self;
                r.addend = static_cast<std::int64_t>(literalAt[i]);
            }
            vf.relocations.push_back(r);
        }
        if (tp.dataBytes != 0) {
            vf.relocations.push_back(Relocation{
                static_cast<std::uint32_t>(literalAt[i]), aim, tp.dataRelocation, aimPlus});
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
