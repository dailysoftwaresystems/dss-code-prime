#include "opt/passes/cse.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/analysis/mir_memory_clobbers.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"
#include "opt/passes/path_compress.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

using dss::opt::analysis::StrictTbaa;
using dss::opt::analysis::MirMemoryClobbers;
using dss::opt::analysis::mirAliasProbeSubstitutionPreservesClobberVerdict;

// Hash-key for a CSE-candidate instruction. Operands are stored in
// canonical order (sorted for commutative 2-operand ops) so the two
// surface forms of `op(a, b)` collapse to one entry.
struct CseKey {
    MirOpcode              op{};
    TypeId                 type{};
    std::vector<MirInstId> operands;
    std::uint32_t          payload = 0;

    [[nodiscard]] bool operator==(CseKey const& o) const noexcept {
        return op == o.op && type.v == o.type.v && payload == o.payload
            && operands.size() == o.operands.size()
            && std::equal(operands.begin(), operands.end(), o.operands.begin(),
                [](MirInstId a, MirInstId b) { return a.v == b.v; });
    }
};

// boost::hash_combine pattern. The default `std::hash<uint32_t>` is
// identity on libstdc++/libc++/MSVC, so a naive XOR-of-shifts
// composition produces trivial collisions on adjacent MirInstIds
// (the common case here). hash_combine mixes the golden-ratio
// constant + rotation so input-bit clustering doesn't survive into
// the output. operator== catches any residual collision so this
// affects throughput, not correctness.
inline void hashCombine(std::size_t& seed, std::size_t v) noexcept {
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

struct CseKeyHash {
    std::size_t operator()(CseKey const& k) const noexcept {
        std::size_t h = std::hash<std::uint16_t>{}(
            static_cast<std::uint16_t>(k.op));
        hashCombine(h, std::hash<std::uint32_t>{}(k.type.v));
        hashCombine(h, std::hash<std::uint32_t>{}(k.payload));
        for (MirInstId const o : k.operands) {
            hashCombine(h, std::hash<std::uint32_t>{}(o.v));
        }
        return h;
    }
};

// Whether an opcode is a CSE candidate. Side-effecting + terminator +
// Phi + Volatile are excluded by the caller; this predicate is the
// OPCODE-level filter only (per-instruction Volatile flag + per-Load
// alias-clobber check are consulted at the use site).
//
// Load admission (cycle 10b): Load IS a CSE candidate now. The use
// site additionally checks the pass-wide `MirMemoryClobbers` index for
// a may-aliasing clobber between the canonical Load and the current
// Load (D-OPT-MEMORYSSA-CLOBBER-WALK — enumeration-identical to the
// reference `mirAnyMayAliasingStoreInRegion` walk) before admitting
// the CSE — this is the alias-safety gate that replaces the prior
// blanket exclusion.
[[nodiscard]] bool isCseCandidateOpcode(MirOpcode op) noexcept {
    if (isTerminator(op)) return false;
    if (isPhi(op)) return false;
    if (opcodeInfo(op).hasSideEffects) return false;
    // Defensive guard: each Alloca is a distinct memory address even
    // at the same type — merging two Allocas would alias two stack
    // slots into one. Today Alloca is `hasSideEffects=true` so the
    // gate above catches it; this redundant check pins the invariant
    // against a future opcode-table cleanup that sets it false.
    if (op == MirOpcode::Alloca) return false;
    return true;
}

// ONE spelling of this pass's name for EVERY diagnostic it can emit — the
// carve-out Info and every `MirFunctionRebuilder` fatal
// (D-OPT-MIR-REBUILDER-FATAL-CANNOT-NAME-THE-PASS). ★ This pass constructs the
// rebuilder at TWO sites (the timed and untimed arms of `runCse`), which is
// precisely why the name is a property of the POLICY and not an argument at each
// construction site: two sites cannot disagree about one constant.
constexpr std::string_view kPassName = "Cse";

class CsePolicy final : public MirRebuildPolicy {
public:
    CsePolicy(Mir const& src, TypeInterner const& interner) noexcept
        : src_(src), interner_(interner),
          strictTbaa_(src.aliasingMode() == MirAliasingMode::StrictTBAA
                      ? StrictTbaa::Yes : StrictTbaa::No),
          charTypesAliasAll_(src.charTypesAliasAll()) {}

    [[nodiscard]] std::string_view passName() const noexcept override {
        return kPassName;
    }

    [[nodiscard]] std::size_t instructionsCsed() const noexcept {
        return instructionsCsed_;
    }

    // `preds` = `mirBuildPredecessors(mir)` for the SAME module, computed ONCE by
    // runCse and threaded in (invariant across every function in one Cse pass), so
    // the whole-module predecessor build is not repeated per function / per query.
    // `clobbers` = the pass-wide memory-clobber index (D-OPT-MEMORYSSA-CLOBBER-WALK)
    // built ONCE beside `preds` — the Load-admission gate queries it instead of
    // re-walking the CFG + re-scanning every instruction per Load query.
    // `domScratch` = the pass-wide reusable dominator scratch
    // (D-OPT-DOMTREE-SCRATCH-REUSE) — byte-identical dom trees without the
    // per-function whole-module allocation storm.
    void analyze(MirFuncId fn, std::vector<std::vector<MirBlockId>> const& preds,
                 MirMemoryClobbers const& clobbers, MirDomScratch& domScratch);

    [[nodiscard]] std::vector<MirBlockId>
    selectBlocks(Mir const& src, MirFuncId fn) override {
        return mirReversePostOrder(src, src.funcEntry(fn));
    }

    [[nodiscard]] MirInstId substituteOldOperand(MirInstId oldOp) override {
        auto it = cseMap_.find(oldOp);
        if (it == cseMap_.end()) return oldOp;
        return it->second;
    }

    void resetPerFunction() {
        cseMap_.clear();
    }

private:
    // THE one place an old operand id becomes its canonical replacement.
    // `buildKey` numbers expressions with it, and the Load-admission gate picks
    // the pointer it hands the alias tests with it. Both MUST speak the same
    // vocabulary: a gate that probes with a RAW id is reasoning about a value
    // the rebuild is about to delete, and the two sites can then silently
    // disagree about what "this pointer" denotes
    // (`D-OPT-CSE-LOAD-PTR-KEY-UNRESOLVED`). One function, so a future change
    // to the resolution discipline cannot reach one consumer and miss the other.
    [[nodiscard]] MirInstId canonicalOperand(MirInstId op) const {
        return resolveTransitive(cseMap_, op, "Cse");
    }

    [[nodiscard]] CseKey buildKey(MirInstId id) const {
        CseKey k;
        k.op      = src_.instOpcode(id);
        k.type    = src_.instType(id);
        k.payload = src_.instPayload(id);
        auto const ops = src_.instOperands(id);
        k.operands.reserve(ops.size());
        for (MirInstId const o : ops) {
            k.operands.push_back(canonicalOperand(o));
        }
        // Canonicalize operand order for binary commutative ops.
        if (isCommutative(k.op) && k.operands.size() == 2) {
            if (k.operands[1].v < k.operands[0].v) {
                std::swap(k.operands[0], k.operands[1]);
            }
        }
        return k;
    }

    Mir const&          src_;
    TypeInterner const& interner_;
    StrictTbaa const    strictTbaa_;
    bool const          charTypesAliasAll_;
    // Old-id → canonical-old-id. Built by analyze() via dom-tree DFS
    // with a scoped value-numbering table; path-compressed after.
    std::unordered_map<MirInstId, MirInstId> cseMap_;
    std::size_t instructionsCsed_ = 0;

public:
    // Env-gated DSS_OPT_TRACE sub-timing accumulators (read by runCse's
    // one-line-per-pass-call trace; zero-cost when the trace is off).
    std::uint64_t traceDomNs = 0;
};

void CsePolicy::analyze(MirFuncId fn,
                        std::vector<std::vector<MirBlockId>> const& preds,
                        MirMemoryClobbers const& clobbers,
                        MirDomScratch& domScratch) {
    resetPerFunction();
    std::uint32_t const blockCount = src_.funcBlockCount(fn);
    if (blockCount == 0) return;

    // Build the dom tree. CSE only walks reachable blocks (RPO). `preds` is the
    // caller's precomputed whole-module predecessor map (invariant this pass);
    // the scratch-backed dom/children are byte-identical to the fresh path and
    // valid until the NEXT analyze() call (function-local use only — const&
    // binding, per the D-OPT-DOMTREE-SCRATCH-REUSE contract).
    MirBlockId const entry = src_.funcEntry(fn);
    static bool const trace = std::getenv("DSS_OPT_TRACE") != nullptr;
    auto const tDom0 = trace ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
    auto const rpo = mirReversePostOrder(src_, entry);
    auto const& dom = computeMirDomTree(src_, entry, rpo, preds, domScratch);
    auto const& dchild = mirDomTreeChildren(src_, dom, domScratch);
    if (trace) {
        traceDomNs += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - tDom0).count());
    }

    // Iterative dom-tree DFS with Visit/Leave frame stack. Scoped
    // value-numbering table: entries added during a block's Visit are
    // rolled back when that block's subtree is left. The
    // dom-tree DFS guarantees a key in scope at use time was defined
    // on the current dom-tree path, satisfying SSA def-dominates-use.
    enum class FrameKind : std::uint8_t { Visit, Leave };
    struct Frame {
        FrameKind   kind;
        MirBlockId  block;
        std::size_t snapshotMark = 0;
    };
    std::vector<Frame> work;
    work.push_back({FrameKind::Visit, entry, 0});

    // Scoped table + rollback log. A block's Visit may BIND a key that was
    // free, or REBIND one an ANCESTOR scope owns — a Load candidate the alias
    // gate refuses becomes the new, nearer canonical
    // (`D-OPT-CSE-STALE-CANONICAL-AFTER-REJECT`). So the log records the
    // PREVIOUS occupant, not merely the key: Leave RESTORES it when there was
    // one and erases only when there was not. Erasing unconditionally (the old
    // shape) destroys the ancestor's canonical for every sibling subtree
    // visited afterwards. Undone in REVERSE order, so repeated rebinds of one
    // key inside a scope unwind to exactly the binding the scope was entered
    // with.
    struct ScopeUndo {
        CseKey    key;
        MirInstId previous{};
        bool      hadPrevious = false;
    };
    std::unordered_map<CseKey, MirInstId, CseKeyHash> scope;
    std::vector<ScopeUndo> log;

    while (!work.empty()) {
        Frame const f = work.back();
        work.pop_back();

        if (f.kind == FrameKind::Leave) {
            while (log.size() > f.snapshotMark) {
                ScopeUndo const& u = log.back();
                if (u.hadPrevious) scope[u.key] = u.previous;
                else               scope.erase(u.key);
                log.pop_back();
            }
            continue;
        }

        MirBlockId const B = f.block;
        std::size_t const snapshotMark = log.size();
        std::uint32_t const ninst = src_.blockInstCount(B);
        for (std::uint32_t i = 0; i < ninst; ++i) {
            MirInstId const id = src_.blockInstAt(B, i);
            MirOpcode const op = src_.instOpcode(id);
            if (!isCseCandidateOpcode(op)) continue;
            // Volatile flag forces an instruction to be observable.
            if (has(src_.instFlags(id), MirInstFlags::Volatile)) continue;

            CseKey k = buildKey(id);
            auto it = scope.find(k);
            if (it != scope.end()) {
                // Load admission gate: a Load CSE'd against a dominating
                // canonical Load is sound only if no may-aliasing clobber sits
                // anywhere between them, ON ANY EXECUTED PATH — the back edge
                // included. That whole question is ONE query against the
                // analysis (`MirMemoryClobbers::anyClobberBetweenPoints`, whose
                // docblock carries the four-slice decomposition, the
                // RE-EXECUTION LEMMA, and the pointer to its specification).
                // It used to be four slices hand-assembled HERE, and that
                // open-coding is exactly what let a missing fourth slice look
                // complete and ship `D-OPT-CSE-LOAD-BACKEDGE-TAIL` — a
                // release-only silent miscompile. This site now states two
                // PROGRAM POINTS and asks; it can no longer reimplement three
                // slices out of four (`D-OPT-CSE-CLOBBER-COVER-CHOKEPOINT`).
                // For non-Load opcodes the gate is a no-op (only Load reads
                // memory in the v1 opcode set; if a future memory-reading
                // opcode lands — AtomicLoad, VolatileLoad — it MUST be added to
                // this gate explicitly, since the alias substrate doesn't know
                // about it).
                bool admit = true;
                if (op == MirOpcode::Load) {
                    MirInstId const canonical = it->second;
                    auto const ops = src_.instOperands(id);
                    if (ops.empty()) {
                        std::fprintf(stderr,
                            "dss::opt::passes::Cse fatal: Load inst v=%u "
                            "has zero operands — verifier-contract "
                            "violation (Load's pointer operand at "
                            "operands[0] is required).\n",
                            id.v);
                        std::abort();
                    }
                    // The alias probe is the pointer operand resolved through
                    // the SAME map `buildKey` uses — the raw operand names a
                    // value the rebuild is about to delete
                    // (`D-OPT-CSE-LOAD-PTR-KEY-UNRESOLVED`). The substitution
                    // is licensed by the theorem on
                    // `mirAliasProbeSubstitutionPreservesClobberVerdict`:
                    // equal opcode + equal TypeId + NOT an `Alloca` ⇒ every
                    // clobber verdict is unchanged, so this can never weaken
                    // the gate. CSE satisfies that premise by construction —
                    // the key carries the opcode AND the result type, and
                    // `isCseCandidateOpcode` excludes `Alloca`, so no redirect
                    // can put an `Alloca` at either end and unlock Rule 2's
                    // "distinct stack slots ⇒ No" for a merged pair. That third
                    // clause is load-bearing: two DISTINCT `Alloca`s share
                    // opcode and TypeId, and swapping them WOULD weaken a
                    // Store's verdict from Rule 1's `Yes` to Rule 2's `No`.
                    // It is CHECKED, not assumed, so a future key that stops
                    // discriminating a field — or an opcode table that lets
                    // `Alloca` into CSE — aborts here instead of silently
                    // admitting a stale Load.
                    MirInstId const rawPtr  = ops[0];
                    MirInstId const loadPtr = canonicalOperand(rawPtr);
                    if (!mirAliasProbeSubstitutionPreservesClobberVerdict(
                            src_, rawPtr, loadPtr)) {
                        std::fprintf(stderr,
                            "dss::opt::passes::Cse fatal: Load pointer v=%u "
                            "canonicalizes to v=%u with a different opcode or "
                            "result type — a CSE redirect may only join "
                            "same-opcode/same-type values, and the alias "
                            "probe substitution is unsound without that "
                            "(D-OPT-CSE-LOAD-PTR-KEY-UNRESOLVED).\n",
                            rawPtr.v, loadPtr.v);
                        std::abort();
                    }
                    MirBlockId const canonicalBlock = src_.instBlock(canonical);

                    // Locate canonical in its block. Substrate-contract
                    // invariant: `instBlock(canonical) ⟹ canonical is
                    // in blockInstAt(canonicalBlock, *)`. A miss
                    // signals a substrate breach (instBlock and
                    // blockInstAt disagree) — fail loud rather than
                    // silently admit/refuse and hide the corruption.
                    std::uint32_t const cn = src_.blockInstCount(canonicalBlock);
                    std::uint32_t canonicalIdx = cn;
                    for (std::uint32_t j = 0; j < cn; ++j) {
                        if (src_.blockInstAt(canonicalBlock, j).v == canonical.v) {
                            canonicalIdx = j;
                            break;
                        }
                    }
                    if (canonicalIdx == cn) {
                        std::fprintf(stderr,
                            "dss::opt::passes::Cse fatal: canonical "
                            "Load v=%u not in canonicalBlock v=%u "
                            "inst list — instBlock/blockInstAt "
                            "substrate-contract violation.\n",
                            canonical.v, canonicalBlock.v);
                        std::abort();
                    }

                    // ONE query, two program points. The four-slice cover, the
                    // RE-EXECUTION LEMMA, the same-block ordering contract and
                    // the wrap-around trigger all live inside it, next to the
                    // specification the differential pins hold it to. Every
                    // slice funnels through the SAME per-instruction predicate
                    // (`mirInstClobbersLoadPtr` — precise for Stores, opaque
                    // for the `opcodeClobbersMemory` ops) called at QUERY time,
                    // so the scans can never disagree about what clobbers; the
                    // index only pre-filters the ENUMERATION and memoizes CFG
                    // reachability (`D-OPT-MEMORYSSA-CLOBBER-WALK`).
                    admit = !clobbers.anyClobberBetweenPoints(
                        interner_, loadPtr, canonicalBlock, canonicalIdx,
                        B, i, strictTbaa_, charTypesAliasAll_);
                }
                if (admit) {
                    cseMap_[id] = it->second;
                    ++instructionsCsed_;
                    continue;
                }
                // Fall through to the REBIND below: a refused candidate takes
                // over the binding (`D-OPT-CSE-STALE-CANONICAL-AFTER-REJECT`).
            }
            // Bind — or REBIND — this instruction as the canonical for `k`.
            //
            // Leaving a stale, pre-clobber canonical in place (what
            // `scope.emplace` did, since it is a NO-OP on an existing key) is
            // SOUND but imprecise: two Loads that both sit AFTER the same
            // clobber could never CSE against each other, because neither ever
            // became canonical. The refused candidate is the strictly better
            // canonical — it is nearer, so the region a later candidate must
            // prove clean is a SUBSET of the one the stale canonical demanded,
            // and it is this very instruction, so it dominates exactly the
            // remaining scope the old binding did. The dom-tree DFS discipline
            // is unchanged; only the undo record grows a "previous" field so
            // Leave can RESTORE an ancestor's binding instead of erasing it.
            if (it == scope.end()) {
                log.push_back(ScopeUndo{k, MirInstId{}, false});
                scope.emplace(std::move(k), id);
            } else {
                log.push_back(ScopeUndo{k, it->second, true});
                it->second = id;
            }
        }

        // Queue Leave for THIS block AFTER children are visited.
        work.push_back({FrameKind::Leave, B, snapshotMark});
        if (B.v < dchild.size()) {
            auto const& kids = dchild[B.v];
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                work.push_back({FrameKind::Visit, *it, 0});
            }
        }
    }

    pathCompressAndVerify(cseMap_, "Cse");
}

} // namespace

