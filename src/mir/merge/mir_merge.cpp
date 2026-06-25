#include "mir/merge/mir_merge.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_reintern.hpp"
#include "link/cross_cu_resolve.hpp"   // resolveCrossCuDefs, CrossCuDef, LinkedSymbolKey
#include "link/symbol_kind.hpp"        // LinkedSymbolKey
#include "mir/mir_cfg.hpp"             // mirReversePostOrder
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"  // rederiveStructCfMarkers (post-merge stamp)
#include "mir/mir_verifier.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>      // std::get_if — F5 symbol-address literal remap
#include <vector>

namespace dss {

namespace {

// ── unified SymbolId allocation ────────────────────────────────────
//
// The merge mints ONE merged SymbolId per distinct externally-visible NAME
// (cross-CU same-name defs/refs collapse to the winner's id) and one fresh
// merged id per module-private (Local / no-name) symbol and per surviving
// real-FFI extern. CU0's symbol VALUES are preferentially retained (a driver-
// + debug-stability nicety); CU1..N and any colliding allocation mint fresh
// ids from a counter seeded above CU0's max.
class SymbolAllocator {
public:
    explicit SymbolAllocator(std::uint32_t seedNextFresh) : next_(seedNextFresh) {}

    // Claim a specific value (CU0 retention). Caller guarantees it is free.
    [[nodiscard]] SymbolId claim(std::uint32_t v) {
        if (!used_.insert(v).second) {
            std::fprintf(stderr,
                "dss::mergeCuMirs fatal: SymbolAllocator::claim(%u) — value "
                "already taken (unified-symbol allocation invariant).\n", v);
            std::abort();
        }
        return SymbolId{v};
    }
    // Mint the next free id.
    [[nodiscard]] SymbolId mint() {
        while (used_.count(next_)) ++next_;
        std::uint32_t const v = next_++;
        used_.insert(v);
        return SymbolId{v};
    }
    [[nodiscard]] bool isFree(std::uint32_t v) const { return used_.count(v) == 0; }

private:
    std::uint32_t                     next_;
    std::unordered_set<std::uint32_t> used_;
};

// (cuIdx, oldSymbol.v) → merged SymbolId.
struct CuSymKey {
    std::uint32_t cuIdx;
    std::uint32_t symV;
    [[nodiscard]] bool operator==(CuSymKey const& o) const noexcept {
        return cuIdx == o.cuIdx && symV == o.symV;
    }
};
struct CuSymKeyHash {
    [[nodiscard]] std::size_t operator()(CuSymKey const& k) const noexcept {
        std::size_t h = std::hash<std::uint32_t>{}(k.cuIdx);
        h ^= std::hash<std::uint32_t>{}(k.symV) + 0x9e3779b97f4a7c15ULL
             + (h << 6) + (h >> 2);
        return h;
    }
};

// All the cross-CU-resolved state the clone reads.
struct MergePlan {
    // Per-CU type-reintern memo (reused across that CU's functions/globals).
    std::vector<std::unordered_map<std::uint32_t, TypeId>> typeRemap;
    // (cuIdx, oldSym.v) → merged SymbolId. Covers func defs, global defs, AND
    // every extern import symbol.
    std::unordered_map<CuSymKey, SymbolId, CuSymKeyHash> symMerged;
    // Externally-visible NAME → its single canonical merged SymbolId.
    std::unordered_map<std::string, SymbolId> canonicalForName;
    // Real-FFI extern mangledName → its single canonical merged SymbolId, shared
    // across CUs. Two CUs importing the SAME library symbol (e.g. both
    // `#include <stdio.h>` → both have a `puts` ExternImport, with NO cross-CU
    // definition) must collapse to ONE merged symbol so the merged module carries
    // exactly ONE import row (one IAT slot) and both CUs' `GlobalAddr(externSym)`
    // resolve to it. Disjoint from `canonicalForName`: a name that IS a defined
    // winner never lands here (it rewires to the direct def instead).
    std::unordered_map<std::string, SymbolId> ffiCanonicalForName;
    // merged-symbol .v → declared name (for the lower half's symtab populate).
    std::unordered_map<std::uint32_t, std::string> symbolNames;
    // (cuIdx, oldFunc.v) → merged MirFuncId — populated as functions are
    // cloned; consulted when a global's initFunc must be remapped.
    std::unordered_map<CuSymKey, MirFuncId, CuSymKeyHash> funcMerged;
    // Names that have a cross-CU winner DEFINITION (so an extern reference to
    // such a name rewires to the winner — a DIRECT intra-module call).
    std::unordered_set<std::string> definedNames;
};

[[noreturn]] void mergeFatal(char const* what) {
    std::fputs("dss::mergeCuMirs fatal: ", stderr);
    std::fputs(what, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Look up the merged SymbolId for (cuIdx, oldSym). Fail loud on a miss — every
// symbol referenced by a cloned instruction must have been assigned a merged id
// in the planning pass (a miss is a cross-module operand the plan didn't see).
[[nodiscard]] SymbolId
mergedSymbolOf(MergePlan const& plan, std::uint32_t cuIdx, SymbolId oldSym) {
    auto const it = plan.symMerged.find(CuSymKey{cuIdx, oldSym.v});
    if (it == plan.symMerged.end()) {
        std::fprintf(stderr,
            "dss::mergeCuMirs fatal: GlobalAddr in CU %u references symbol "
            "v=%u with no unified merged id — the symbol was not seen during "
            "planning (cross-module reference the plan missed).\n",
            cuIdx, oldSym.v);
        std::abort();
    }
    return it->second;
}

// F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): a symbol-address init literal embeds a
// per-CU SymbolId in `MirSymbolAddrValue.symbol` (an `int* p = &target;` /
// `char* g = "...";` / function-pointer-table global). The merge RENUMBERS
// symbols, so this raw id must be remapped through `mergedSymbolOf` — the literal
// analogue of `mergedGlobalAddrSymbol` for the GlobalAddr INSTRUCTION form.
// Recurses through aggregate fields (a future `static int* a[] = {&x, &y};`).
// WITHOUT this remap the global's abs64 reloc targets a STALE CU-local id in any
// multi-`.c` build → linker `K_SymbolUndefined` (lucky) or a silently-wrong VA
// (id collision) — a pointer miscompile invisible to a single-CU corpus.
void remapLiteralSymbols(MirLiteralValue& lit, MergePlan const& plan,
                         std::uint32_t cuIdx) {
    if (auto* sa = std::get_if<MirSymbolAddrValue>(&lit.value)) {
        sa->symbol = mergedSymbolOf(plan, cuIdx, SymbolId{sa->symbol}).v;
    } else if (auto* agg = std::get_if<MirAggregateValue>(&lit.value)) {
        for (MirLiteralValue& f : agg->fields)
            remapLiteralSymbols(f, plan, cuIdx);
    }
}

// ── one-function clone into the shared builder ─────────────────────
//
// Extends the inliner's RPO clone (opt/passes/inlining.cpp
// `MultiBlockInliner::rebuildFunction`) but intercepts EVERY `instType` with
// `reinternType` (into the host lattice) and EVERY GlobalAddr / func symbol with
// the unified `symMerged` map (cross-CU names already collapsed to the winner).
// Unlike the inliner this does NOT inline — Call / IntrinsicCall are copied
// verbatim (with operands + payload + reinterned type), so the merged module
// preserves each CU's call structure (a cross-CU call's GlobalAddr now points at
// the in-module winner — a direct call).
class FunctionCloner {
public:
    FunctionCloner(Mir const& src, TypeInterner const& srcInterner,
                   std::uint32_t cuIdx, MergePlan& plan, TypeLattice& host,
                   MirBuilder& dst)
        : src_(src), srcInterner_(srcInterner), cuIdx_(cuIdx), plan_(plan),
          host_(host), dst_(dst) {}

    // Clone `f` into `dst_`. Returns the merged MirFuncId.
    [[nodiscard]] MirFuncId clone(MirFuncId f, SymbolId mergedSymbol) {
        TypeId const sig = reinternType(srcInterner_, src_.funcSignature(f),
                                        host_, typeRemap());
        MirFuncId const newF = dst_.addFunction(
            sig, mergedSymbol, src_.funcBinding(f), src_.funcVisibility(f));
        plan_.funcMerged.emplace(CuSymKey{cuIdx_, src_.funcSymbol(f).v}, newF);

        std::uint32_t const nb = src_.funcBlockCount(f);

        // Phase 1: pre-create every block in NATURAL order (so block 0 stays the
        // entry, markers + indices match the source 1:1) — terminators target
        // forward blocks (loop back-edges) as forward references.
        blockMap_.clear();
        local_.clear();
        deferredPhis_.clear();
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const oldB = src_.funcBlockAt(f, bi);
            MirBlockId const newB = dst_.createBlock(src_.blockMarker(oldB));
            blockMap_.emplace(oldB.v, newB);
        }

        // Phase 2: fill blocks in RPO from the entry (a valid def-before-use
        // order for SSA — a block's dominators precede it, so the function-wide
        // `local_` map is always populated before a use). Pre-created blocks not
        // reached by RPO (unreachable) are filled afterward defensively.
        std::vector<MirBlockId> const rpo =
            mirReversePostOrder(src_, src_.funcEntry(f));
        std::unordered_set<std::uint32_t> filled;
        for (MirBlockId const oldB : rpo) {
            fillBlock(f, oldB);
            filled.insert(oldB.v);
        }
        // Any pre-created-but-unfilled block would leave the MirBuilder's
        // every-block-must-be-filled invariant violated → finish() aborts. The
        // merge targets reachable-only CFGs (optimized MIR + the hand-built
        // tests); fail loud here rather than rely on the builder's late abort.
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const oldB = src_.funcBlockAt(f, bi);
            if (!filled.count(oldB.v)) {
                std::fprintf(stderr,
                    "dss::mergeCuMirs fatal: CU %u function symbol v=%u block "
                    "v=%u is unreachable from the entry — the merge clones the "
                    "reachable CFG only (run DCE before merge to drop dead "
                    "blocks; D-MERGE-UNREACHABLE-BLOCK).\n",
                    cuIdx_, src_.funcSymbol(f).v, oldB.v);
                std::abort();
            }
        }

        // Phase 3: flush phi incomings (values + preds mapped post-fill so loop
        // back-edge values resolve).
        for (DeferredPhi const& dp : deferredPhis_) {
            for (MirPhiIncoming const& inc : src_.phiIncomings(dp.oldPhi)) {
                MirInstId const newVal = mapValue(inc.value, dp.oldPhi);
                MirBlockId const newPred = mapBlock(inc.pred);
                dst_.addPhiIncoming(dp.newPhi, MirPhiIncoming{newVal, newPred});
            }
        }
        return newF;
    }

private:
    struct DeferredPhi {
        MirInstId oldPhi;
        MirInstId newPhi;
    };

    [[nodiscard]] std::unordered_map<std::uint32_t, TypeId>& typeRemap() {
        return plan_.typeRemap[cuIdx_];
    }

    [[nodiscard]] TypeId reType(TypeId t) {
        return reinternType(srcInterner_, t, host_, typeRemap());
    }

    [[nodiscard]] MirBlockId mapBlock(MirBlockId oldB) {
        auto const it = blockMap_.find(oldB.v);
        if (it == blockMap_.end()) {
            std::fprintf(stderr,
                "dss::mergeCuMirs fatal: CU %u block v=%u not pre-created — "
                "every block is created in phase 1.\n", cuIdx_, oldB.v);
            std::abort();
        }
        return it->second;
    }

    [[nodiscard]] MirInstId mapValue(MirInstId oldV, MirInstId user) {
        auto const it = local_.find(oldV.v);
        if (it == local_.end()) {
            std::fprintf(stderr,
                "dss::mergeCuMirs fatal: CU %u inst v=%u operand v=%u has no "
                "clone mapping — RPO/def-before-use violation.\n",
                cuIdx_, user.v, oldV.v);
            std::abort();
        }
        return it->second;
    }

    void fillBlock(MirFuncId f, MirBlockId oldB) {
        (void)f;
        dst_.beginBlock(mapBlock(oldB));
        std::uint32_t const ni = src_.blockInstCount(oldB);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const id = src_.blockInstAt(oldB, ii);
            MirOpcode const op = src_.instOpcode(id);

            if (op == MirOpcode::Phi) {
                MirInstId const newPhi = dst_.addPhi(reType(src_.instType(id)));
                local_.emplace(id.v, newPhi);
                deferredPhis_.push_back({id, newPhi});
                continue;
            }
            if (opcodeInfo(op).isTerminator) {
                emitTerminator(op, id, oldB);
                break;  // terminator is the last instruction
            }
            emitValue(op, id);
        }
    }

