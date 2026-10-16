#include "mir/merge/mir_merge.hpp"

#include "core/substrate/phase_timers.hpp"  // the merge-side whole-program verify's `--time` row
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout — the extent of a tentative definition
#include "core/types/type_lattice/type_reintern.hpp"
#include "link/common_yield.hpp"       // the ONE reader of the link document's weak / common statements
#include "link/cross_cu_resolve.hpp"   // resolveCrossCuDefs, CrossCuDef, LinkedSymbolKey
#include "link/object_format_schema.hpp"   // ObjectFormatSchema — the link's document
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
#include <limits>       // SymbolAllocator::end — saturate at the last id
#include <optional>     // the per-CU subset-import filter
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
        handedOut_(v);
        return SymbolId{v};
    }
    // Mint the next free id.
    [[nodiscard]] SymbolId mint() {
        while (used_.count(next_)) ++next_;
        std::uint32_t const v = next_++;
        used_.insert(v);
        handedOut_(v);
        return SymbolId{v};
    }
    [[nodiscard]] bool isFree(std::uint32_t v) const { return used_.count(v) == 0; }

    // One past the highest id this allocator has handed out: the END of the
    // merged module's id space, which is a NEW space (no unit's name table
    // describes it — the merged names are the plan's own `symbolNames`, every key
    // of which was handed out here). `mergeCuMirs` states it to the merged
    // module's builder, so whatever mints into the merged module afterwards asks
    // the module and clears every merged id — the ones the module holds, the
    // imports' (their rows live beside it) and the ones a name alone holds.
    [[nodiscard]] std::uint32_t end() const noexcept { return end_; }