CseResult runCse(Mir& mir, TypeInterner const& interner,
                 DiagnosticReporter& reporter) {
    CseResult result{};
    MirBuilder builder;

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, kPassName)
        == GlobalClonePrelude::CarvedOut) {
        result.ok = true;
        return result;
    }

    CsePolicy policy{mir, interner};
    std::size_t const nf = mir.moduleFuncCount();
    // Compute the whole-module predecessor map + the memory-clobber index ONCE
    // for the entire pass — both are invariant while `mir` is read-only (the
    // rebuild writes a SEPARATE builder, finalized only after this loop at
    // `mir = ...finish()`). The preds hoist removed the O(numFunctions ×
    // moduleSize) rebuild (D-OPT-CSE-ANALYSIS-HOIST); the clobber index removes
    // the per-Load-query CFG re-walk + every-instruction region scans
    // (D-OPT-MEMORYSSA-CLOBBER-WALK) — the Load-admission alias tests now touch
    // only actual clobbers via memoized reachability.
    static bool const trace = std::getenv("DSS_OPT_TRACE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto msSince = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    auto const tSetup = now();
    auto const preds = mirBuildPredecessors(mir);
    MirMemoryClobbers const clobbers{mir, preds};
    MirDomScratch domScratch;   // one per pass call (D-OPT-DOMTREE-SCRATCH-REUSE)
    long long const setupMs = trace ? msSince(tSetup) : 0;
    std::uint64_t analyzeNs = 0, rebuildNs = 0;
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        auto const tA = trace ? now() : std::chrono::steady_clock::time_point{};
        policy.analyze(f, preds, clobbers, domScratch);
        if (trace) {
            auto const tR = now();
            analyzeNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(tR - tA)
                    .count());
            MirFunctionRebuilder rb{mir, builder, policy};
            rb.rebuildFunction(f);
            rebuildNs += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now() - tR)
                    .count());
        } else {
            MirFunctionRebuilder rb{mir, builder, policy};
            rb.rebuildFunction(f);
        }
    }
    if (trace) {
        std::fprintf(stderr,
            "opt:   Cse sub: preds+index=%lldms analyze=%llums (dom=%llums "
            "vn=%llums) rebuild=%llums\n",
            setupMs,
            static_cast<unsigned long long>(analyzeNs / 1000000u),
            static_cast<unsigned long long>(policy.traceDomNs / 1000000u),
            static_cast<unsigned long long>(
                (analyzeNs - std::min(analyzeNs, policy.traceDomNs)) / 1000000u),
            static_cast<unsigned long long>(rebuildNs / 1000000u));
    }

    result.instructionsCsed = policy.instructionsCsed();
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