    // Re-emit one value-producing (non-Phi, non-terminator) instruction. Arg /
    // Const / GlobalAddr use their dedicated builders (each owns a distinct
    // payload encoding — argIndex / literalIndex / symbol — that a raw addInst
    // would mis-stamp); every other opcode (Call, Load, Store, Add, Alloca,
    // IntrinsicCall, ExtractValue, ...) re-emits generically with operands mapped
    // through `local_` and `payload`/`flags` copied verbatim.
    void emitValue(MirOpcode op, MirInstId id) {
        if (op == MirOpcode::Arg) {
            local_.emplace(id.v,
                dst_.addArg(src_.argIndex(id), reType(src_.instType(id)),
                            src_.instFlags(id)));
            return;
        }
        if (op == MirOpcode::Const) {
            // Copy + remap the literal so a symbol-address value's embedded
            // SymbolId is rewritten into the merged id space — SYMMETRY with the
            // step-5 global path (F5 remapLiteralSymbols). Today no value-position
            // `Const` carries a MirSymbolAddrValue (it is only ever a global
            // initializer), but routing BOTH literal-copy sites through the one
            // shared remap closes the missed-site class BY CONSTRUCTION rather than
            // leaving a silent stale-id twin (the FC7 clone-site miscompile class).
            MirLiteralValue lit = src_.literalValue(src_.constLiteralIndex(id));
            remapLiteralSymbols(lit, plan_, cuIdx_);
            local_.emplace(id.v, dst_.addConst(
                std::move(lit), reType(src_.instType(id)), src_.instFlags(id)));
            return;
        }
        if (op == MirOpcode::GlobalAddr) {
            local_.emplace(id.v, dst_.addGlobalAddr(
                mergedGlobalAddrSymbol(id), reType(src_.instType(id)),
                src_.instFlags(id)));
            return;
        }
        if (op == MirOpcode::BlockAddress) {
            // D-CSUBSET-COMPUTED-GOTO: the payload is a BLOCK id, which the merge
            // RE-NUMBERS — a generic `addInst` would copy it verbatim and point the
            // address at the WRONG (or a stale) block (the FC7 clone-site silent-
            // miscompile class, extended to BlockAddress). Re-map via `mapBlock`.
            local_.emplace(id.v, dst_.addBlockAddress(
                mapBlock(src_.blockAddressTarget(id)), reType(src_.instType(id)),
                src_.instFlags(id)));
            return;
        }
        auto const ops = src_.instOperands(id);
        std::vector<MirInstId> newOps;
        newOps.reserve(ops.size());
        for (MirInstId const o : ops) newOps.push_back(mapValue(o, id));
        local_.emplace(id.v, dst_.addInst(op, newOps, reType(src_.instType(id)),
                                          src_.instPayload(id), src_.instFlags(id)));
    }

