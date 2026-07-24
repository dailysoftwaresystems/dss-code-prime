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
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

using dss::opt::analysis::StrictTbaa;
using dss::opt::analysis::MirMemoryClobbers;

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

class CsePolicy final : public MirRebuildPolicy {
public:
    CsePolicy(Mir const& src, TypeInterner const& interner) noexcept
        : src_(src), interner_(interner),
          strictTbaa_(src.aliasingMode() == MirAliasingMode::StrictTBAA
                      ? StrictTbaa::Yes : StrictTbaa::No),
          charTypesAliasAll_(src.charTypesAliasAll()) {}

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
    [[nodiscard]] CseKey buildKey(MirInstId id) const {
        CseKey k;
        k.op      = src_.instOpcode(id);
        k.type    = src_.instType(id);
        k.payload = src_.instPayload(id);
        auto const ops = src_.instOperands(id);
        k.operands.reserve(ops.size());
        for (MirInstId const o : ops) {
            k.operands.push_back(resolveTransitive(cseMap_, o, "Cse"));
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

    // Scoped table + rollback log. Inserts are gated by a miss
    // (`scope.find(k) == end()`) so the log records keys-to-erase
    // only — never a prior occupant to restore. A future variant
    // that overwrites on hit (e.g. "prefer earlier dominating def
    // by RPO depth") would extend the log shape; today the
    // single-pass insert-on-miss discipline keeps it minimal.
    std::unordered_map<CseKey, MirInstId, CseKeyHash> scope;
    std::vector<CseKey> log;

    while (!work.empty()) {
        Frame const f = work.back();
        work.pop_back();

        if (f.kind == FrameKind::Leave) {
            while (log.size() > f.snapshotMark) {
                scope.erase(log.back());
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
                // canonical Load is sound only if no may-aliasing Store
                // sits anywhere between them, ON ANY EXECUTED PATH — the
                // back edge included. We scan FOUR slices, three owned by
                // the caller (this site) plus the strictly-between region
                // owned by the clobber index's `anyClobberBetween` (same
                // fwd∩bwd region as the reference `mirRegionBetween`,
                // memoized):
                //   (a) canonical's block tail (after canonical, to end)
                //   (b) strictly-between blocks (region walker)
                //   (c) useBlock's head (start, up to current)
                //   (d) useBlock's TAIL, when the use block sits on a cycle
                //       that does not pass through the canonical's block
                //       (`D-OPT-CSE-LOAD-BACKEDGE-TAIL` — omitting this was
                //       a real release-only miscompile)
                // ★ These four are a COVER: proving that is a hand argument
                // re-done at each edit, not a property the API enforces. If a
                // SECOND consumer ever needs point-to-point clobber cover,
                // hoist the decomposition into the analysis as one chokepoint
                // so it cannot be partially reimplemented —
                // `D-OPT-CSE-CLOBBER-COVER-CHOKEPOINT`.
                // The region walker EXCLUDES both endpoints to keep the
                // two responsibilities disjoint and prevent the dead-
                // code-masking-bug class where overlapping scans hide
                // each other's correctness gaps. For non-Load opcodes
                // the gate is a no-op (only Load reads memory in the
                // v1 opcode set; if a future memory-reading opcode
                // lands — AtomicLoad, VolatileLoad — it MUST be added
                // to this gate explicitly, since the alias substrate
                // doesn't know about it).
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
                    MirInstId const loadPtr = ops[0];
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

                    // c113 (audit-F1): every slice funnels through the SAME
                    // per-instruction predicate (`mirInstClobbersLoadPtr` —
                    // precise for Stores, opaque clobber for the
                    // `opcodeClobbersMemory` ops), called at QUERY time by the
                    // clobber index — the scan sites can never disagree on
                    // what clobbers. The index only pre-filters the
                    // ENUMERATION to actual clobbers (a non-clobber can never
                    // satisfy the predicate) and memoizes the CFG reachability
                    // (D-OPT-MEMORYSSA-CLOBBER-WALK).
                    auto storesClobber = [&](MirBlockId blk,
                                             std::uint32_t lo,
                                             std::uint32_t hi) -> bool {
                        return clobbers.anyClobberInBlockRange(
                            interner_, loadPtr, blk, lo, hi,
                            strictTbaa_, charTypesAliasAll_);
                    };

                    if (canonicalBlock.v == B.v) {
                        // Same-block case: scan strictly between
                        // canonical (at canonicalIdx) and current
                        // (at i). Dom-tree DFS scope guarantees
                        // canonicalIdx < i; assert it so a future
                        // reorder doesn't silently corrupt scope.
                        if (canonicalIdx >= i) {
                            std::fprintf(stderr,
                                "dss::opt::passes::Cse fatal: "
                                "canonical idx=%u >= current idx=%u "
                                "in same block v=%u — dom-tree DFS "
                                "scope invariant violation.\n",
                                canonicalIdx, i, B.v);
                            std::abort();
                        }
                        if (storesClobber(B, canonicalIdx + 1, i)) {
                            admit = false;
                        }
                    } else {
                        // Different-block: scan
                        //   (a) canonical's block tail (after canonical)
                        //   (b) strictly-between region
                        //   (c) useBlock's head (before current)
                        if (storesClobber(canonicalBlock, canonicalIdx + 1, cn)) {
                            admit = false;
                        }
                        if (admit
                            && clobbers.anyClobberBetween(
                                   interner_, loadPtr, canonicalBlock, B,
                                   strictTbaa_, charTypesAliasAll_)) {
                            admit = false;
                        }
                        if (admit && storesClobber(B, 0, i)) {
                            admit = false;
                        }
                        // (d) WRAP-AROUND slice (D-OPT-CSE-LOAD-BACKEDGE-TAIL).
                        // Slices (a)-(c) cover only an ACYCLIC canonical→use path.
                        // If the use sits on a cycle that does NOT re-execute the
                        // canonical, execution WRAPS: Load(iter N) → B's TAIL →
                        // back-edge → Load(iter N+1). A may-aliasing Store in B's
                        // tail therefore clobbers the NEXT iteration's Load, and
                        // NOTHING above scans [i+1, ninst) — (b) drops both
                        // endpoints and (c) stops at `i`. That gap is the sqlite
                        // `balance_nonroot` silent miscompile: its loop body holds
                        // both this Load and the store that advances the pointer.
                        // Cheap index scan FIRST (usually a map miss); the
                        // reachability walk runs only when a clobber is actually
                        // there.
                        //
                        // RE-EXECUTION LEMMA (why the other two regions need no
                        // slice) — stated over the MODELED CFG, where every edge
                        // originates at a terminator (an `IndirectBr` lists all
                        // address-taken blocks as successors, and the SEH region
                        // ops are themselves terminators AND memory clobbers, so
                        // no mid-block fault edge can smuggle a path past this):
                        // a block is entered only at index 0, so any wrap
                        // that re-enters canonicalBlock re-executes the canonical
                        // and REFRESHES the value. Hence canonicalBlock's head
                        // [0, canonicalIdx) is not a hole, and the same-block
                        // branch above (canonicalIdx < i) is already exact.
                        // Scanning either would only lose precision. That
                        // admission carries one obligation — the canonical must
                        // stay inside the cycle — which LICM discharges: its Load
                        // hoist gate scans the WHOLE loop body (licm.cpp), so it
                        // refuses to hoist exactly when such a clobber exists.
                        if (admit
                            && storesClobber(B, i + 1, ninst)
                            && clobbers.blockReachesItselfAvoiding(B, canonicalBlock)) {
                            admit = false;
                        }
                    }
                }
                if (admit) {
                    cseMap_[id] = it->second;
                    ++instructionsCsed_;
                    continue;
                }
                // Fall through. NOTE (corrected — the old comment here claimed
                // this Load "becomes the new canonical", which is FALSE):
                // `scope.emplace` below is a NO-OP when the key already exists,
                // so the STALE canonical survives a rejected candidate. That is
                // SOUND — a later identical Load re-runs the full gate against
                // that stale canonical, and the same clobber that rejected this
                // one rejects it too — but IMPRECISE: two Loads that both sit
                // AFTER the clobber cannot CSE against each other, because
                // neither ever became canonical. Replacing the canonical here
                // is NOT a local edit: `log` records keys for scope-exit
                // rollback (erase), so an overwrite would need save/restore of
                // the previous binding. Precision only, never a miscompile:
                // anchored `D-OPT-CSE-STALE-CANONICAL-AFTER-REJECT`.
            }
            log.push_back(k);
            scope.emplace(std::move(k), id);
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

    if (cloneGlobalsOrCarveOut(mir, builder, reporter, "Cse")
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
