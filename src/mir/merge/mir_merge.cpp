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
#include <format>       // ffiImportKey — the length-prefixed import-identity key
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

// ── D-LK11-EXTERN-IMPORT-DEDUP — the key that IDENTIFIES one dynamic symbol ──
//
// The MIR merge is the LIVE route: it is how `--compile a.c b.c` and every
// `--project` build fold their CUs (the assembled-tier `mergeModules` fold is
// reached only via `--resolve-library`). Two CUs' import rows may collapse onto
// ONE merged symbol + ONE import row only when they name the SAME dynamic
// symbol, and that identity is the TRIPLE (mangledName, libraryPath, version) --
// nothing weaker:
//   * `libraryPath` is IN the key: `foo` from `a.dll` and `foo` from `b.dll` are
//     DIFFERENT imports. It is the very field the walkers group DT_NEEDED /
//     IMAGE_IMPORT_DESCRIPTOR / LC_LOAD_DYLIB by, so folding across it silently
//     binds one CU's call sites into the OTHER library's export.
//   * `version` is IN the key: `puts@GLIBC_2.2.5` and `puts@GLIBC_2.17` are
//     genuinely different dynamic symbols (c156 D-LK-ELF-SYMBOL-VERSIONING).
//     Folding them reintroduces exactly the glibc compat-form misbind c156
//     exists to prevent -- a name-only key REGRESSES that fix.
// LENGTH-PREFIXED, never separator-joined: a mangledName is arbitrary bytes from
// a descriptor, so any separator-joined encoding is non-injective (two different
// triples could collide into one key and fold two UNRELATED imports).
// AGNOSTIC: structural equality over declared row data -- no language / arch /
// object-format branch; an IAT slot and a `.dynsym` row key by the same rule.
// The assembled-tier `mergeModules` (link/linker.cpp) keys IDENTICALLY, so the
// two tiers agree on what "one import" means and neither can re-split or
// re-fold what the other decided.
[[nodiscard]] std::string ffiImportKey(ExternImport const& e) {
    return std::format("{}:{}|{}:{}|{}:{}",
                       e.mangledName.size(), e.mangledName,
                       e.libraryPath.size(), e.libraryPath,
                       e.version.size(),     e.version);
}