    // The merged symbol a GlobalAddr should reference. A GlobalAddr naming a
    // symbol with a cross-CU winner DEFINITION resolves to that winner's merged
    // id (so a call through it becomes a DIRECT intra-module call); otherwise it
    // resolves to the symbol's own unified-remapped id (an intra-CU reference or
    // a surviving real-FFI extern).
    [[nodiscard]] SymbolId mergedGlobalAddrSymbol(MirInstId id) {
        SymbolId const oldSym = src_.globalAddrSymbol(id);
        return mergedSymbolOf(plan_, cuIdx_, oldSym);
    }

    void emitTerminator(MirOpcode op, MirInstId id, MirBlockId oldB) {
        auto const ops  = src_.instOperands(id);
        auto const succ = src_.blockSuccessors(oldB);
        switch (op) {
            case MirOpcode::Br:
                dst_.addBr(mapBlock(succ[0]));
                return;
            case MirOpcode::CondBr:
                dst_.addCondBr(mapValue(ops[0], id),
                               mapBlock(succ[0]), mapBlock(succ[1]));
                return;
            case MirOpcode::Switch: {
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                std::size_t const ncases = succ.size() - 1;
                cases.reserve(ncases);
                for (std::size_t i = 0; i < ncases; ++i) {
                    cases.emplace_back(mapValue(ops[1 + i], id),
                                       mapBlock(succ[i]));
                }
                dst_.addSwitch(mapValue(ops[0], id), cases,
                               mapBlock(succ[ncases]));
                return;
            }
            case MirOpcode::Return: {
                // FC7 C1c: a by-value struct returned IN REGISTERS carries N
                // eightbyte/HFA PIECES (every operand is a return-register value),
                // not just one. The clone MUST map EVERY operand — taking only
                // ops[0] silently dropped pieces 1..N-1, a miscompile masked on
                // x86_64 only because the dropped piece's value often still aliased
                // its arg register at the return reg (e.g. a 3rd field passed in rdx
                // == returnGprs[1]); AAPCS64's distinct arg/return mapping exposed it.
                // `addReturnMulti` handles 0 (void), 1 (scalar), and N (pieces).
                std::vector<MirInstId> rvs;
                rvs.reserve(ops.size());
                for (MirInstId const o : ops) rvs.push_back(mapValue(o, id));
                dst_.addReturnMulti(rvs);
                return;
            }
            case MirOpcode::Unreachable:
                dst_.addUnreachable();
                return;
            case MirOpcode::IndirectBr: {
                // D-CSUBSET-COMPUTED-GOTO: ★ THE SILENT-MISCOMPILE CLONE SITE (MF-A).
                // Re-map BOTH the address operand AND every successor — dropping any
                // successor would delete an address-taken edge (reachability/DCE
                // would then prune a live `&&label` target). operand[0] = address;
                // successors = all address-taken blocks.
                std::vector<MirBlockId> targets;
                targets.reserve(succ.size());
                for (MirBlockId const b : succ) targets.push_back(mapBlock(b));
                dst_.addIndirectBr(mapValue(ops[0], id), targets);
                return;
            }
            default:
                std::fprintf(stderr,
                    "dss::mergeCuMirs fatal: CU %u terminator opcode %d marked "
                    "isTerminator but has no clone arm.\n",
                    cuIdx_, static_cast<int>(op));
                std::abort();
        }
    }

