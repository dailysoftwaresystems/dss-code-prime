#include "opt/passes/cse.hpp"

#include "mir/mir_cfg.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/mir_alias.hpp"
#include "opt/passes/mir_rebuild_helper.hpp"
#include "opt/passes/path_compress.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dss::opt::passes {

namespace {

using dss::opt::analysis::MirAliasResult;
using dss::opt::analysis::StrictTbaa;
using dss::opt::analysis::mirMayAlias;
using dss::opt::analysis::mirRegionBetween;
using dss::opt::analysis::mirAnyMayAliasingStoreInRegion;

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
// site additionally walks `mirAnyMayAliasingStoreInRegion` between
// the canonical Load's block and the current Load's block before
// admitting the CSE — this is the alias-safety gate that replaces
// the prior blanket exclusion.
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
                      ? StrictTbaa::Yes : StrictTbaa::No) {}

    [[nodiscard]] std::size_t instructionsCsed() const noexcept {
        return instructionsCsed_;
    }

    void analyze(MirFuncId fn);

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
    // Old-id → canonical-old-id. Built by analyze() via dom-tree DFS
    // with a scoped value-numbering table; path-compressed after.
    std::unordered_map<MirInstId, MirInstId> cseMap_;
    std::size_t instructionsCsed_ = 0;
};

void CsePolicy::analyze(MirFuncId fn) {
    resetPerFunction();
    std::uint32_t const blockCount = src_.funcBlockCount(fn);
    if (blockCount == 0) return;

    // Build the dom tree. CSE only walks reachable blocks (RPO).
    MirBlockId const entry = src_.funcEntry(fn);
    auto const rpo = mirReversePostOrder(src_, entry);
    auto const preds = mirBuildPredecessors(src_);
    auto const dom = computeMirDomTree(src_, entry, rpo, preds);
    auto const dchild = mirDomTreeChildren(src_, dom);

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
                // sits anywhere between them. We scan three slices
                // owned by the caller (this site) plus the strictly-
                // between region owned by `mirRegionBetween`:
                //   — canonical's block tail (after canonical, to end)
                //   — strictly-between blocks (region walker)
                //   — useBlock's head (start, up to current)
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

                    auto storesClobber = [&](MirBlockId blk,
                                             std::uint32_t lo,
                                             std::uint32_t hi) -> bool {
                        for (std::uint32_t j = lo; j < hi; ++j) {
                            MirInstId const sid = src_.blockInstAt(blk, j);
                            if (src_.instOpcode(sid) != MirOpcode::Store) continue;
                            auto const sops = src_.instOperands(sid);
                            if (sops.size() < 2) {
                                std::fprintf(stderr,
                                    "dss::opt::passes::Cse fatal: "
                                    "Store inst v=%u has fewer than "
                                    "2 operands — verifier-contract "
                                    "violation.\n", sid.v);
                                std::abort();
                            }
                            if (mirMayAlias(src_, interner_,
                                            loadPtr, sops[1],
                                            strictTbaa_)
                                != MirAliasResult::No) {
                                return true;
                            }
                        }
                        return false;
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
                        if (admit) {
                            auto const region = mirRegionBetween(
                                src_, canonicalBlock, B);
                            if (mirAnyMayAliasingStoreInRegion(
                                    src_, interner_, loadPtr, region,
                                    strictTbaa_)) {
                                admit = false;
                            }
                        }
                        if (admit && storesClobber(B, 0, i)) {
                            admit = false;
                        }
                    }
                }
                if (admit) {
                    cseMap_[id] = it->second;
                    ++instructionsCsed_;
                    continue;
                }
                // Fall through to insert this Load as a new canonical
                // — a later identical Load may still CSE against it
                // if no aliasing Store separates THEM.
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
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        policy.analyze(f);
        MirFunctionRebuilder rb{mir, builder, policy};
        rb.rebuildFunction(f);
    }

    result.instructionsCsed = policy.instructionsCsed();
    mir = std::move(builder).finish();
    result.ok = true;
    return result;
}

} // namespace dss::opt::passes