// All the cross-CU-resolved state the clone reads.
struct MergePlan {
    // Per-CU type-reintern memo (reused across that CU's functions/globals).
    std::vector<std::unordered_map<std::uint32_t, TypeId>> typeRemap;
    // (cuIdx, oldSym.v) → merged SymbolId. Covers func defs, global defs, AND
    // every extern import symbol.
    std::unordered_map<CuSymKey, SymbolId, CuSymKeyHash> symMerged;
    // Externally-visible NAME → its single canonical merged SymbolId.
    std::unordered_map<std::string, SymbolId> canonicalForName;
    // Real-FFI import IDENTITY (`ffiImportKey` — the length-prefixed
    // (mangledName, libraryPath, version) triple) → its single canonical merged
    // SymbolId, shared across CUs. Two CUs importing the SAME library symbol
    // (e.g. both `#include <stdio.h>` → both have a `puts` ExternImport, with NO
    // cross-CU definition) collapse to ONE merged symbol so the merged module
    // carries exactly ONE import row (one IAT slot) and both CUs'
    // `GlobalAddr(externSym)` resolve to it. Two CUs importing the same NAME from
    // DIFFERENT libraries (or at different symbol versions) do NOT collapse —
    // they are two dynamic symbols and keep two merged ids, two import rows, and
    // two independently-bound call sites. Disjoint from `canonicalForName`: a
    // name that IS a defined winner never lands here (it rewires to the direct
    // def instead).
    std::unordered_map<std::string, SymbolId> ffiCanonicalForImport;
    // Real-FFI extern mangledName → the FIRST merged SymbolId minted for that
    // NAME. Serves ONE consumer: the step-3c shim arm below, whose subject is a
    // referenced-only SHIM symbol that carries NO ExternImport row and therefore
    // has no libraryPath / version to key on — a bare name is the only identity
    // it has. Deliberately NOT the extern-collapse index (that is
    // `ffiCanonicalForImport` above): a name imported from two libraries has two
    // merged ids and this map holds whichever was minted first, which is only
    // acceptable because that arm is documented-defensive (a shim name is not
    // FFI) and its pre-existing behaviour is name-first-wins.
    std::unordered_map<std::string, SymbolId> ffiCanonicalForName;
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): pe64 <threads.h> SHIM mangledName →
    // its single canonical merged SymbolId, shared across CUs. A shim symbol (mtx_lock
    // etc.) is REFERENCED-ONLY in each CU (CST→HIR skipped its import; the def is
    // synthesized POST-merge by `synthesizeThreadsShim` in program.cpp), so it is
    // neither a def nor an ExternImport — the def/extern planning below never sees it,
    // and a cloned caller's `GlobalAddr(shimSym)` would abort in `mergedSymbolOf`. This
    // pre-registers each shim symbol with a merged id (unified by name across CUs, like
    // `ffiCanonicalForName`) so the clone remaps cleanly; the merged id lands in
    // `symbolNames` so the post-merge reconstruction re-finds it. NEVER emitted as an
    // import (shims carry no ExternImport row → they cannot leak into survivingExterns).
    std::unordered_map<std::string, SymbolId> shimCanonicalForName;
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
        // TF-C78 (D-CSUBSET-NOINLINE): carried across the cross-CU merge — the
        // merged module is what the optimizer then runs on, so a flag dropped
        // here would let a `noinline` function from CU A be inlined after link.
        // TF-C81 (D-CSUBSET-ALWAYSINLINE): carried across the same boundary —
        // the merged module is what the optimizer runs on, so a flag dropped
        // here would silently restore the size threshold for an `always_inline`
        // function that came from CU A.
        // TF-C85: and the optimizer opt-out, across the same boundary and for
        // the same reason — the merged module is what the optimizer runs on, so
        // a flag dropped here would let a function the source put inside a
        // `#pragma optimize("", off)` region be optimized after link.
        // TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): and the thread-sanitizer
        // exclusion, across the same boundary. No pass reads it, so the argument
        // for carrying it is provenance rather than codegen: the merged module is
        // the artifact a `.dssir` dump describes, and a per-function fact that
        // silently disappears at the CU boundary is a fact the compiler cannot be
        // said to record at all.
        MirFuncId const newF = dst_.addFunction(
            sig, mergedSymbol, src_.funcBinding(f), src_.funcVisibility(f),
            src_.funcNoInline(f), src_.funcAlwaysInline(f),
            src_.funcNoOptimize(f), src_.funcNoSanitizeThread(f));
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
            // Thread the flat call-operand position (arg_payload.hpp) — the
            // 2-TU path optimizes POST-merge, so a position wipe here would
            // resurface the mixed-class inline miscompile on merged modules
            // (sqlite is 2-TU). D-OPT-RELEASE-SYSV-MIXED-CLASS-REG-ARG-DROP.
            local_.emplace(id.v,
                dst_.addArg(src_.argIndex(id), reType(src_.instType(id)),
                            src_.argPosition(id), src_.instFlags(id)));
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
        // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: carry the secondary payload (the
        // Alloca's effective alignment) across the cross-CU merge — a generic
        // copy that dropped it would silently under-align an over-aligned local
        // in a multi-CU build.
        local_.emplace(id.v, dst_.addInst(op, newOps, reType(src_.instType(id)),
                                          src_.instPayload(id), src_.instFlags(id),
                                          src_.instPayload2(id)));
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
            case MirOpcode::SehTryBegin:
                // c115 SEH (D-WIN64-SEH-FUNCLETS): succs [tryEntry, filterEntry];
                // the region-id payload clones verbatim (function-scoped ids —
                // merge clones whole functions, no renumbering).
                dst_.addSehTryBegin(mapBlock(succ[0]), mapBlock(succ[1]),
                                    src_.instPayload(id));
                return;
            case MirOpcode::SehFilterReturn:
                // operand [filterValue]; succ [handlerEntry]; payload verbatim.
                dst_.addSehFilterReturn(mapValue(ops[0], id), mapBlock(succ[0]),
                                        src_.instPayload(id));
                return;
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
    // extern import across all CUs. `ffiRow` is non-null only for an ExternImport
    // row, so a surviving real-FFI extern (its name has no cross-CU def)
    // collapses across CUs to one canonical merged id per IMPORT IDENTITY (the
    // `ffiImportKey` triple), not per mangledName.
    auto assignSymbol = [&](std::uint32_t ci, SymbolId oldSym,
                            std::string const& name, ExternImport const* ffiRow,
                            bool isLocalDef) {
        CuSymKey const key{ci, oldSym.v};
        if (plan.symMerged.count(key)) return;  // already assigned (idempotent)
        // A LOCAL (internal-linkage) DEFINITION is module-private (C 6.2.2p3): a
        // `static` object / function is a DISTINCT entity even when an UNRELATED CU
        // exports the SAME name, so it must NEVER fold onto a same-named externally-
        // visible winner. Only externally-visible defs (Global/Weak — collapsing
        // onto their own winner) and extern REFERENCES fold by name. Without this
        // guard a `static aSyscall[]` in one TU silently ALIASES a non-static
        // `aSyscall[]` in another (a function-pointer-table miscompile invisible
        // until BOTH TUs are linked — D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0).
        if (!name.empty() && !isLocalDef) {
            auto const it = plan.canonicalForName.find(name);
            if (it != plan.canonicalForName.end()) {
                // Externally-visible name (def winner / shadowed loser / an
                // extern reference to a cross-CU def) → collapse to the winner.
                plan.symMerged.emplace(key, it->second);
                return;
            }
            if (ffiRow != nullptr) {
                // A surviving real-FFI extern (no cross-CU def of this name).
                // Collapse externs that name the SAME DYNAMIC SYMBOL across CUs
                // to ONE merged symbol — the first occurrence mints it; later
                // CUs reuse it — so the merged module emits exactly one import
                // row per (mangledName, libraryPath, version). Keying on
                // mangledName ALONE (the pre-fix rule) folded `foo`@a.dll with
                // `foo`@b.dll and `puts@GLIBC_2.2.5` with `puts@GLIBC_2.17`,
                // silently binding one CU's call sites to the wrong library /
                // the wrong glibc compat form — see `ffiImportKey`.
                auto const fit = plan.ffiCanonicalForImport.find(ffiImportKey(*ffiRow));
                if (fit != plan.ffiCanonicalForImport.end()) {
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
            if (ffiRow != nullptr) {
                plan.ffiCanonicalForImport.emplace(ffiImportKey(*ffiRow), merged);
                // Name-first-wins, for the step-3c shim arm only (see the field
                // comment). Two libraries owning one name legitimately produce
                // two `symbolNames` entries carrying the SAME name under
                // DIFFERENT merged ids — every consumer of `symbolNames` is
                // id-keyed (compile_pipeline's `nameOf` lookup and program.cpp's
                // recipe reconstruction both key on the merged id), so a name is
                // never asked to resolve back to a single id.
                plan.ffiCanonicalForName.emplace(name, merged);
            }
        }
    };

    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            assignSymbol(ci, m.funcSymbol(f), cus[ci].nameOf(m.funcSymbol(f)),
                         /*ffiRow=*/nullptr,
                         /*isLocalDef=*/m.funcBinding(f) == SymbolBinding::Local);
        }
        std::size_t const ng = m.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            assignSymbol(ci, m.globalSymbol(g), cus[ci].nameOf(m.globalSymbol(g)),
                         /*ffiRow=*/nullptr,
                         /*isLocalDef=*/m.globalBinding(g) == SymbolBinding::Local);
        }
        for (ExternImport const& e : cus[ci].externImports) {
            // An extern's name is its mangledName (nameOf must agree, but the
            // import row is authoritative for the on-binary name). An extern is a
            // REFERENCE, never a Local definition — it folds onto a cross-CU def
            // winner or a shared FFI import (isLocalDef=false). The ROW itself is
            // passed, not just a flag: its (mangledName, libraryPath, version) is
            // the import identity the FFI collapse keys on.
            assignSymbol(ci, e.symbol, e.mangledName, /*ffiRow=*/&e,
                         /*isLocalDef=*/false);
        }
    }

    // ── (3c) FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): pe64 <threads.h> SHIM symbols.
    // A shim (mtx_lock etc.) is REFERENCED-ONLY in each CU (CST→HIR skipped its import;
    // the definition is synthesized POST-merge by `synthesizeThreadsShim`), so 3b never
    // assigned it a merged id and a cloned caller's `GlobalAddr(shimSym)` would abort in
    // `mergedSymbolOf`. Register each with a merged id, unified by NAME across CUs (two
    // CUs both `#include <threads.h>` share ONE merged mtx_lock, synthesized once). MUST
    // run AFTER 3a/3b so a name that ALSO has a genuine def winner (a user-defined
    // mtx_lock — goal-2 per CU) or FFI extern collapses to THAT (the user's def wins;
    // the post-merge reconstruction then sees a DEFINED symbol and synthesizes nothing).
    // The merged id lands in `symbolNames` so program.cpp's reconstruction re-finds it;
    // it is NEVER an ExternImport row, so it cannot leak into `survivingExterns`.
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        if (cus[ci].synthRecipes == nullptr) continue;
        for (auto const& [shimV, recipeId] : *cus[ci].synthRecipes) {
            (void)recipeId;
            CuSymKey const key{ci, shimV};
            if (plan.symMerged.count(key)) continue;   // also a real symbol — leave it
            std::string const name = cus[ci].nameOf(SymbolId{shimV});
            if (name.empty()) continue;                // no match key — cannot unify
            if (auto it = plan.canonicalForName.find(name);
                it != plan.canonicalForName.end()) {   // a genuine def winner wins
                plan.symMerged.emplace(key, it->second);
                continue;
            }
            // Defensive (a shim name is not FFI). Keyed by NAME because a shim
            // symbol carries NO ExternImport row — it has no libraryPath /
            // version to form an `ffiImportKey` from — which is exactly why
            // `ffiCanonicalForName` is kept alongside `ffiCanonicalForImport`.
            if (auto it = plan.ffiCanonicalForName.find(name);
                it != plan.ffiCanonicalForName.end()) {
                plan.symMerged.emplace(key, it->second);
                continue;
            }
            if (auto it = plan.shimCanonicalForName.find(name);
                it != plan.shimCanonicalForName.end()) { // another CU's same-named shim
                plan.symMerged.emplace(key, it->second);
                continue;
            }
            SymbolId const merged =
                (ci == 0 && alloc.isFree(shimV)) ? alloc.claim(shimV) : alloc.mint();
            plan.symMerged.emplace(key, merged);
            plan.symbolNames.emplace(merged.v, name);
            plan.shimCanonicalForName.emplace(name, merged);
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
                               SymbolId sym, bool isLocal) -> bool {
        if (name.empty()) return false;
        // A LOCAL (internal-linkage) definition is module-private (C 6.2.2p3): it
        // is neither a winner NOR a loser of cross-CU name resolution, even when it
        // shares a name with an externally-visible symbol in another CU. It is
        // ALWAYS kept (cloned with its own fresh merged id). Without this, a
        // `static aSyscall[]` sharing a name with another TU's extern `aSyscall[]`
        // would be dropped as a "loser" and its own references would dangle
        // (D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0).
        if (isLocal) return false;
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
            if (isShadowedLoser(ci, name, m.funcSymbol(f),
                                m.funcBinding(f) == SymbolBinding::Local))
                continue;  // weak loser (never a Local — module-private, kept)
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
            if (isShadowedLoser(ci, name, m.globalSymbol(g),
                                m.globalBinding(g) == SymbolBinding::Local))
                continue;

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
                                    m.globalIsConst(g),
                                    // TLS C1 (D-CSUBSET-THREAD-LOCAL, CRIT-3):
                                    // carry thread storage duration across the
                                    // cross-CU merge — dropping it here would
                                    // silently demote a per-thread object to
                                    // process-shared in every N>1 build.
                                    mirThreadStorageOf(m.globalIsThreadLocal(g)),
                                    // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: carry
                                    // the global's explicit alignment across the
                                    // cross-CU merge.
                                    m.globalAlignmentBytes(g));
        }
    }

    // ── (6) surviving externImports: an extern whose mangledName has NO cross-CU
    // winner DEFINITION stays a real FFI import (carried, symbol unified-
    // remapped); a cross-CU-resolved extern is STRIPPED (its calls were rewired
    // to direct in step 4). Dedup by IMPORT IDENTITY (`ffiImportKey`) — the SAME
    // key step 3b's `ffiCanonicalForImport` collapse used, so the two passes
    // cannot disagree about what "one import" is: two CUs naming the same dynamic
    // symbol share one merged id AND emit one row (one IAT slot), while `foo`
    // from two DIFFERENT libraries keeps two ids and emits two rows.
    //
    // ★ THE PAYLOAD IS FOLDED, NEVER DROPPED (D-LK11-EXTERN-IMPORT-DEDUP).
    // Keeping the first row and discarding the rest is a SILENT MISCOMPILE, not a
    // size win — see the per-field rules at each site below. This tier is the
    // LIVE route (`--compile a.c b.c` and every `--project` build, both sqlite
    // legs); the assembled-tier `mergeModules` fold in link/linker.cpp is the
    // matching implementation one tier down, reached only via
    // `--resolve-library`. The two MUST stay in lockstep: a rule enforced on one
    // tier only means the build that actually runs is the unguarded one.
    std::vector<ExternImport> survivingExterns;
    // A disagreement between two CUs about ONE dynamic symbol is a REAL conflict,
    // never a pick-one. `K_ExternImportAttributeConflict` is the DECLARATION-tier
    // code (distinct from the definition-tier `K_SymbolRedefinedAcrossUnits`
    // reported in step 2 — nobody DEFINES this symbol; the two `extern`
    // declarations of it simply contradict each other).
    auto const externAttrConflict = [&](ExternImport const& e, char const* field,
                                        std::string const& kept,
                                        std::string const& incoming) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::K_ExternImportAttributeConflict;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "mergeCuMirs: extern import \"" + e.mangledName + "\"" +
                     (e.libraryPath.empty() ? std::string{}
                                            : " (library \"" + e.libraryPath + "\")") +
                     (e.version.empty() ? std::string{}
                                        : " (version \"" + e.version + "\")") +
                     " is declared with conflicting " + field +
                     " across compilation units (" + kept + " vs " + incoming +
                     ") — one dynamic symbol cannot be imported two ways "
                     "(D-LK11-EXTERN-IMPORT-DEDUP).";
        reporter.report(std::move(d));
    };
    // `dataSizeBytes` / `dataAlignBytes` SIZE the ELF copy-relocation `.bss` slot
    // (c84 D-LK-EXTERN-DATA-IMPORT). One CU legitimately holds an INCOMPLETE type
    // (`extern const char v[];` ⇒ 0/0 — extern_import.hpp:76-81), so a zero is
    // "unknown here", not a disagreement: take the non-zero. Two DIFFERING
    // non-zero values would reserve the SAME slot two ways — the loader memcpy's
    // `st_size` bytes, so picking either silently truncates or over-copies.
    auto const foldNonZero = [&](std::uint64_t& kept, std::uint64_t incoming,
                                 char const* field, ExternImport const& e) {
        if (incoming == 0 || kept == incoming) return;  // incomplete / agrees
        if (kept == 0) { kept = incoming; return; }     // the complete type wins
        externAttrConflict(e, field, std::to_string(kept), std::to_string(incoming));
    };
    auto const boolStr = [](bool b) { return std::string{b ? "true" : "false"}; };

    // import identity → survivingExterns INDEX (not a bare seen-set) so a
    // collapsed duplicate can fold its payload into the already-emitted row.
    std::unordered_map<std::string, std::size_t> emittedExternIdx;
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        for (ExternImport const& e : cus[ci].externImports) {
            if (plan.definedNames.count(e.mangledName)) continue;  // → direct, strip
            SymbolId const mergedSym = mergedSymbolOf(plan, ci, e.symbol);
            auto const [it, inserted] =
                emittedExternIdx.try_emplace(ffiImportKey(e), survivingExterns.size());
            if (inserted) {
                ExternImport carried = e;
                carried.symbol = mergedSym;
                survivingExterns.push_back(std::move(carried));
                continue;
            }
            ExternImport& kept = survivingExterns[it->second];
            // Steps 3b and 6 key on the SAME `ffiImportKey`, so a row that folds
            // here must already share the group's merged id. A mismatch means the
            // two passes disagree about import identity — and folding two rows
            // whose references bind to DIFFERENT merged symbols would silently
            // drop one row's payload while its call sites still point elsewhere.
            // An internal invariant breach, not user error: abort, never report.
            if (kept.symbol.v != mergedSym.v) {
                std::fprintf(stderr,
                    "dss::mergeCuMirs fatal: CU %u extern import \"%s\" folds into "
                    "the dedup group canonicalized to merged symbol v=%u, but its "
                    "planned merged symbol is v=%u — step 3b's import-identity key "
                    "and step 6's must be THE SAME key "
                    "(D-LK11-EXTERN-IMPORT-DEDUP).\n",
                    ci, e.mangledName.c_str(), kept.symbol.v, mergedSym.v);
                std::abort();
            }
            // `isEagerImport` — OR-COMBINE, as extern_import.hpp:112-114 mandates
            // (D-LINK-EXTERN-IMPORT-REFERENCE-GATE (e)). Keeping a NON-eager row
            // when a sibling CU declared the same import EAGER lets the linker's
            // `rejectOrDropUnreferencedExterns` DROP a
            // shipped-descriptor symbol the loader must bind
            // (D-FFI-DESCRIPTOR-EAGER-IMPORT) — a LOAD failure (pe 0xC0000139 /
            // elf exit 127), not a size regression. Order-INDEPENDENT: whichever
            // CU's row lands first, the bit is ORed in.
            kept.isEagerImport = kept.isEagerImport || e.isEagerImport;
            // `isData` / `isThreadLocal` — silently picking either row is the
            // D-LK-EXTERN-DATA-IMPORT silent-miscompile shape: `isData` decides
            // whether the walker binds the name through the DATA-slot model (the
            // ELF copy-relocation) or the function-import path, so the loser's CU
            // would have every reference bound through the WRONG model — a PLT
            // stub standing in for a data object, or a copy-reloc `.bss` slot
            // standing in for a function. `isThreadLocal` likewise selects the
            // (unimplemented, walker-rejected) initial-exec TLS model —
            // D-CSUBSET-THREAD-LOCAL.
            if (kept.isData != e.isData) {
                externAttrConflict(e, "`isData` (data object vs function import)",
                                   boolStr(kept.isData), boolStr(e.isData));
            }
            if (kept.isThreadLocal != e.isThreadLocal) {
                externAttrConflict(e, "`isThreadLocal` (thread storage duration)",
                                   boolStr(kept.isThreadLocal), boolStr(e.isThreadLocal));
            }
            foldNonZero(kept.dataSizeBytes,  e.dataSizeBytes,
                        "`dataSizeBytes` (copy-relocation slot size)", e);
            foldNonZero(kept.dataAlignBytes, e.dataAlignBytes,
                        "`dataAlignBytes` (copy-relocation slot alignment)", e);
            // Every remaining ExternImport field is accounted for: `symbol` IS the
            // dedup output (the canonical merged id, checked equal above), and
            // `mangledName` / `libraryPath` / `version` are the KEY, hence equal
            // by construction. No field is carried over silently.
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
                if (isShadowedLoser(ci, name, m.funcSymbol(f),
                                    m.funcBinding(f) == SymbolBinding::Local))
                    continue;
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