    Mir const&          src_;
    TypeInterner const& srcInterner_;
    std::uint32_t       cuIdx_;
    MergePlan&          plan_;
    TypeLattice&        host_;
    MirBuilder&         dst_;
    std::unordered_map<std::uint32_t, MirBlockId> blockMap_;
    std::unordered_map<std::uint32_t, MirInstId>  local_;
    std::vector<DeferredPhi>                       deferredPhis_;
};

} // namespace

std::optional<MergedMirModule>
mergeCuMirs(std::span<MergeCuInput const> cus, TypeLattice&& host,
            std::span<std::string const> entryNames, DiagnosticReporter& reporter) {
    if (cus.empty()) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "mergeCuMirs: received no compilation units to merge.";
        reporter.report(std::move(d));
        return std::nullopt;
    }
    for (std::size_t i = 0; i < cus.size(); ++i) {
        if (cus[i].mir == nullptr || cus[i].interner == nullptr || !cus[i].nameOf) {
            mergeFatal("a MergeCuInput is missing its mir / interner / nameOf "
                       "(decomposed-input contract violation).");
        }
    }

    MergePlan plan;
    plan.typeRemap.resize(cus.size());

    // ── (1)+(2) name → defining (cuIdx, MirFuncId, binding) + resolveCrossCuDefs.
    // A LinkedSymbolKey's cuId is the synthetic `cuIdx+1` (unique per CU, order-
    // stable); `cuIdxOf(key) == key.cuId.v - 1`. Only externally-visible
    // (Global/Weak) function definitions feed the resolver — Local stays module-
    // private (resolveCrossCuDefs filters Local, but pre-filtering keeps the def
    // list honest about what crosses CU boundaries).
    std::vector<linker::CrossCuDef> defs;
    // (cuIdx, MirFuncId.v) for the winning key of each externally-visible name —
    // so we can decide whether a function is the winner (keep) or a shadowed
    // loser (skip its body).
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            std::string const name = cus[ci].nameOf(m.funcSymbol(f));
            if (name.empty()) continue;
            SymbolBinding const binding = m.funcBinding(f);
            if (binding == SymbolBinding::Local) continue;
            plan.definedNames.insert(name);
            defs.push_back(linker::CrossCuDef{
                name, binding,
                LinkedSymbolKey{CompilationUnitId{ci + 1}, m.funcSymbol(f)}});
        }
        // Externally-visible globals participate in cross-CU resolution too (a
        // strong global shadows a weak one of the same name), exactly like
        // functions.
        std::size_t const ng = m.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            std::string const name = cus[ci].nameOf(m.globalSymbol(g));
            if (name.empty()) continue;
            SymbolBinding const binding = m.globalBinding(g);
            if (binding == SymbolBinding::Local) continue;
            plan.definedNames.insert(name);
            defs.push_back(linker::CrossCuDef{
                name, binding,
                LinkedSymbolKey{CompilationUnitId{ci + 1}, m.globalSymbol(g)}});
        }
    }

    linker::CrossCuResolution const resolution =
        linker::resolveCrossCuDefs(defs);

    // (2) report each two-strong conflict (one per collision event — mirrors the
    // linker's per-pair K_SymbolRedefinedAcrossUnits count).
    for (linker::CrossCuConflict const& c : resolution.conflicts) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_SymbolRedefinedAcrossUnits;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "mergeCuMirs: symbol '" + c.name +
                     "' has multiple strong (global) definitions across "
                     "compilation units (CU #" +
                     std::to_string(c.existing.cuId.v) + " and CU #" +
                     std::to_string(c.incoming.cuId.v) + ").";
        reporter.report(std::move(d));
    }

    // ── (3) unified SymbolId space. CU0 retains its symbol values; CU1..N (and
    // any collision) mint fresh ids seeded above CU0's max.
    std::uint32_t maxCu0 = 0;
    {
        Mir const& m0 = *cus[0].mir;
        std::size_t const nf = m0.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            maxCu0 = std::max(maxCu0, m0.funcSymbol(m0.funcAt(fi)).v);
        }
        std::size_t const ng = m0.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            maxCu0 = std::max(maxCu0, m0.globalSymbol(m0.globalAt(gi)).v);
        }
        for (ExternImport const& e : cus[0].externImports) {
            maxCu0 = std::max(maxCu0, e.symbol.v);
        }
    }
    SymbolAllocator alloc{maxCu0 + 1};

    auto cuIdxOf = [](LinkedSymbolKey const& k) -> std::uint32_t {
        return k.cuId.v - 1;
    };

    // (3a) one canonical merged id per externally-visible winner NAME. Use the
    // winner's natural value when it is a free CU0 value; otherwise mint fresh.
    for (auto const& [name, winKey] : resolution.winners) {
        SymbolId merged;
        if (cuIdxOf(winKey) == 0 && alloc.isFree(winKey.symbol.v)) {
            merged = alloc.claim(winKey.symbol.v);
        } else {
            merged = alloc.mint();
        }
        plan.canonicalForName.emplace(name, merged);
        plan.symbolNames.emplace(merged.v, name);
    }

    // (3b) assign a merged id to EVERY defined symbol (func + global) + every
    // extern import across all CUs. `isFfiExtern` is true only for an
    // ExternImport row, so a surviving real-FFI extern (its name has no cross-CU
    // def) collapses across CUs to one canonical merged id by mangledName.
    auto assignSymbol = [&](std::uint32_t ci, SymbolId oldSym,
                            std::string const& name, bool isFfiExtern) {
        CuSymKey const key{ci, oldSym.v};
        if (plan.symMerged.count(key)) return;  // already assigned (idempotent)
        if (!name.empty()) {
            auto const it = plan.canonicalForName.find(name);
            if (it != plan.canonicalForName.end()) {
                // Externally-visible name (def winner / shadowed loser / an
                // extern reference to a cross-CU def) → collapse to the winner.
                plan.symMerged.emplace(key, it->second);
                return;
            }
            if (isFfiExtern) {
                // A surviving real-FFI extern (no cross-CU def of this name).
                // Collapse same-mangledName externs across CUs to ONE merged
                // symbol — the first occurrence mints it; later CUs reuse it —
                // so the merged module emits exactly one import row per name.
                auto const fit = plan.ffiCanonicalForName.find(name);
                if (fit != plan.ffiCanonicalForName.end()) {
                    plan.symMerged.emplace(key, fit->second);
                    return;
                }
            }
        }
        // Module-private (Local / no-name) OR the FIRST surviving real-FFI extern
        // of a given name.
        SymbolId merged;
        if (ci == 0 && alloc.isFree(oldSym.v)) {
            merged = alloc.claim(oldSym.v);
        } else {
            merged = alloc.mint();
        }
        plan.symMerged.emplace(key, merged);
        if (!name.empty()) {
            plan.symbolNames.emplace(merged.v, name);
            if (isFfiExtern) plan.ffiCanonicalForName.emplace(name, merged);
        }
    };

    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            assignSymbol(ci, m.funcSymbol(f), cus[ci].nameOf(m.funcSymbol(f)),
                         /*isFfiExtern=*/false);
        }
        std::size_t const ng = m.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            assignSymbol(ci, m.globalSymbol(g), cus[ci].nameOf(m.globalSymbol(g)),
                         /*isFfiExtern=*/false);
        }
        for (ExternImport const& e : cus[ci].externImports) {
            // An extern's name is its mangledName (nameOf must agree, but the
            // import row is authoritative for the on-binary name).
            assignSymbol(ci, e.symbol, e.mangledName, /*isFfiExtern=*/true);
        }
    }

    // ── (4) clone every SURVIVING function (skip a cross-CU loser whose name's
    // winner is a DIFFERENT (cuIdx, sym)) into ONE builder over the host lattice.
    MirBuilder builder;
    // Carry CU0's module-level alias polarity (the driver merges homogeneous-
    // language CUs; CU0 is representative). Agnostic — a flag value, no branch.
    builder.setAliasingMode(cus[0].mir->aliasingMode());
    builder.setCharTypesAliasAll(cus[0].mir->charTypesAliasAll());

    auto isShadowedLoser = [&](std::uint32_t ci, std::string const& name,
                               SymbolId sym) -> bool {
        if (name.empty()) return false;
        auto const it = resolution.winners.find(name);
        if (it == resolution.winners.end()) return false;  // not externally visible
        LinkedSymbolKey const& win = it->second;
        return !(cuIdxOf(win) == ci && win.symbol.v == sym.v);
    };

    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            std::string const name = cus[ci].nameOf(m.funcSymbol(f));
            if (isShadowedLoser(ci, name, m.funcSymbol(f))) continue;  // weak loser
            SymbolId const mergedSym =
                mergedSymbolOf(plan, ci, m.funcSymbol(f));
            FunctionCloner cloner{m, *cus[ci].interner, ci, plan, host, builder};
            (void)cloner.clone(f, mergedSym);
        }
    }

    // ── (5) merge globals (skip shadowed-weak losers). The initFunc MirFuncId
    // is remapped into the merged func space via `plan.funcMerged` (populated in
    // step 4); initLiteral is re-added by value.
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        std::size_t const ng = m.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            std::string const name = cus[ci].nameOf(m.globalSymbol(g));
            if (isShadowedLoser(ci, name, m.globalSymbol(g))) continue;

            TypeId const ty = reinternType(*cus[ci].interner, m.globalType(g),
                                           host, plan.typeRemap[ci]);
            SymbolId const mergedSym = mergedSymbolOf(plan, ci, m.globalSymbol(g));

            std::uint32_t initLit = m.globalInitLiteralIndex(g);
            std::uint32_t newInitLit = UINT32_MAX;
            if (initLit != UINT32_MAX) {
                // Copy the init literal so a symbol-address value's embedded
                // per-CU SymbolId can be remapped into the merged id space
                // (F5 — see remapLiteralSymbols). A plain by-value re-add would
                // carry a stale id → silently-wrong abs64 reloc target.
                MirLiteralValue lit = m.literalValue(initLit);
                remapLiteralSymbols(lit, plan, ci);
                newInitLit = builder.literalPoolAdd(std::move(lit));
            }

            MirFuncId newInitFunc{};
            MirFuncId const oldInitFunc = m.globalInitFunc(g);
            if (oldInitFunc.valid()) {
                auto const it =
                    plan.funcMerged.find(CuSymKey{ci, m.funcSymbol(oldInitFunc).v});
                if (it == plan.funcMerged.end()) {
                    std::fprintf(stderr,
                        "dss::mergeCuMirs fatal: CU %u global symbol v=%u "
                        "initFunc (func symbol v=%u) was not cloned — a global's "
                        "init function must survive the merge "
                        "(D-MERGE-GLOBAL-INITFUNC).\n",
                        ci, m.globalSymbol(g).v, m.funcSymbol(oldInitFunc).v);
                    std::abort();
                }
                newInitFunc = it->second;
            }

            (void)builder.addGlobal(ty, mergedSym, newInitLit, newInitFunc,
                                    m.globalBinding(g), m.globalVisibility(g),
                                    m.globalIsConst(g));
        }
    }

    // ── (6) surviving externImports: an extern whose mangledName has NO cross-CU
    // winner DEFINITION stays a real FFI import (carried, symbol unified-
    // remapped); a cross-CU-resolved extern is STRIPPED (its calls were rewired
    // to direct in step 4). Dedup by merged symbol: step 3b's
    // `ffiCanonicalForName` collapse already gave two CUs importing the SAME
    // library symbol ONE merged id, so this id-keyed dedup emits exactly one row
    // per name (one IAT slot).
    std::vector<ExternImport> survivingExterns;
    std::unordered_set<std::uint32_t> emittedExternSym;
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        for (ExternImport const& e : cus[ci].externImports) {
            if (plan.definedNames.count(e.mangledName)) continue;  // → direct, strip
            SymbolId const mergedSym = mergedSymbolOf(plan, ci, e.symbol);
            if (!emittedExternSym.insert(mergedSym.v).second) continue;  // deduped
            ExternImport carried = e;
            carried.symbol = mergedSym;
            survivingExterns.push_back(std::move(carried));
        }
    }

    // ── (7) userEntrySymbol: the merged symbol of the function whose name is in
    // the grammar's entry-name list.
    std::optional<SymbolId> userEntrySymbol;
    {
        std::unordered_set<std::string> entrySet(entryNames.begin(), entryNames.end());
        for (std::uint32_t ci = 0; ci < cus.size() && !userEntrySymbol; ++ci) {
            Mir const& m = *cus[ci].mir;
            std::size_t const nf = m.moduleFuncCount();
            for (std::uint32_t fi = 0; fi < nf; ++fi) {
                MirFuncId const f = m.funcAt(fi);
                std::string const name = cus[ci].nameOf(m.funcSymbol(f));
                if (name.empty() || !entrySet.count(name)) continue;
                if (isShadowedLoser(ci, name, m.funcSymbol(f))) continue;
                userEntrySymbol = mergedSymbolOf(plan, ci, m.funcSymbol(f));
                break;
            }
        }
    }

    Mir merged = std::move(builder).finish();

    // Canonical-marker stamping (D-OPT4-1): clones copy markers verbatim
    // and per-function CFGs are unchanged by the merge, so this is a
    // uniformity re-stamp — it keeps the equality verifier below green
    // BY CONSTRUCTION even when an input CU carried stale stamps.
    rederiveStructCfMarkers(merged);

    // ── verify the merged module before returning (the engine's verify-after-
    // every-transform discipline). A non-verifying merge is a build break, never
    // a silent miscompile.
    {
        MirVerifier verifier{merged, &host.interner()};
        if (!verifier.verify(reporter)) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = "mergeCuMirs: the merged whole-program module failed "
                         "MIR verification (structural / SSA / type invariant "
                         "broken by the merge).";
            reporter.report(std::move(d));
            return std::nullopt;
        }
    }

    MergedMirModule out{
        std::move(merged), std::move(host), std::move(plan.symbolNames),
        std::move(survivingExterns), userEntrySymbol};
    return out;
}

} // namespace dss