private:
    void handedOut_(std::uint32_t v) noexcept {
        constexpr std::uint32_t kLast = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t const past = v == kLast ? kLast : v + 1u;
        if (past > end_) end_ = past;
    }

    std::uint32_t                     next_;
    std::uint32_t                     end_ = 1;
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
    // Cross-CU composite identity, SHARED by every CU and populated from EVERY
    // source interner before the first reintern -- so a `typedef struct Bitvec
    // Bitvec;` in one CU and the `struct Bitvec { ... }` in another land on ONE
    // host TypeId whichever CU the merge walks first
    // (D-MIR-MERGE-COMPOSITE-HOST-IDENTITY-IS-THE-DECLARATION-SITE).
    CompositeIdentityIndex compositeIdentity;
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
// D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL: the field
// descent was host recursion with no cap; it is now the shared heap walker in
// `mir/mir_literal_pool.hpp`.
void remapLiteralSymbols(MirLiteralValue& lit, MergePlan const& plan,
                         std::uint32_t cuIdx) {
    forEachLiteralNode(lit, [&](MirLiteralValue& n) {
        if (auto* sa = std::get_if<MirSymbolAddrValue>(&n.value)) {
            sa->symbol = mergedSymbolOf(plan, cuIdx, SymbolId{sa->symbol}).v;
        }
    });
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
    // `keptAsModulePrivate`: the body is a LOSING definition the merge keeps for an
    // alias of its unit (step 4) — module-private under its own fresh id, with no
    // static-initialization schedule of its own (a loser's is dropped with it, as
    // every loser's is).
    [[nodiscard]] MirFuncId clone(MirFuncId f, SymbolId mergedSymbol,
                                  bool keptAsModulePrivate = false) {
        TypeId const sig = reinternType(srcInterner_, src_.funcSignature(f),
                                        host_, typeRemap(),
                                        plan_.compositeIdentity);
        // TF-C78 (D-CSUBSET-NOINLINE-PER-FUNCTION-SINK): carried across the cross-CU merge — the
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
        // P69: the LINKAGE crosses as its one record (`Mir::funcLinkage`) — the
        // binding, the visibility and the weak kind together, so a weak
        // definition that survives the merge still says which kind it is to the
        // object writer.
        MirFuncId const newF = dst_.addFunction(
            sig, mergedSymbol,
            keptAsModulePrivate ? MirLinkage{SymbolBinding::Local, SymbolVisibility::Default}
                                : src_.funcLinkage(f),
            src_.funcNoInline(f), src_.funcAlwaysInline(f),
            src_.funcNoOptimize(f), src_.funcNoSanitizeThread(f),
            // D-C-GNU-CONSTRUCTOR-ATTRIBUTE-IS-WARNED-AND-IGNORED-NOT-RUN:
            // the schedule must cross the CU boundary or a constructor defined
            // in one translation unit stops running the moment a second unit
            // joins the link — the sharpest form of the drop this axis fears.
            keptAsModulePrivate ? StaticInitSchedule{} : src_.funcStaticInit(f));
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
                // Anchored: D-MERGE-UNREACHABLE-BLOCK.
                std::fprintf(stderr,
                    "dss::mergeCuMirs fatal: CU %u function symbol v=%u block "
                    "v=%u is unreachable from the entry — the merge clones the "
                    "reachable CFG only (run DCE before merge to drop dead "
                    "blocks).\n",
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
        return reinternType(srcInterner_, t, host_, typeRemap(),
                            plan_.compositeIdentity);
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
    // Const / GlobalAddr / BlockAddress / InlineAsm use their dedicated builders
    // (each owns a distinct payload encoding — argIndex / literalIndex / symbol /
    // block id / asm-descriptor index — that a raw addInst would mis-stamp); every
    // other opcode (Call, Load, Store, Add, Alloca, IntrinsicCall, ExtractValue,
    // ...) re-emits generically with operands mapped through `local_` and
    // `payload`/`flags` copied verbatim.
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
        if (op == MirOpcode::BlockAddressExport) {
            // D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: the payload is a
            // per-CU SymbolId, which the merge RENUMBERS — a generic `addInst` copy
            // would carry the stale id, and the initializer literal's own
            // `MirSymbolAddrValue` (remapped by `remapLiteralSymbols`) would then
            // relocate against a DIFFERENT symbol than the one the block is bound
            // to. Both sides must go through the SAME `symMerged` map, which is
            // exactly why step 3d assigns these symbols a merged id.
            local_.emplace(id.v, dst_.addBlockAddressExport(
                mapValue(src_.instOperands(id)[0], id),
                mergedSymbolOf(plan_, cuIdx_, src_.blockAddressExportSymbol(id)),
                src_.instFlags(id)));
            return;
        }
        if (op == MirOpcode::InlineAsm) {
            // Inline-asm P5. The payload indexes the SOURCE CU's asm-descriptor
            // pool; the merged module is a DIFFERENT `Mir` whose pool starts
            // empty, so forwarding the raw index below would name a descriptor
            // that does not exist there — i.e. silently drop the template and the
            // clobber list. Re-add the descriptor to the destination instead.
            //
            // ★ WHOLE, BY VALUE — never field-by-field, the same rule the rebuild
            // substrate states at `opt/passes/mir_rebuild_helper.cpp`: a
            // field-by-field copy that omits `isExtended` or an input's
            // `tiedOutput` is exactly the silent-drop class `mir_asm_descriptor.hpp`
            // guards.
            //
            // NOTE the one asymmetry with every other arm here: the descriptor
            // needs NO `reType`. It carries template text, constraint strings,
            // register names, clobbers and bools — no `TypeId` — so nothing in it
            // is CU-scoped. Only the INSTRUCTION's result type crosses the lattice
            // boundary, and that goes through `reType` below like all the others.
            auto const asmOps = src_.instOperands(id);
            std::vector<MirInstId> newAsmOps;
            newAsmOps.reserve(asmOps.size());
            for (MirInstId const o : asmOps) newAsmOps.push_back(mapValue(o, id));
            local_.emplace(id.v, dst_.addInlineAsm(src_.asmDescriptor(id), newAsmOps,
                                                   reType(src_.instType(id)),
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
                // P69: what the terminator asserts survives the merge.
                dst_.addUnreachable(static_cast<MirUnreachableKind>(src_.instPayload(id)));
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
            case MirOpcode::InlineAsmGoto: {
                // Inline-asm P5, the TERMINATOR form — the sibling of `emitValue`'s
                // `InlineAsm` arm and the same descriptor rule: the payload indexes
                // the SOURCE CU's pool, so the descriptor is re-added to the merged
                // module rather than forwarded as an index.
                //
                // `cloneInlineAsmGoto`, NEVER `addInlineAsmGoto`. The result pieces
                // live at the heads of the successor blocks, which this merge already
                // clones as ORDINARY blocks — the landing blocks came across through
                // `mapBlock` with their pieces intact. `addInlineAsmGoto` owns the
                // edge-PLACEMENT rule and re-running it here would interpose a SECOND
                // landing block on every edge, on top of the one already cloned.
                //
                // ⚠ EVERY successor crosses, the trailing FALL-THROUGH edge
                // included — it is what keeps the code after the asm statement
                // reachable, so a filtered walk here would delete live code in the
                // merged module. `cloneInlineAsmGoto` re-checks the count against
                // the descriptor's own label list rather than trusting the opcode
                // row's range, which a truncated set can still satisfy.
                std::vector<MirInstId> newOps;
                newOps.reserve(ops.size());
                for (MirInstId const o : ops) newOps.push_back(mapValue(o, id));
                std::vector<MirBlockId> targets;
                targets.reserve(succ.size());
                for (MirBlockId const b : succ) targets.push_back(mapBlock(b));
                MirInstId const newId = dst_.cloneInlineAsmGoto(
                    src_.asmDescriptor(id), newOps, targets, src_.instFlags(id));
                // ★ THE ONE TERMINATOR ARM THAT MUST RECORD ITS CLONE. Every other
                // terminator here produces nothing, so none of them touch `local_`.
                // An `asm goto` WITH OUTPUTS is different: each result piece is a
                // `ReturnPiece` at the head of a successor block that anchors to
                // this terminator AS ITS OPERAND (`addReturnPiece(producer, ...)`,
                // mir.cpp). Those successors are filled later in the same RPO walk,
                // and without this mapping their `mapValue` would abort with the
                // "no clone mapping" fatal instead of finding the terminator.
                local_.emplace(id.v, newId);
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
            std::span<std::string const> entryNames,
            ObjectFormatSchema const& linkFormat, DiagnosticReporter& reporter) {
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

    // ── THE SUBSET-IMPORT FILTER (OPT11, D-OPT11-LAZY-IMPORT-EDGE) ───────────
    // `MergeCuInput::importOnly` turns a CU from a merge PARTICIPANT into an
    // IMPORT SOURCE. Materialized ONCE, as a set per CU, so the six sites that
    // consult it below ask the same question the same way — a per-site
    // re-derivation is how two of them would eventually disagree about which
    // functions a CU contributes, and a disagreement here is a symbol the clone
    // emits with no definition behind it.
    std::vector<std::optional<std::unordered_set<std::string>>>
        importFilter(cus.size());
    for (std::size_t i = 0; i < cus.size(); ++i) {
        if (cus[i].importOnly == nullptr) continue;
        importFilter[i].emplace(cus[i].importOnly->begin(),
                                cus[i].importOnly->end());
    }
    // ⚠ CU0 IS THE DESTINATION, ALWAYS. Every id-retention rule below
    // (`maxCu0`, `alloc.claim`) is written for "CU0 is the module we are
    // importing INTO"; an import source in slot 0 would keep its own symbol
    // values and mint fresh ones for the destination's, which is a silently
    // renumbered module rather than an error.
    if (importFilter[0].has_value()) {
        // Anchored: D-OPT11-LAZY-IMPORT-EDGE.
        mergeFatal("cus[0] carries an importOnly filter — slot 0 is the merge "
                   "DESTINATION and must be a full participant.");
    }
    auto const isImportSource = [&](std::uint32_t ci) {
        return importFilter[ci].has_value();
    };
    // Does CU `ci` contribute `name` as a DEFINITION?
    auto const contributesFunction = [&](std::uint32_t ci,
                                         std::string const& name) {
        if (!importFilter[ci].has_value()) return true;
        return !name.empty() && importFilter[ci]->count(name) != 0;
    };

    MergePlan plan;
    plan.typeRemap.resize(cus.size());

    // ── (0) COMPOSITE IDENTITY PRE-PASS — before ANY reintern ────────────────
    // Whether a forward-declared `struct T` may unify with a definition depends
    // on CUs it does not itself contain, so the question cannot be answered
    // while walking. Observing EVERY source interner first is what makes the
    // merged type graph independent of the order the CUs are walked -- and
    // order-independence is not a nicety here: the same operator ruling that
    // requires byte-identical optimized output for any prefetch depth applies to
    // a type graph that decides what the optimizer may inline.
    for (auto const& cu : cus) plan.compositeIdentity.observe(*cu.interner);

    // ── (1)+(2) name → defining (cuIdx, MirFuncId, binding) + resolveCrossCuDefs.
    // A LinkedSymbolKey's cuId is the synthetic `cuIdx+1` (unique per CU, order-
    // stable); `cuIdxOf(key) == key.cuId.v - 1`. Only externally-visible
    // (Global/Weak) function definitions feed the resolver — Local stays module-
    // private (resolveCrossCuDefs filters Local, but pre-filtering keeps the def
    // list honest about what crosses CU boundaries).
    std::vector<linker::CrossCuDef> defs;
    // ── WHAT THE LINK'S DOCUMENT SAYS OF A WEAK DEFINITION (P69 fold 3) ───────
    // The kernel compares what it is handed, and until now this tier handed it
    // a binding and nothing else — so every weak definition folded at one rank
    // (✔MEASURED 2026-10-12 on DSS-compiled units, pe64: a select-any
    // definition beside an overridable one ran the select-any body with its
    // unit named first and the OVERRIDABLE one, exit 44, with it second), and a
    // strong definition replaced a select-any one in silence where every PE
    // reference refuses the pair. The three statements below are the ones the
    // unit linker makes of a reader's row, read from the same document by the
    // same functions (`link/common_yield.hpp`) off the definition's own kind
    // (`MirLinkage::statedWeakKind`): one rule, two tiers.
    auto const stateTheLinksAnswers = [&](linker::CrossCuDef& def,
                                          MirLinkage const&   linkage) {
        linker::stateTheLinksAnswers(def, linkFormat, linkage.statedWeakKind());
    };
    // ── THE TENTATIVE DEFINITIONS, BY NAME ───────────────────────────────────
    //    D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED
    // A global that is a COMMON (`Mir::globalIsCommon`) enters the election as
    // one — `CrossCuDef::tentative` — and is remembered here with the index of
    // its `defs` entry, so that a name two units define this way can be given
    // the extents the fold compares (below, after the walk).
    struct TentativeCarrier {
        std::size_t   def;      // index into `defs`
        std::uint32_t cu;
        MirGlobalId   global;
        // The alignment this carrier's own definition asks for — its type's,
        // or the one its unit stated if that is wider. Measured only for a name
        // with two or more carriers; 0 until then.
        std::uint32_t alignment = 0;
    };
    std::unordered_map<std::string, std::vector<TentativeCarrier>> tentativeCarriers;
    // P69 (lane `cs`): the (cu, symbol) of every alias that entered the election —
    // so a name's winner can be told to be an alias (below, `aliasWonNames`).
    std::unordered_set<CuSymKey, CuSymKeyHash> aliasKeys;
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
            // An import source contributes only its SELECTED functions — the
            // rest are not definitions of this merge and must not enter the
            // cross-CU election, or a name it merely happens to define would
            // shadow the importer's own reference to the real one.
            if (!contributesFunction(ci, name)) continue;
            MirLinkage const linkage = m.funcLinkage(f);
            if (linkage.binding == SymbolBinding::Local) continue;
            plan.definedNames.insert(name);
            defs.push_back(linker::CrossCuDef{
                name, linkage.binding,
                LinkedSymbolKey{CompilationUnitId{ci + 1}, m.funcSymbol(f)}});
            stateTheLinksAnswers(defs.back(), linkage);
        }
        // Externally-visible globals participate in cross-CU resolution too (a
        // strong global shadows a weak one of the same name), exactly like
        // functions.
        //
        // ⚠ AN IMPORT SOURCE CONTRIBUTES NO GLOBAL AT ALL, and this is the
        // asymmetry that makes a subset import safe. A global has IDENTITY and
        // STATE; two copies of one are a miscompile, whereas a function body is
        // pure code the inliner may duplicate. So the importer never receives a
        // definition — only a reference the final whole-program merge resolves.
        if (isImportSource(ci)) continue;
        std::size_t const ng = m.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            std::string const name = cus[ci].nameOf(m.globalSymbol(g));
            if (name.empty()) continue;
            MirLinkage const linkage = m.globalLinkage(g);
            if (linkage.binding == SymbolBinding::Local) continue;
            plan.definedNames.insert(name);
            defs.push_back(linker::CrossCuDef{
                name, linkage.binding,
                LinkedSymbolKey{CompilationUnitId{ci + 1}, m.globalSymbol(g)}});
            stateTheLinksAnswers(defs.back(), linkage);
            if (m.globalIsCommon(g)) {
                defs.back().tentative = true;
                tentativeCarriers[name].push_back(
                    TentativeCarrier{defs.size() - 1, ci, g});
            }
        }
        // P69 (lane `cs`): AN ALIAS IS A DEFINITION OF ITS NAME (`Mir::aliases`),
        // and it enters the election as one, with its OWN binding and kind — a
        // weak alias yields to another unit's strong definition of the name, and
        // a strong one beside it is two definitions, exactly as the unit linker
        // ranks the alias row of a compiled object. Its name is NOT a
        // `definedNames` entry: every reference to an alias is by NAME (the
        // unit's own reference row, `ExternImport`), and while an alias wins the
        // name that row must survive, so the link resolves it to the alias row.
        for (MirAlias const& a : m.aliases()) {
            std::string const name = cus[ci].nameOf(a.alias);
            if (name.empty() || a.linkage.binding == SymbolBinding::Local) continue;
            aliasKeys.insert(CuSymKey{ci, a.alias.v});
            defs.push_back(linker::CrossCuDef{
                name, a.linkage.binding,
                LinkedSymbolKey{CompilationUnitId{ci + 1}, a.alias}});
            stateTheLinksAnswers(defs.back(), a.linkage);
        }
    }

    // ── THE EXTENT OF EACH COMMON THAT HAS A SIBLING ─────────────────────────
    // Two units' tentative definitions of one name are ONE object, the larger,
    // at the wider alignment — what every reference linker makes of two commons
    // (✔MEASURED 2026-10-12, PE and ELF; C leaves two declarations of one
    // object with types that are not compatible undefined, and `int a[];`
    // beside `int a[10];` is the shape that reaches a linker anyway). So the
    // fold needs each carrier's SIZE, and the size of a type is the layout
    // engine's to say — `computeLayout`, the rule `sizeof` asks — under the
    // layout rule ITS OWN UNIT was analyzed under
    // (`MergeCuInput::aggregateLayout`) and the link's data model.
    //   ⓘ Asked ONLY of a name two or more units define tentatively. A lone
    //     tentative definition is sized by nobody here: it wins or loses by its
    //     class, and the assembler sizes what survives.
    //   ⓘ Each carrier is measured in ITS OWN unit's lattice and under its own
    //     unit's rule — the question is the size its unit declared, no type is
    //     re-interned to ask it, and no unit answers for another.
    //   ⓘ Two carriers whose units were analyzed under DIFFERENT rules have
    //     sizes that are not comparable, and the name is refused: the driver's
    //     units agree by construction, and this is where that is checked.
    // `commonAlignment[name]` is the widest alignment among the name's
    // carriers; step (5) raises the surviving global to it.
    std::unordered_map<std::string, std::uint32_t> commonAlignment;
    {
        auto const sameRule = [](AggregateLayoutParams const& a,
                                 AggregateLayoutParams const& b) {
            return a.scalarAlignment == b.scalarAlignment
                   && a.maxAlignment == b.maxAlignment
                   && a.bitFieldStrategy == b.bitFieldStrategy
                   && a.unnamedBitFieldAlignment == b.unnamedBitFieldAlignment
                   && a.maxRequestedAlignment == b.maxRequestedAlignment;
        };
        auto const refuse = [&](std::string const& name, std::string why) {
            ParseDiagnostic d;
            d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
            d.severity = DiagnosticSeverity::Error;
            d.actual   = std::format(
                "mergeCuMirs: symbol '{}' is defined tentatively by {} compilation "
                "units, which makes it ONE object as large as the largest of them "
                "— and {}.",
                name, tentativeCarriers.at(name).size(), why);
            reporter.report(std::move(d));
        };
        std::vector<std::string> names;
        names.reserve(tentativeCarriers.size());
        for (auto const& [name, carriers] : tentativeCarriers) {
            if (carriers.size() >= 2) names.push_back(name);
        }
        std::sort(names.begin(), names.end());   // refusals in a stated order
        for (std::string const& name : names) {
            std::uint32_t widest = 0;
            std::vector<TentativeCarrier>& carriers = tentativeCarriers.at(name);
            auto const& firstRule = cus[carriers.front().cu].aggregateLayout;
            for (TentativeCarrier& c : carriers) {
                Mir const& m    = *cus[c.cu].mir;
                auto const& rule = cus[c.cu].aggregateLayout;
                if (!rule.has_value()) {
                    refuse(name, std::format(
                        "the definition in CU #{} cannot be sized: the merge was "
                        "handed no layout rule for that unit "
                        "(MergeCuInput::aggregateLayout)", c.cu + 1));
                    continue;
                }
                if (firstRule.has_value() && !sameRule(*firstRule, *rule)) {
                    refuse(name, std::format(
                        "CU #{} and CU #{} were analyzed under different layout "
                        "rules, so the sizes of their definitions are not "
                        "comparable", carriers.front().cu + 1, c.cu + 1));
                    continue;
                }
                auto const lay = computeLayout(m.globalType(c.global),
                                               *cus[c.cu].interner, *rule,
                                               linkFormat.dataModel());
                if (!lay.has_value()) {
                    refuse(name, std::format(
                        "the definition in CU #{} cannot be sized: the layout "
                        "engine has no size for its type", c.cu + 1));
                    continue;
                }
                defs[c.def].bodySize = static_cast<std::size_t>(lay->size);
                c.alignment = std::max(static_cast<std::uint32_t>(lay->align.bytes()),
                                       m.globalAlignmentBytes(c.global));
                widest = std::max(widest, c.alignment);
            }
            commonAlignment.emplace(name, widest);
        }
    }

    linker::CrossCuResolution const resolution =
        linker::resolveCrossCuDefs(defs);

    // ── A TENTATIVE DEFINITION LARGER, OR MORE ALIGNED, THAN THE DEFINITION IT
    //    YIELDS TO IS SAID (P69 fold 3b, the review of fold 3a, F4) ───────────
    // The unit linker's sentence (`allocateCommonDefinitions`), for the units
    // this merge folds: the tentative definition's unit reads and writes the
    // object as the one it declared, past the definition's end or off its
    // alignment. Every reference linker links the pair, so this merge does, and
    // says so (`K_CommonSymbolLargerThanItsDefinition`, a warning). Each side is
    // sized as step (1b) sizes a carrier: its own unit's layout rule; a side that
    // cannot be sized is not judged here (that refusal is step (1b)'s).
    {
        auto const extentOf = [&](std::uint32_t ci, MirGlobalId g)
            -> std::optional<std::pair<std::uint64_t, std::uint32_t>> {
            Mir const& m    = *cus[ci].mir;
            auto const& rule = cus[ci].aggregateLayout;
            if (!rule.has_value()) return std::nullopt;
            auto const lay = computeLayout(m.globalType(g), *cus[ci].interner, *rule,
                                           linkFormat.dataModel());
            if (!lay.has_value()) return std::nullopt;
            return std::pair{static_cast<std::uint64_t>(lay->size),
                             std::max(static_cast<std::uint32_t>(lay->align.bytes()),
                                      m.globalAlignmentBytes(g))};
        };
        std::vector<std::string> names;
        for (auto const& [name, carriers] : tentativeCarriers) names.push_back(name);
        std::sort(names.begin(), names.end());   // warnings in a stated order
        for (std::string const& name : names) {
            auto const won = resolution.winners.find(name);
            if (won == resolution.winners.end()) continue;
            auto const& carriers = tentativeCarriers.at(name);
            bool const aCarrierWon = std::any_of(carriers.begin(), carriers.end(),
                [&](TentativeCarrier const& c) { return defs[c.def].key == won->second; });
            if (aCarrierWon) continue;
            std::uint32_t const wcu = won->second.cuId.v - 1;
            if (wcu >= cus.size()) continue;
            Mir const& wm = *cus[wcu].mir;
            std::optional<MirGlobalId> winner;
            for (std::uint32_t gi = 0; gi < wm.moduleGlobalCount(); ++gi) {
                if (wm.globalSymbol(wm.globalAt(gi)) == won->second.symbol) winner = wm.globalAt(gi);
            }
            if (!winner.has_value()) continue;   // a function: no storage to compare
            auto const definition = extentOf(wcu, *winner);
            if (!definition.has_value()) continue;
            for (TentativeCarrier const& c : carriers) {
                auto const tentative = extentOf(c.cu, c.global);
                if (!tentative.has_value()) continue;
                if (tentative->first <= definition->first && tentative->second <= definition->second) continue;
                ParseDiagnostic d;
                d.code     = DiagnosticCode::K_CommonSymbolLargerThanItsDefinition;
                d.severity = DiagnosticSeverity::Warning;
                d.actual   = std::format(
                    "mergeCuMirs: the tentative definition of '{}' in CU #{} is {} bytes "
                    "aligned to {}, and the definition of the name it yields to (CU #{}) "
                    "is {} bytes aligned to {}: the tentative definition's unit reads and "
                    "writes the object as the larger or more aligned one it declared. "
                    "Linked as every reference linker links it.",
                    name, c.cu + 1, tentative->first, tentative->second, wcu + 1,
                    definition->first, definition->second);
                reporter.report(std::move(d));
                break;   // once per name
            }
        }
    }

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
    // A weak definition the link's format does not let a strong one replace, and
    // a pair the link's document was asked about and does not decide — the SAME
    // conditions, codes and sentences as the unit linker's and the summary
    // index's: the kernel recorded them, and the ONE wording all three doors
    // share makes each (`linker::refusalOf`, link/common_yield.hpp).
    auto const reportRefusal = [&](linker::CrossCuRefusal refusal) {
        ParseDiagnostic d;
        d.code     = refusal.code;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "mergeCuMirs: " + std::move(refusal.text);
        reporter.report(std::move(d));
    };
    for (linker::CrossCuCollision const& c : resolution.collisions) {
        reportRefusal(linker::refusalOf(c, linkFormat));
    }
    for (linker::CrossCuUndecided const& u : resolution.undecided) {
        reportRefusal(linker::refusalOf(u, linkFormat));
    }
    // P69 (lane `cs`): the names an ALIAS won. A reference to one stays a
    // reference to the name (its row is carried, step 6), never a direct call to
    // a definition: the alias has none of its own to call.
    std::unordered_set<std::string> aliasWonNames;
    for (auto const& [name, winKey] : resolution.winners) {
        if (aliasKeys.contains(CuSymKey{winKey.cuId.v - 1, winKey.symbol.v}))
            aliasWonNames.insert(name);
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
        for (MirAlias const& a : m0.aliases()) {   // P69 (lane `cs`)
            maxCu0 = std::max(maxCu0, a.alias.v);
        }
    }
    SymbolAllocator alloc{maxCu0 + 1};

    auto cuIdxOf = [](LinkedSymbolKey const& k) -> std::uint32_t {
        return k.cuId.v - 1;
    };

    // (3a) one canonical merged id per externally-visible winner NAME. Use the
    // winner's natural value when it is a free CU0 value; otherwise mint fresh.
    //
    // ★★ IN SORTED NAME ORDER. `resolution.winners` is an `unordered_map`, and
    // this loop MINTS SYMBOL IDS as it goes — so ranging over it directly makes
    // the merged symbol numbering, and therefore the emitted symbol table, a
    // function of the standard library's hash-table layout rather than of the
    // input. Sorting the names first makes the numbering a pure function of the
    // CU set, which is what lets two hosts (and two stdlib versions) agree on
    // the artifact. Sibling of the `MirBuilder::finish` phi-pool flush ordering.
    std::vector<std::string> winnerNames;
    winnerNames.reserve(resolution.winners.size());
    for (auto const& [name, winKey] : resolution.winners) {
        (void)winKey;
        winnerNames.push_back(name);
    }
    std::sort(winnerNames.begin(), winnerNames.end());
    for (std::string const& name : winnerNames) {
        LinkedSymbolKey const& winKey = resolution.winners.at(name);
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

    // A symbol-address value nested anywhere inside one literal — the THIRD
    // symbol carrier, beside `GlobalAddr` and `BlockAddressExport`, and the one
    // a scan written from the instruction opcodes alone misses. It is exactly
    // the set `remapLiteralSymbols` rewrites, so planning and remapping now walk
    // the same shape; a body whose literal named a symbol the plan never saw
    // would abort in `remapLiteralSymbols` with no way to tell which body did it.
    // D-MIR-NESTED-AGGREGATE-LITERAL-WALKS-RECURSE-PER-INITIALIZER-LEVEL: was a
    // `self(self, …)` recursive lambda, one host frame per brace level, no cap.
    // ⚠ The ASSIGNMENT ORDER is observable (symbols are numbered as they are
    // assigned), which is why the shared walker preserves field order.
    auto const assignLiteralSymbols = [&](std::uint32_t ci,
                                          MirLiteralValue const& v) -> void {
        forEachLiteralNode(v, [&](MirLiteralValue const& n) {
            if (auto const* sa = std::get_if<MirSymbolAddrValue>(&n.value)) {
                SymbolId const s{sa->symbol};
                assignSymbol(ci, s, cus[ci].nameOf(s), /*ffiRow=*/nullptr,
                             /*isLocalDef=*/false);
            }
        });
    };

    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        if (isImportSource(ci)) {
            // ── AN IMPORT SOURCE PLANS ONLY WHAT ITS SELECTED BODIES NEED ────
            // Each selected function's own symbol, plus every symbol those
            // bodies reference through any of the three carriers. Planning the
            // WHOLE source CU would also be correct, but it is O(program) per
            // importer — precisely the serial whole-program cost the lazy import
            // edge exists to remove, reintroduced in the planning pass.
            //
            // Every REFERENCE is assigned with `isLocalDef=false` so it folds
            // onto the cross-CU winner when one exists. A reference with no
            // winner mints a private id and reaches the clone with no definition
            // behind it — which is why the caller owes satisfiability (see
            // `MergeCuInput::importOnly`); the lazy import driver refuses such an
            // import rather than emitting a module that cannot link.
            std::size_t const nfSrc = m.moduleFuncCount();
            for (std::uint32_t fi = 0; fi < nfSrc; ++fi) {
                MirFuncId const f = m.funcAt(fi);
                std::string const fname = cus[ci].nameOf(m.funcSymbol(f));
                if (!contributesFunction(ci, fname)) continue;
                assignSymbol(ci, m.funcSymbol(f), fname, /*ffiRow=*/nullptr,
                             /*isLocalDef=*/m.funcBinding(f)
                                            == SymbolBinding::Local);
                std::uint32_t const nb = m.funcBlockCount(f);
                for (std::uint32_t bi = 0; bi < nb; ++bi) {
                    MirBlockId const b = m.funcBlockAt(f, bi);
                    std::uint32_t const ni = m.blockInstCount(b);
                    for (std::uint32_t ii = 0; ii < ni; ++ii) {
                        MirInstId const inst = m.blockInstAt(b, ii);
                        switch (m.instOpcode(inst)) {
                        case MirOpcode::GlobalAddr: {
                            SymbolId const s = m.globalAddrSymbol(inst);
                            assignSymbol(ci, s, cus[ci].nameOf(s),
                                         /*ffiRow=*/nullptr,
                                         /*isLocalDef=*/false);
                            break;
                        }
                        case MirOpcode::BlockAddressExport:
                            // Anonymous + local, the same treatment the whole-CU
                            // arm below gives it, and for the same reason.
                            assignSymbol(ci, m.blockAddressExportSymbol(inst),
                                         /*name=*/std::string{},
                                         /*ffiRow=*/nullptr,
                                         /*isLocalDef=*/true);
                            break;
                        case MirOpcode::Const:
                            assignLiteralSymbols(
                                ci, m.literalValue(m.constLiteralIndex(inst)));
                            break;
                        default:
                            break;
                        }
                    }
                }
            }
            continue;
        }
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
        // P69 (lane `cs`): an alias's name is a definition of its unit — folded by
        // name onto its winner like any other, or module-private when it is Local.
        for (MirAlias const& a : m.aliases()) {
            assignSymbol(ci, a.alias, cus[ci].nameOf(a.alias), /*ffiRow=*/nullptr,
                         /*isLocalDef=*/a.linkage.binding == SymbolBinding::Local);
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
        // D-C-LABEL-ADDRESS-IN-A-STATIC-INITIALIZER-REFUSED: a BLOCK symbol minted
        // at HIR→MIR for a label whose address a static initializer took. It is a
        // real symbol in every downstream sense — a `MirSymbolAddrValue` initializer
        // relocates against it and the object file defines it as a local — but it is
        // NOT a function, a global, or an extern, so the three loops above walk past
        // it. Left unassigned, `remapLiteralSymbols` aborts the merge on the first
        // 2-TU build containing one ("no unified merged id"), and no single-CU test
        // can see that.
        //
        // ANONYMOUS AND LOCAL, both deliberately: it names an interior point of one
        // function's code, so it must never collapse onto a same-named winner from
        // another CU. Passing an empty name routes it straight to the mint arm,
        // which is the same treatment the merge already gives a `static` object.
        std::size_t const nfx = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nfx; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            std::uint32_t const nb = m.funcBlockCount(f);
            for (std::uint32_t bi = 0; bi < nb; ++bi) {
                MirBlockId const b = m.funcBlockAt(f, bi);
                std::uint32_t const ni = m.blockInstCount(b);
                for (std::uint32_t ii = 0; ii < ni; ++ii) {
                    MirInstId const inst = m.blockInstAt(b, ii);
                    if (m.instOpcode(inst) != MirOpcode::BlockAddressExport) continue;
                    assignSymbol(ci, m.blockAddressExportSymbol(inst),
                                 /*name=*/std::string{}, /*ffiRow=*/nullptr,
                                 /*isLocalDef=*/true);
                }
            }
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

    // ★ P69 (lane `cs`) — A LOSING DEFINITION AN ALIAS OF ITS OWN UNIT NAMES. An
    // alias names a DEFINITION, never the name, so when the alias's target loses
    // its name to another unit's definition, what the alias is depends on what the
    // link does with the loser's bytes — and the LINK's own document states that
    // (`supersededDefinition`, per kind of weak definition); the merge reads the
    // same statement the unit linker reads (`linker::mergeModules`):
    //   * keepsItsBytes (ELF; PE's overridable kind): the loser's body stays, and
    //     the alias stays on it (✔MEASURED by the document's own cells: a name kept
    //     on a superseded weak definition reads that definition's bytes under GNU ld,
    //     ld.lld, link.exe and lld-link). So the merge keeps that body under a FRESH
    //     module-private id that only the alias names (`keptLoserBody`); every
    //     reference of the unit to the target's NAME still goes to the winner, as
    //     the link sends it;
    //   * replacedWhole (Mach-O; PE's select-any kind): the loser's bytes are the
    //     winner's, so the alias names the winning definition (step 5b);
    //   * unstated: refused by name, as the link refuses it (step 5b).
    std::unordered_set<CuSymKey, CuSymKeyHash> aliasTargetKeys;
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        if (isImportSource(ci)) continue;
        for (MirAlias const& a : cus[ci].mir->aliases())
            aliasTargetKeys.insert(CuSymKey{ci, a.target.v});
    }
    std::unordered_map<CuSymKey, SymbolId, CuSymKeyHash> keptLoserBody;
    auto const lostBodyKept = [&](std::uint32_t ci, SymbolId sym, MirLinkage const& linkage) {
        if (!aliasTargetKeys.contains(CuSymKey{ci, sym.v})) return false;
        return linkFormat.supersededDefinition().answerFor(linkage.statedWeakKind())
               == std::optional<SupersededDefinition>{SupersededDefinition::KeepsItsBytes};
    };
    auto const keepLoserBody = [&](std::uint32_t ci, SymbolId sym, std::string const& name) {
        SymbolId const fresh = alloc.mint();
        if (!name.empty()) plan.symbolNames.emplace(fresh.v, name);
        keptLoserBody.emplace(CuSymKey{ci, sym.v}, fresh);
        return fresh;
    };

    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        Mir const& m = *cus[ci].mir;
        std::size_t const nf = m.moduleFuncCount();
        for (std::uint32_t fi = 0; fi < nf; ++fi) {
            MirFuncId const f = m.funcAt(fi);
            std::string const name = cus[ci].nameOf(m.funcSymbol(f));
            // An import source contributes only its SELECTED bodies.
            if (!contributesFunction(ci, name)) continue;
            if (isShadowedLoser(ci, name, m.funcSymbol(f),
                                m.funcBinding(f) == SymbolBinding::Local)) {
                // A weak loser (never a Local — module-private, kept) is gone, unless
                // an alias of its unit names it and the link keeps its bytes (above).
                if (lostBodyKept(ci, m.funcSymbol(f), m.funcLinkage(f))) {
                    FunctionCloner cloner{m, *cus[ci].interner, ci, plan, host, builder};
                    (void)cloner.clone(f, keepLoserBody(ci, m.funcSymbol(f), name),
                                       /*keptAsModulePrivate=*/true);
                }
                continue;
            }
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
        // An import source contributes NO global — see step (1)'s note.
        if (isImportSource(ci)) continue;
        std::size_t const ng = m.moduleGlobalCount();
        for (std::uint32_t gi = 0; gi < ng; ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            std::string const name = cus[ci].nameOf(m.globalSymbol(g));
            // A weak loser is gone, unless an alias of its unit names it and the link
            // keeps its bytes (step 4's note): then it stays, module-private, under an
            // id of its own.
            bool const lost = isShadowedLoser(ci, name, m.globalSymbol(g),
                                              m.globalBinding(g) == SymbolBinding::Local);
            if (lost && !lostBodyKept(ci, m.globalSymbol(g), m.globalLinkage(g))) continue;

            TypeId const ty = reinternType(*cus[ci].interner, m.globalType(g),
                                           host, plan.typeRemap[ci],
                                           plan.compositeIdentity);
            SymbolId const mergedSym = lost ? keepLoserBody(ci, m.globalSymbol(g), name)
                                            : mergedSymbolOf(plan, ci, m.globalSymbol(g));

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
                        "(PIN-MERGE-GLOBAL-INITFUNC).\n",
                        ci, m.globalSymbol(g).v, m.funcSymbol(oldInitFunc).v);
                    std::abort();
                }
                newInitFunc = it->second;
            }

            // P69 fold 3: a COMMON that survives is the ONE object of every unit
            // that defined the name tentatively, so it takes the WIDEST
            // alignment any of them asked for (`commonAlignment`, measured
            // above for a name with two or more carriers) — its own type is
            // already the largest, which is why it is the survivor. Stated on
            // the merged global ONLY where a sibling asked for more than the
            // survivor's own definition does: a global that is not a common, a
            // common with no sibling, and a common whose siblings ask for no
            // more keep exactly what their unit stated.
            std::uint32_t alignmentBytes = m.globalAlignmentBytes(g);
            if (auto const widest = commonAlignment.find(name);
                widest != commonAlignment.end() && m.globalIsCommon(g)) {
                for (TentativeCarrier const& c : tentativeCarriers.at(name)) {
                    if (c.cu == ci && c.global.v == g.v && widest->second > c.alignment) {
                        alignmentBytes = widest->second;
                    }
                }
            }
            (void)builder.addGlobal(ty, mergedSym, newInitLit, newInitFunc,
                                    // P69: the linkage crosses as its one
                                    // record — binding, visibility, the weak
                                    // kind and the tentative mark together (a
                                    // kept loser is module-private).
                                    lost ? MirLinkage{SymbolBinding::Local,
                                                      SymbolVisibility::Default}
                                         : m.globalLinkage(g),
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
                                    alignmentBytes);
        }
    }

    // ── (5b) the ALIASES (P69, lane `cs`) — each one that won its name, and every
    // module-private one, as one more name on its target's MERGED definition. An
    // alias that lost its name is gone with it, exactly as a losing definition is.
    // An alias whose TARGET lost its own name is what the link's document says the
    // loser's bytes become (step 4's note): on the kept loser body, on the winner,
    // or refused by name.
    for (std::uint32_t ci = 0; ci < cus.size(); ++ci) {
        if (isImportSource(ci)) continue;
        Mir const& m = *cus[ci].mir;
        if (m.moduleAliasCount() == 0) continue;
        std::unordered_map<std::uint32_t, MirLinkage> targetLinkage;
        for (std::uint32_t fi = 0; fi < m.moduleFuncCount(); ++fi) {
            MirFuncId const f = m.funcAt(fi);
            targetLinkage.emplace(m.funcSymbol(f).v, m.funcLinkage(f));
        }
        for (std::uint32_t gi = 0; gi < m.moduleGlobalCount(); ++gi) {
            MirGlobalId const g = m.globalAt(gi);
            targetLinkage.emplace(m.globalSymbol(g).v, m.globalLinkage(g));
        }
        for (MirAlias const& a : m.aliases()) {
            std::string const name = cus[ci].nameOf(a.alias);
            bool const local = a.linkage.binding == SymbolBinding::Local;
            if (isShadowedLoser(ci, name, a.alias, local)) continue;
            auto const target = targetLinkage.find(a.target.v);
            std::string const targetName = cus[ci].nameOf(a.target);
            auto const refuse = [&](std::string const& why) {
                ParseDiagnostic d;
                d.code     = DiagnosticCode::K_CrossCuMergeUnsupported;
                d.severity = DiagnosticSeverity::Error;
                d.actual   = "mergeCuMirs: alias '" + name + "' (compilation unit #" +
                             std::to_string(ci + 1) + ") names the definition '" +
                             targetName + "', " + why;
                reporter.report(std::move(d));
            };
            if (target == targetLinkage.end()) {
                refuse("which its unit does not define — an alias is one more name on "
                       "a definition of its own unit.");
                continue;
            }
            SymbolId mergedTarget = mergedSymbolOf(plan, ci, a.target);
            if (isShadowedLoser(ci, targetName, a.target,
                                target->second.binding == SymbolBinding::Local)) {
                if (auto const kept = keptLoserBody.find(CuSymKey{ci, a.target.v});
                    kept != keptLoserBody.end()) {
                    mergedTarget = kept->second;   // the link keeps the loser's bytes
                } else if (linkFormat.supersededDefinition().answerFor(
                               target->second.statedWeakKind())
                           != std::optional<SupersededDefinition>{
                                  SupersededDefinition::ReplacedWhole}) {
                    refuse("which another unit's definition of '" + targetName +
                           "' replaces, and the link's document states nothing of what "
                           "becomes of a replaced definition of its kind "
                           "(`supersededDefinition`) — refused, as the link refuses it.");
                    continue;
                }
                // replacedWhole: the loser's bytes are the winner's, and so is the
                // alias (`mergedTarget` is the winner's id).
            }
            builder.addAlias(mergedSymbolOf(plan, ci, a.alias), mergedTarget, a.linkage);
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
                     ") — one dynamic symbol cannot be imported two ways: declare "
                     "it the same way in every compilation unit.";
        // Anchored: D-LK11-EXTERN-IMPORT-DEDUP.
        reporter.report(std::move(d));
    };
    // `dataSizeBytes` / `dataAlignBytes` SIZE the ELF copy-relocation `.bss` slot
    // (c84 D-LK-EXTERN-DATA-IMPORT). One CU legitimately holds an INCOMPLETE type
    // (`extern const char v[];` ⇒ 0/0 — see `ExternImport::dataSizeBytes` in
    // extern_import.hpp), so a zero is "unknown here", not a disagreement:
    // take the non-zero. Two DIFFERING
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
        // An import source contributes NO extern row. Its rows describe ITS
        // module's dependencies, not the importer's, and folding them in would
        // make an importer's emitted import table depend on which bodies it
        // happened to page in. The lazy import driver copies exactly the rows a
        // SELECTED body actually needs onto the IMPORTER instead, so the row
        // arrives as the importer's own dependency and rides its identity.
        if (isImportSource(ci)) continue;
        for (ExternImport const& e : cus[ci].externImports) {
            // → direct, strip — unless an ALIAS won the name: then the reference
            // stays one to the name, and the link resolves it to the alias row.
            if (plan.definedNames.count(e.mangledName)
                && !aliasWonNames.contains(e.mangledName)) continue;
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
                // Anchored: D-LK11-EXTERN-IMPORT-DEDUP.
                std::fprintf(stderr,
                    "dss::mergeCuMirs fatal: CU %u extern import \"%s\" folds into "
                    "the dedup group canonicalized to merged symbol v=%u, but its "
                    "planned merged symbol is v=%u — step 3b's import-identity key "
                    "and step 6's must be THE SAME key.\n",
                    ci, e.mangledName.c_str(), kept.symbol.v, mergedSym.v);
                std::abort();
            }
            // `isEagerImport` — OR-COMBINE, as `ExternImport::isEagerImport`'s own
            // contract note in extern_import.hpp mandates
            // (D-LINK-EXTERN-IMPORT-REFERENCE-GATE (e)). Keeping a NON-eager row
            // when a sibling CU declared the same import EAGER lets the linker's
            // `rejectOrDropUnreferencedExterns` DROP a
            // shipped-descriptor symbol the loader must bind
            // (D-FFI-DESCRIPTOR-EAGER-IMPORT) — a LOAD failure (pe 0xC0000139 /
            // elf exit 127), not a size regression. Order-INDEPENDENT: whichever
            // CU's row lands first, the bit is ORed in.
            kept.isEagerImport = kept.isEagerImport || e.isEagerImport;
            // `binding` — STRONGEST WINS, as `ExternImport::binding`'s own
            // contract note in extern_import.hpp mandates
            // (D-CSUBSET-WEAK-EXTERN-IMPORT-NOT-IN-SYMBOL-TABLE). A STRONG
            // reference anywhere in the program makes the symbol REQUIRED, so a
            // CU that declared the import plain must not have its requirement
            // erased by a sibling CU that declared the same name `weak` — that
            // erasure is a program which links with the symbol absent and then
            // reads through a null address. NOT a conflict like `isData`: both
            // rows name the same object bound the same way and differ only in
            // whether their own TU can do without it, which is a DEFINED fold.
            // Order-INDEPENDENT: `Weak` < `Global` in the shared enum, so the max
            // is the same whichever row lands first, and Local never reaches here
            // (`collectExterns` refuses it at the declaration). The rule lives in
            // `strongerReferenceBinding` so this tier and the linker's dedup fold
            // read ONE owner — see its docblock for why it does not compare the
            // enumerators numerically.
            kept.binding = strongerReferenceBinding(kept.binding, e.binding);
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
                        "`dataSizeBytes` (declared object size)", e);
            foldNonZero(kept.dataAlignBytes, e.dataAlignBytes,
                        "`dataAlignBytes` (declared object alignment)", e);
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
            // An import source is not a participant, so it cannot supply the
            // program's entry point even if it happens to define `main`.
            if (isImportSource(ci)) continue;
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

    // The merged module's id space ends where the allocator stopped. Stated HERE,
    // after the last allocation, so the module carries it (`Mir::symbolIdEnd`)
    // and the passes that synthesize into the merged module ask the module.
    builder.stateSymbolIdEnd(alloc.end());
    Mir merged = std::move(builder).finish();

    // Canonical-marker stamping (D-OPT4-1): clones copy markers verbatim
    // and per-function CFGs are unchanged by the merge, so this is a
    // uniformity re-stamp — it keeps the equality verifier below green
    // BY CONSTRUCTION even when an input CU carried stale stamps.
    rederiveStructCfMarkers(merged);

    // ── verify the merged module before returning (the engine's verify-after-
    // every-transform discipline). A non-verifying merge is a build break, never
    // a silent miscompile.
    //
    // ★★ THIS VERIFY IS THE SECOND WHOLE-PROGRAM `MirVerifier` RUN OF A BUILD,
    // and until cycle P36 it was INVISIBLE: `mergeCuMirs` is called from the
    // driver OUTSIDE any `PhaseTimers::Scope`, so its cost landed in the
    // `--time` report's `[other]` row, which names nothing
    // ([[D-PERF-MERGE-SIDE-WHOLE-PROGRAM-VERIFY-IS-UNATTRIBUTED]]). It is
    // scoped as `CompilePhase::Verify` — the phase whose verb IS "the
    // MirVerifier ran" — so the row now counts EVERY whole-module verify a
    // build pays instead of only the optimizer's. ⓘ The `runs` column is what
    // keeps the two distinguishable: it now reads (CUs + 1 optimizer verifies)
    // + 1 merge verify.
    {
        substrate::PhaseTimers::Scope const verifyScope{
            substrate::CompilePhase::Verify};
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
