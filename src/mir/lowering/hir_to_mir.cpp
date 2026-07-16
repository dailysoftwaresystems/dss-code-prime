#include "mir/lowering/hir_to_mir.hpp"

#include "core/types/aggregate_abi.hpp"
#include "core/types/call_payload.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/semantic_config.hpp"  // c103: BuiltinLowering (BuiltinCall payload)
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir_op.hpp"
#include "mir/mir_struct_markers.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>       // FC17.5 F2: the string-global byte-content memo
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

// HIR→MIR literal value conversion. The two pools' value variants are
// structurally parallel — scalar arms (monostate / bool / int64 / uint64 /
// double / string) copy verbatim; the aggregate arm (`HirAggregateValue`
// → `MirAggregateValue`, D5.3) recurses field-by-field so a folded
// aggregate `constInit` carries the same shape on the MIR side.
[[nodiscard]] MirLiteralValue toMirLiteral(HirLiteralValue const& src) {
    MirLiteralValue dst;
    dst.core = src.core;
    std::visit([&](auto const& arm) {
        using T = std::decay_t<decltype(arm)>;
        if constexpr (std::is_same_v<T, HirAggregateValue>) {
            MirAggregateValue agg;
            agg.fields.reserve(arm.fields.size());
            for (auto const& f : arm.fields) agg.fields.push_back(toMirLiteral(f));
            dst.value = std::move(agg);
        } else if constexpr (std::is_same_v<T, HirAddressValue>) {
            // c43 (D-CSUBSET-ADDRESS-CONSTANT-FOLD): an HIR address constant maps
            // field-for-field to the MIR symbol-address relocation (F5). The
            // fold-transient `pointeeType` is dropped — MIR addresses carry only
            // {symbol, addend}. A NULL-base address never reaches here (the
            // const-eval engines collapse it to an integer before pooling); if one
            // did, the asm emitter's symbol-address arm would fail loud on symbol 0.
            dst.value = MirSymbolAddrValue{arm.base, arm.byteOffset};
        } else if constexpr (std::is_same_v<T, BitIntValue>) {
            // C4b (D-CSUBSET-BITINT-WIDE-LITERAL / I1+C2): the `_BitInt` bit-precise
            // value is the SAME host type in both pools — copy it directly (NEVER
            // degrade to monostate, which would silently drop the value). The narrow-
            // literal MIR lowering extracts the container int from this arm; the
            // wide path fills limbs; the globals emitter fails loud on it.
            dst.value = arm;
        } else {
            dst.value = arm;
        }
    }, src.value);
    return dst;
}

// One transient per `lowerToMir` call. The HirLowering analog is `Lowerer`
// in cst_to_hir.cpp; this is much smaller because MIR is structurally
// simpler than CST (no schema-driven shape dispatch, no per-language
// vocabulary).
struct Lowerer {
    Hir const&               hir;
    HirLiteralPool const&    literals;
    TypeInterner&            interner;
    DiagnosticReporter&      reporter;
    HirSourceMap const*      sourceMap;   // optional — diagnostics carry spans when bound
    MirLoweringConfig const& config;      // schema-driven knobs (plan 12.5 §0.2 D3)
    HirFfiMap const*         ffiMap;      // optional — populated by CST→HIR or FF5
                                           // (LK6 cycle 2d, D-LK6-6).
    HirLinkageMap const*     linkageMap;  // optional — native-decl binding/
                                           // visibility (D-CSUBSET-LINKAGE-
                                           // SPECIFIERS). nullptr ⇒ all Global.
    HirMutabilityMap const*  mutabilityMap; // optional — native-global const-ness
                                           // (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL).
                                           // nullptr / no entry ⇒ mutable.
    HirVolatileMap const*    volatileMap; // optional — per-ACCESS volatility (c21,
                                           // D-CSUBSET-VOLATILE-QUALIFIER). nullptr /
                                           // no entry ⇒ plain (non-volatile) access.
    HirReturnsTwiceMap const* returnsTwiceMap; // optional — per-CALL returns-twice
                                           // (FC17.9(c), D-CSUBSET-SETJMP). nullptr /
                                           // no entry ⇒ ordinary call (no flag).
    HirAlignmentMap const*   alignmentMap; // optional — per-DECLARATION explicit
                                           // `alignas` (D-CSUBSET-ALIGNAS-VARIABLE-
                                           // CODEGEN). nullptr / no entry ⇒ natural.
    HirThreadLocalMap const* threadLocalMap; // optional — per-DECLARATION thread
                                           // storage duration (TLS C1,
                                           // D-CSUBSET-THREAD-LOCAL). nullptr / no
                                           // entry ⇒ ordinary process-shared.
    // VLA C1a/C3 (D-CSUBSET-VLA): optional — a block-scope variable-length array
    // local's per-DIMENSION SIZE-expression HIR nodes (outer→inner order), keyed by
    // SymbolId.v. nullptr / no entry ⇒ the local is not a VLA. Read in
    // `vlaAllocaForLocal` to lower each runtime bound + form the cumulative row
    // strides and the total byte size at the DECL point. A 1-D VLA has one entry.
    std::unordered_map<std::uint32_t, std::vector<HirNodeId>> const* vlaSizeMap =
        nullptr;
    // VLA C2 (D-CSUBSET-VLA): a `sizeof <vla-object>` SizeOf HIR node id.v → the VLA
    // operand's SymbolId.v. nullptr / no entry ⇒ a plain (static-fold) sizeof. Read in
    // the MIR SizeOf case to emit a runtime Load of the decl-frozen size slot below.
    std::unordered_map<std::uint32_t, std::uint32_t> const* sizeofVlaSymMap = nullptr;
    // VLA C4b (D-CSUBSET-VLA): a VLA-typedef OBJECT's SymbolId.v → its typedef origin
    // R's SymbolId.v. nullptr / no entry ⇒ not a VLA typedef. Read in `allocaForLocal`
    // to route `R a;` to `vlaAllocaFromTypedef`, which copies R's decl-frozen size
    // slots down (freeze-once) instead of re-lowering the bound.
    std::unordered_map<std::uint32_t, std::uint32_t> const* typedefVlaOriginMap =
        nullptr;
    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): pe64 <threads.h> shim SymbolId.v →
    // recipe id. nullptr / empty ⇒ no threads shim. The `collectThreadShimSymbols`
    // pre-pass SEEDS `functionSymbols` with each key so the user's `mtx_lock(&m)` call
    // lowers to `GlobalAddr(sym)` (the callee is defined later by the synth pass, which
    // MirVerifier tolerates) instead of failing loud "HIR Ref to unbound symbol".
    std::unordered_map<std::uint32_t, std::string> const* synthRecipeMap = nullptr;
    MirBuilder               mir;
    // Extern symbols extracted during the pre-pass. Each extern's
    // SymbolId is also inserted into `functionSymbols` so a `Ref`
    // to it routes through the existing GlobalAddr emission path.
    std::vector<ExternImport> externImports;
    // Within one function: HIR `SymbolId.v` → SSA value producer. Used for
    // params that are NOT address-taken (those stay as raw `Arg` instructions);
    // an entry is set iff the symbol resolves as a plain rvalue. A symbol is
    // EITHER in `symbolToValue` (SSA-only) OR in `addressableLocal` (slot-
    // backed), never both — `Ref` lookup checks alloca first and falls back.
    std::unordered_map<std::uint32_t, MirInstId> symbolToValue;
    // HIR `SymbolId.v` → its entry-block `Alloca` instruction. Populated by
    // `VarDecl` lowering (every body-local var gets a slot) and by the param
    // slot-promotion pre-pass (params whose address is taken via `AddressOf`).
    // `Ref` reads emit `Load(alloca)`; `AssignStmt` writes emit `Store(value,
    // alloca)`; `AddressOf(Ref(sym))` returns the alloca itself.
    std::unordered_map<std::uint32_t, MirInstId> addressableLocal;
    // VLA C2/C3 (D-CSUBSET-VLA): within one function, a VLA object's per-LEVEL
    // decl-frozen BYTE-size slots, keyed by `pack(SymbolId.v, TypeId.v)` where the
    // TypeId is the level's SHAPE type (CRITICAL-1: keyed by the SUBSCRIPT-RESULT
    // type, so each slot holds EXACTLY `sizeof(that type)` by construction). The
    // whole-object entry is (sym, declaredType) = `sizeof a` (the C2 total); each
    // intermediate VLA row is (sym, rowType) = the runtime stride an index steps by
    // AND `sizeof a[0]`. Materialized once at the decl by `vlaAllocaForLocal`. The MIR
    // SizeOf case Loads (sym, sized-type); `scaleIndexToBytes` Loads (root-sym,
    // Index-result-type). Cleared per function. `pack` = (uint64(sym)<<32)|typeId.
    std::unordered_map<std::uint64_t, MirInstId> vlaStrideSlot;
    static std::uint64_t vlaSlotKey(std::uint32_t symV, std::uint32_t typeV) {
        return (static_cast<std::uint64_t>(symV) << 32) | typeV;
    }
    // FC7 (D-FC7-MEMBER-ACCESS): per-CU memoized struct/union layouts keyed
    // by TypeId.v. `computeLayout` is PURE and a TypeId's layout is
    // invariant within the CU (one interner + one target config), so this
    // caches across functions and is never cleared. Read by
    // `fieldByteOffset` (member-access offset resolution) and
    // `aggregateByteSize` (aggregate-local Alloca sizing).
    std::unordered_map<std::uint32_t, StructLayout> layoutCache_;
    // FC17.5 F2 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER): per-MODULE byte-content
    // memo for `materializeStringLiteralGlobal` — (interned array TypeId.v,
    // exact byte content) → the already-minted rodata global's SymbolId. C
    // 6.4.5p7 PERMITS identical string literals to share one array; `__func__`
    // REQUIRES it (C99 6.4.2.2 declares ONE static array per function — two
    // reads folded to two literals must compare EQUAL after decay, so both
    // must materialize to the SAME global). Keying on the TypeId keeps
    // same-bytes-different-element-width literals (narrow "ab" vs u"ab"'s
    // code units) distinct by construction. Never cleared — rodata globals
    // are module-level (the layoutCache_ precedent). Also a size win: sqlite's
    // repeated literals collapse to one rodata object each.
    std::map<std::pair<std::uint32_t, std::string>, SymbolId> stringGlobalMemo_;
    // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): per-function by-value return
    // state, reset at the top of each `lowerFunction`. `currentFnResult_` is the
    // enclosing function's result type — a `ReturnStmt` needs it to classify a
    // by-value struct/union return (its only other access is the FnSig in
    // lowerFunction). `sretPtr_` is the hidden result pointer when the return is
    // class MEMORY (>16B, ByReference): the ReturnStmt copies the result through
    // it and returns it. InvalidMirInst for a scalar or in-register (≤16B) return.
    TypeId    currentFnResult_{};
    MirInstId sretPtr_{};
    // Set of module-level function symbols. A pre-pass populates this so a
    // direct `Call` whose callee is a `Ref` to a forward-declared function
    // resolves cleanly. The actual MirFuncId is irrelevant during lowering —
    // direct calls go through `GlobalAddr(SymbolId)`, and codegen wires the
    // symbol to the MirFunc later. Hence: set, not map.
    std::unordered_set<std::uint32_t> functionSymbols;
    // Set of module-level global symbols. Populated by the global pre-pass
    // alongside `functionSymbols` so a `Ref` to a global resolves to a
    // `GlobalAddr(SymbolId)` (consumed via `Load` for reads, used as the
    // pointer operand of `Store` for writes). The MIR's `addGlobal` records
    // the storage; codegen later wires the symbol to that arena entry.
    std::unordered_set<std::uint32_t> globalSymbols;
    // TLS C1 (D-CSUBSET-THREAD-LOCAL, ★CRIT-1): the module's THREAD-LOCAL
    // symbols (intra-module globals + extern data), populated by
    // `collectGlobals`/`collectExterns` from the HirThreadLocalMap — i.e.
    // COMPLETE before `classifyGlobals` runs. Consulted by the
    // symbol-address initializer classification: the ADDRESS of a
    // thread-local object is NOT an address constant (C11 6.6p9 — it
    // differs per thread, computable only at runtime against the executing
    // thread's TLS block), so a static-storage initializer naming one fails
    // loud S_ThreadLocalAddressNotConstant instead of minting a
    // MirSymbolAddrValue whose resolved abs64 would be the link-time tpoff
    // bit-cast into a data slot (a silent garbage pointer).
    std::unordered_set<std::uint32_t> threadLocalTargetSymbols;
    // The synthesized module-init function — created lazily when the first
    // non-constant initializer needs runtime evaluation. Each subsequent
    // non-constant init appends a Store-into-global into this function's
    // entry block. If still invalid at finish() time, no module-init was
    // needed and none is emitted.
    MirFuncId moduleInitFunc{};
    // Stack of enclosing loop/switch frames. `BreakStmt`/`ContinueStmt` are
    // resolved by indexing into this stack with HIR's `branchDepth` (a de
    // Bruijn index — 0 means innermost). Loops contribute both edges;
    // switches only contribute a break edge (a `continue` aimed at a switch
    // is an HIR verifier failure — `continueBB.valid()` is the runtime
    // assertion). Frames are pushed by `WhileStmt`/`DoWhileStmt`/`ForStmt`/
    // `SwitchStmt` lowering and popped on scope exit (RAII via a small
    // helper to keep the push/pop discipline visible at each call site).
    struct BranchFrame {
        MirBlockId continueBB;   // invalid for switch (no continue target)
        MirBlockId breakBB;
        // True iff a `continue;` inside this frame's body resolved here.
        // Used by `DoWhileStmt` to decide whether to lower the (otherwise
        // dead) condition block when the body self-sealed.
        bool       continueReferenced = false;
        // VLA C5 (D-CSUBSET-VLA, audit fix #3): the `vlaScopeStack_.size()` captured
        // when THIS loop/switch frame was pushed. A `break`/`continue` targeting this
        // frame exits every VLA scope opened INSIDE it (indices >= this depth), so it
        // restores SP to the entry watermark of `vlaScopeStack_[vlaDepthAtPush]` (the
        // shallowest VLA scope strictly inside the target) — or nothing if the loop/
        // switch declared no VLA (size == depth).
        std::size_t vlaDepthAtPush = 0;
    };
    std::vector<BranchFrame> branchStack;

    // VLA C5 (D-CSUBSET-VLA): the stack of currently-OPEN block-scope VLA
    // watermarks, innermost on the back. One frame per dynamic-VLA VarDecl (the
    // finer per-decl granularity that makes a backward `goto` between two VLAs
    // correct). Pushed at the VarDecl (with `saveBefore` = the StackSave emitted
    // just BEFORE the VLA's `sub sp`); popped when the declaring lexical block's
    // lowering completes. Correlated with `branchStack` (break/continue) via
    // `vlaDepthAtPush`, and walked for `goto` teardown via HIR ancestry. Reset per
    // function. See the teardown helpers below.
    struct VlaScopeFrame {
        HirNodeId  declNode;    // the VLA VarDecl (textual-position anchor for goto)
        HirNodeId  scopeNode;   // the enclosing lexical Block (== hir.parent(declNode))
        MirInstId  saveBefore;  // the StackSave value (SP captured before this VLA)
        std::uint32_t scopeId;  // the StackSave/StackRestore pairing payload
    };
    std::vector<VlaScopeFrame> vlaScopeStack_;
    // VLA C5: per-function monotonic scopeId stamped on each StackSave/StackRestore
    // (the verifier pairs Restore→Save on it). Reset per function.
    std::uint32_t vlaScopeCounter_ = 0;
    // VLA C5: per-function label-ordinal → LabelStmt HIR node, for resolving a
    // `goto`'s target label to compute which VLA scopes the jump exits (the
    // ancestry walk). Populated at function entry; reset per function.
    std::unordered_map<std::uint32_t, HirNodeId> labelNodeByOrdinal_;

    // FC12a-core (D-FC12A-VARIADIC-CALLEE): the enclosing function's count of FIXED
    // (non-variadic) params that consumed an integer / SSE arg register. `va_start`
    // initializes gp_offset = fixedGpr*gpSlotBytes and fp_offset = gpOffsetLimit +
    // fixedSse*fpSlotBytes from these (the SysV cursor past the fixed args into the
    // register-save-area). Set by the function-lowering method BEFORE the body is
    // lowered (so a body `va_start` reads the right counts); reset per function.
    std::uint32_t currentFnFixedGpr_ = 0;
    std::uint32_t currentFnFixedFpr_ = 0;
    // FC12b (D-FC12B-WIN64-VARIADIC-CALLEE): the enclosing function's count of FIXED
    // named-arg SLOTS under a slot-aligned CC (Win64). The HomogeneousPointer
    // va_start sets ap = &home[currentFnFixedFlat_] — the slot count positions past
    // ALL named args (int OR float, each one slot). Equal to currentFnFixedGpr_ for
    // an int-only-named-param fn; differs only when a named param is FP (each still
    // one slot). Set alongside the per-class counts before the body is lowered.
    std::uint32_t currentFnFixedFlat_ = 0;
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the EXACT byte count of
    // FIXED params placed on the INCOMING stack, accumulated in param order as the
    // reception loop runs — a STACKED scalar contributes one outgoing slot
    // (gpSlotBytes); a STACKED by-value aggregate (it straddled the reg/stack
    // boundary, received all-or-nothing) contributes roundUp(aggBytes, slot). This
    // REPLACES the old `(gprOver + fprOver) * gpSlotBytes` slot-count formula (which
    // silently UNDERCOUNTED a stacked aggregate: the all-or-nothing cursor either
    // backfills — never exceeds the pool — or clamps to exactly the pool, so neither
    // surfaces the aggregate's bytes). lowerVaStart reads it as the
    // VaOverflowArgAreaAddr displacement (the overflow/__stack base skips the named
    // stack args). Each stacked aggregate's RecvByValueStackParam also reads the
    // cursor value AT its position as its own incoming byte offset. Reset per fn.
    std::uint32_t currentFnFixedStackBytes_ = 0;
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: set once a FIXED by-value
    // aggregate has been received WHOLLY from the incoming stack. The callee receives
    // a stacked scalar via its per-class Arg ordinal (lir_callconv sites it at
    // `(ordinal - pool) * slot`), which is consistent with the byte cursor ONLY for a
    // pure-class FIRST overflow. So a stacked fixed param of ANY kind that FOLLOWS a
    // stacked aggregate — or a second stacked aggregate, or a stacked scalar BEFORE a
    // stacked aggregate — would desync the two offset models. Rather than silently
    // miscompile, lowerFunction FAILS LOUD on those rare shapes (the witnessed cases
    // place the aggregate as the FIRST/only overflow, offset 0). Reset per fn.
    bool currentFnSawStackedAggregate_ = false;
    bool currentFnSawFixedStackParam_  = false;

    // FC5: per-function `goto`/label lowering. A LabelStmt and its goto(s) share a
    // per-function ordinal (HIR payload); this maps the ordinal → its MIR block.
    // `getOrCreateLabelBlock` is lazy so a FORWARD goto (`goto end;` before `end:`)
    // and a BACKWARD goto resolve uniformly — whichever reaches the ordinal first
    // creates the block; the other reuses it. Unstructured edges are fine: the MIR
    // CFG is a general graph, markers are re-derived from it after `finish()`, and
    // any block left unreachable is dropped by the mandatory unreachable-prune.
    std::unordered_map<std::uint32_t, MirBlockId> labelBlocks_;
    MirBlockId getOrCreateLabelBlock(std::uint32_t ordinal) {
        auto it = labelBlocks_.find(ordinal);
        if (it != labelBlocks_.end()) return it->second;
        MirBlockId const b = mir.createBlock(StructCfMarker::Linear);
        labelBlocks_.emplace(ordinal, b);
        return b;
    }

    // c115 SEH (D-WIN64-SEH-FUNCLETS): the module-wide SEH region-id counter —
    // stamps SehTryBegin/SehFilterReturn/SehTryEnd payloads. Module-monotonic
    // (never reset per function) so ids stay unique within every function by
    // construction; the verifier pairs Begin/FilterReturn/End on it.
    std::uint32_t sehRegionCounter_ = 0;

    // c60 (Design I-A): does a flattened switch's body Block start with a case
    // marker (a LabelStmt)? When it does, the marker opens its own label-block right
    // after the Switch terminator, so no extra pre-case block is needed; only a body
    // whose first statement is a jumped-over leading declaration needs one. (An empty
    // body Block trivially "starts with a label" — there is nothing to lower, so no
    // pre-case block is needed either.)
    [[nodiscard]] bool switchBodyStartsWithLabel(HirNodeId body) const {
        if (hir.kind(body) != HirKind::Block) return false;
        auto const kids = hir.children(body);
        return kids.empty() || hir.kind(kids.front()) == HirKind::LabelStmt;
    }

    // D-CSUBSET-COMPUTED-GOTO: the per-function set of label ordinals whose ADDRESS
    // is taken via `&&label` (LabelAddressOf). Collected once at function entry (a
    // HIR pre-scan) so `goto *p` (IndirectBr) can name EVERY address-taken block as
    // a successor — the full target set must be known when the IndirectBr is built,
    // even for a `&&end` that appears textually AFTER the `goto *p`. The blocks
    // themselves are created on demand via getOrCreateLabelBlock.
    std::unordered_set<std::uint32_t> addressTakenLabelOrdinals_;

    // D-LK4-RODATA-PRODUCER-STRING (2026-06-02): synthetic-symbol
    // counter for string-literal-promoted globals. Initialized to
    // 1 + max(existing function/extern/global SymbolId.v) on first
    // use so synthetic symbols can't collide with user-declared
    // ones. Mirrors the `entry_trampoline.cpp::maxExistingSymbolIdV`
    // pattern used by D-LK10-ENTRY Slice C. Lazy seeding lets the
    // pre-passes (`collectFunctions` / `collectGlobals` /
    // `collectExterns`) populate the symbol sets BEFORE the first
    // expression lowering reaches a string literal.
    //
    // Encoded as `optional<uint32_t>` (type-design audit fold,
    // 2026-06-02): a `(bool seeded, uint32_t value)` two-field
    // pair admits the contradictory state `(false, 17)` that no
    // invariant pinned. `optional<uint32_t>` encodes "unseeded +
    // value" in one type-enforced state; `if (!opt.has_value())`
    // is the lazy-seed gate.
    std::optional<std::uint32_t> nextSyntheticGlobalSym_;
    // Returns a fresh SymbolId, OR an invalid `SymbolId{}` if the
    // mint would wrap around UINT32_MAX. The caller is expected to
    // fail loud on the invalid sentinel — silent-failure HIGH-1
    // audit fold (2026-06-02): mirrors the D-LK10-ENTRY Slice C
    // discipline `SymbolId{} == 0` sentinel + caller-checked
    // wraparound, anchored by the prior `3541177` audit fold's
    // `SymbolIdSpaceExhaustionFailsLoud` precedent. Without this
    // guard, a 2^32-occurrence string-literal corpus would silently
    // wrap and collide with SymbolId{0} / user symbols.
    [[nodiscard]] SymbolId mintSyntheticGlobalSymbol() {
        if (!nextSyntheticGlobalSym_.has_value()) {
            std::uint32_t maxV = 0;
            for (auto v : functionSymbols) {
                if (v > maxV) maxV = v;
            }
            for (auto v : globalSymbols) {
                if (v > maxV) maxV = v;
            }
            // Cross-tier collision protection (code-architect audit
            // fold, 2026-06-02): `collectExterns` (search by name
            // — line numbers drift) inserts every extern's
            // `SymbolId.v` into `functionSymbols`. The scan above
            // therefore covers functions + externs + globals — the
            // three user-symbol categories MIR sees. The HIR builder's
            // `freshSymbol()` runs past `model.symbols().size()`
            // for HIR-synthesized symbols; those SymbolIds are
            // ALREADY in `functionSymbols`/`globalSymbols` by the
            // time `mintSyntheticGlobalSymbol()` first runs, so
            // the lazy seed naturally clears them too.
            //
            // c86 (D-MIR-SYNTHETIC-GLOBAL-SYMBOL-ALIAS): the scan alone is
            // NOT enough — the SEMANTIC symbol table also holds typedefs,
            // tags, fields, locals, and injected constants, none of which
            // are MIR-visible, and the LK11 merge maps every MIR symbol to
            // a NAME through that table. A synthetic id landing inside the
            // table fabricates a NAMED strong def from an anonymous literal
            // global (bogus cross-CU collisions; potential silent
            // mis-merge). `config.syntheticSymbolFloor` (the pipeline
            // passes `model.symbols().size()`) lifts the seed clear of the
            // whole semantic id space.
            //
            // UINT32_MAX seed: refuse to seed at the saturated
            // edge so the immediate `*nextSyntheticGlobalSym_`
            // read below never wraps. The caller fail-louds.
            if (maxV == std::numeric_limits<std::uint32_t>::max()) {
                return SymbolId{};
            }
            nextSyntheticGlobalSym_ =
                std::max(maxV + 1u, config.syntheticSymbolFloor);
        }
        std::uint32_t const minted = *nextSyntheticGlobalSym_;
        // Wrap detection (silent-failure HIGH-1 audit fold): if
        // `minted == UINT32_MAX`, advancing would wrap to 0
        // (collide with the invalid sentinel). Refuse and let the
        // caller fail loud.
        if (minted == std::numeric_limits<std::uint32_t>::max()) {
            return SymbolId{};
        }
        nextSyntheticGlobalSym_ = minted + 1u;
        return SymbolId{minted};
    }

    // Emit an unsupported-construct diagnostic anchored at the HIR node's
    // source span (via the optional source map). The buffer/span both
    // default to invalid/empty when no source map is bound — matching the
    // span-less fallback the HirVerifier uses.
    void unsupported(HirNodeId node, std::string what) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::H_UnsupportedLoweringForKind;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        if (sourceMap != nullptr) {
            if (auto const* loc = sourceMap->tryGet(node); loc != nullptr) {
                d.buffer = loc->buffer;
                d.span   = loc->span;
            }
        }
        reporter.report(std::move(d));
    }

    // Emit a SPECIFIC-code positioned error from the MIR-lowering tier (the twin of
    // `unsupported`, which uses the generic H_UnsupportedLoweringForKind). Used for the
    // still-deferred wide-`_BitInt` gaps (e.g. S_BitIntWideFloatConvUnsupported,
    // D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV) — a real, unsuppressable diagnostic, never a
    // silent scalar op. (The C2 `* / %` boundary code S_BitIntWideMulDivUnsupported is
    // retired as of C3 — those ops now lower to the multi-limb path.)
    void diagnoseCode(HirNodeId node, DiagnosticCode code, std::string what) {
        ParseDiagnostic d;
        d.code     = code;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
        if (sourceMap != nullptr) {
            if (auto const* loc = sourceMap->tryGet(node); loc != nullptr) {
                d.buffer = loc->buffer;
                d.span   = loc->span;
            }
        }
        reporter.report(std::move(d));
    }

    // c21 (D-CSUBSET-VOLATILE-QUALIFIER): the access-volatility funnel. Returns
    // `MirInstFlags::Volatile` iff `accessNode` carries a VolatileAttr (set by
    // CST→HIR from the accessed object's / field's `SymbolRecord.isVolatile`),
    // else `None`. EVERY user-access Load/Store emit site passes its access node
    // through here, so the flag's coverage is by-construction at one chokepoint —
    // a missed site would be a silent miscompile (the optimizer elides/caches the
    // access). For a load the access node is the Ref / MemberAccess being read;
    // for a store it is the lvalue target (a Ref / MemberAccess) or the
    // VarDecl/Global whose object the init store targets. nullptr map / no entry
    // ⇒ a plain access (the optimizer may freely transform it).
    [[nodiscard]] MirInstFlags volatileFlagFor(HirNodeId accessNode) const {
        if (volatileMap == nullptr || !accessNode.valid())
            return MirInstFlags::None;
        if (auto const* p = volatileMap->tryGet(accessNode); p != nullptr && p->isVolatile)
            return MirInstFlags::Volatile;
        return MirInstFlags::None;
    }

    // FC17.9(c) (D-CSUBSET-SETJMP): the returns-twice funnel — the EXACT twin of
    // `volatileFlagFor`. Returns `MirInstFlags::ReturnsTwice` iff `callNode` carries a
    // ReturnsTwiceAttr (set by CST→HIR when the Call's direct callee record is
    // `SymbolRecord.returnsTwice` — a `setjmp`/`_setjmp`), else `None`. The Call-lowering
    // chokepoint (`finishCall`) passes its HIR node through here so the flag's coverage
    // is by-construction at one site. nullptr map / no entry ⇒ an ordinary call the
    // optimizer may freely transform (promote locals across it, inline its callee).
    [[nodiscard]] MirInstFlags returnsTwiceFlagFor(HirNodeId callNode) const {
        if (returnsTwiceMap == nullptr || !callNode.valid())
            return MirInstFlags::None;
        if (auto const* p = returnsTwiceMap->tryGet(callNode);
            p != nullptr && p->returnsTwice)
            return MirInstFlags::ReturnsTwice;
        return MirInstFlags::None;
    }

    // c27 (D-CSUBSET-VOLATILE-POINTEE): the TYPE-derived half of the access-
    // volatility funnel. A Load/Store whose ACCESSED type (the thing read/written
    // — the pointee for a deref/index, the field for a member, the value for a
    // scalar) is `volatile`-qualified IS a volatile access, regardless of any
    // symbol-level c21 flag. Every access form's Load/Store ORs this with
    // `volatileFlagFor(node)` so the flag's coverage is by-construction at one
    // place per form: c21 carries volatile that lives on a SYMBOL (a `volatile`
    // object/member/pointer-object), c27 carries volatile that lives in the TYPE
    // (a deref/index/member through a `volatile`-pointee `Ptr<VolatileQual(T)>`).
    // A missed OR here = a silent miscompile (the optimizer elides/caches the
    // access), so it is threaded at the SAME sites as `volatileFlagFor`.
    [[nodiscard]] MirInstFlags volatileFlagForType(TypeId accessedTy) const {
        return interner.isVolatileQualified(accessedTy) ? MirInstFlags::Volatile
                                                        : MirInstFlags::None;
    }

    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): the C11 memory_order carried in an
    // AtomicLoad/AtomicStore `payload` (relaxed=0..seq_cst=5). A plain `_Atomic`
    // access is seq_cst — the strongest, default order (C11 6.5.2.4/7.17.3).
    static constexpr std::uint32_t kAtomicOrderSeqCst = 5;

    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): the SINGLE scalar-access chokepoint
    // that decides atomic-vs-plain. Every scalar Load emit routes through here so
    // an `_Atomic`-qualified access lowers to `AtomicLoad` (seq_cst) BY
    // CONSTRUCTION, and a plain access keeps the EXACT c21|c27 volatile-flag
    // expression it always used. `accessedTy` is the value type read (the Load's
    // result type); `node` is the HIR access node (drives the c21 object-volatile
    // flag). A missed funnel site is caught LOUD by the MIR verifier's atomic belt
    // (I_AtomicAccessNotLowered) — never a silent non-atomic access.
    [[nodiscard]] MirInstId emitScalarLoad(std::span<MirInstId const> ptrOps,
                                           TypeId accessedTy, HirNodeId node) {
        if (interner.isAtomicQualified(accessedTy)) {
            // `_Atomic` (incl. `_Atomic volatile`): AtomicLoad ALONE — its
            // hasSideEffects + opcodeClobbersMemory membership subsume volatile's
            // no-elide / no-CSE / no-hoist, so no Volatile flag is threaded.
            return mir.addInst(MirOpcode::AtomicLoad, ptrOps, accessedTy,
                               /*payload=*/kAtomicOrderSeqCst, MirInstFlags::None);
        }
        return mir.addInst(MirOpcode::Load, ptrOps, accessedTy, /*payload=*/0,
                           volatileFlagFor(node) | volatileFlagForType(accessedTy));
    }

    // FC17.9(d) cycle 1b (D-CSUBSET-ATOMIC): the Store twin of `emitScalarLoad`.
    // `storeOps` is the plain-Store operand pair {value, ptr} (AtomicStore reuses
    // that exact order — a drop-in). `accessedTy` is the lvalue's type (the store
    // target's declared/pointee type); `node` is the target HIR node. An `_Atomic`
    // target lowers to `AtomicStore` (seq_cst); a plain target keeps the exact
    // c21|c27 volatile flag. NOTE: object INITIALIZATION stores (C11 7.17.2.1 —
    // init is not itself atomic) do NOT route here; they stay plain `Store` and are
    // marked `AtomicInitExempt` so the verifier belt spares them.
    void emitScalarStore(std::span<MirInstId const> storeOps, TypeId accessedTy,
                         HirNodeId node) {
        if (interner.isAtomicQualified(accessedTy)) {
            mir.addInst(MirOpcode::AtomicStore, storeOps, InvalidType,
                        /*payload=*/kAtomicOrderSeqCst, MirInstFlags::None);
            return;
        }
        mir.addInst(MirOpcode::Store, storeOps, InvalidType, /*payload=*/0,
                    volatileFlagFor(node) | volatileFlagForType(accessedTy));
    }

    // Map a HIR core operator + operand TypeKind to a MIR opcode. Integer
    // signed/unsigned is type-driven (HirOpKind has only `Div`/`Rem`/`Shr`,
    // not separate signed/unsigned forms — same convention as type_lattice).
    // Floating-point uses the F-prefixed opcodes. Returns
    // `MirOpcode::Invalid` for unsupported combinations so the caller can
    // diagnose with the actual HirOpKind name.
    [[nodiscard]] static MirOpcode mapBinaryOp(HirOpKind op, TypeKind tk) noexcept {
        bool const isFloat = (tk == TypeKind::F16 || tk == TypeKind::F32
                           || tk == TypeKind::F64 || tk == TypeKind::F128);
        bool const isSigned = (tk == TypeKind::I8 || tk == TypeKind::I16
                            || tk == TypeKind::I32 || tk == TypeKind::I64
                            || tk == TypeKind::I128);
        switch (op) {
            case HirOpKind::Add: return isFloat ? MirOpcode::FAdd : MirOpcode::Add;
            case HirOpKind::Sub: return isFloat ? MirOpcode::FSub : MirOpcode::Sub;
            case HirOpKind::Mul: return isFloat ? MirOpcode::FMul : MirOpcode::Mul;
            case HirOpKind::Div: return isFloat ? MirOpcode::FDiv
                                       : (isSigned ? MirOpcode::SDiv : MirOpcode::UDiv);
            case HirOpKind::Rem: return isFloat ? MirOpcode::Invalid
                                       : (isSigned ? MirOpcode::SMod : MirOpcode::UMod);
            case HirOpKind::BitAnd: return MirOpcode::And;
            case HirOpKind::BitOr:  return MirOpcode::Or;
            case HirOpKind::BitXor: return MirOpcode::Xor;
            case HirOpKind::Shl:    return MirOpcode::Shl;
            case HirOpKind::Shr:    return isSigned ? MirOpcode::AShr : MirOpcode::LShr;
            // FC3.5 sweep-c2 — the D-COND-FLOAT-NAN-TRUTHINESS-FCMP
            // adjudication (C 6.5.9 + IEEE 754):
            //   * `==` on floats → FCmpOeq (ordered-equal): NaN == x
            //     is FALSE, and Oeq is false on unordered ✓.
            //   * `!=` on floats → FCmpUNE (UNORDERED-or-unequal):
            //     C 6.5.9p3+fn says `!=` is the NEGATION of `==`, so
            //     NaN != x is TRUE — One (ordered-ne, false on NaN)
            //     would miscompile every NaN inequality. This single
            //     mapping ALSO serves the truthiness lowering: a bare
            //     float condition (`if (d)`) builds `Ne(d, 0.0)` at
            //     cst_to_hir's coerceCondition, which routes here →
            //     Une → `if (NaN)` is TRUE (NaN compares unequal to
            //     0.0), the C-correct truthiness.
            // The relationals stay the ORDERED forms (C relational
            // operators are false on unordered operands).
            case HirOpKind::Eq:     return isFloat ? MirOpcode::FCmpOeq : MirOpcode::ICmpEq;
            case HirOpKind::Ne:     return isFloat ? MirOpcode::FCmpUne : MirOpcode::ICmpNe;
            case HirOpKind::Lt:     return isFloat ? MirOpcode::FCmpOlt
                                       : (isSigned ? MirOpcode::ICmpSlt : MirOpcode::ICmpUlt);
            case HirOpKind::Le:     return isFloat ? MirOpcode::FCmpOle
                                       : (isSigned ? MirOpcode::ICmpSle : MirOpcode::ICmpUle);
            case HirOpKind::Gt:     return isFloat ? MirOpcode::FCmpOgt
                                       : (isSigned ? MirOpcode::ICmpSgt : MirOpcode::ICmpUgt);
            case HirOpKind::Ge:     return isFloat ? MirOpcode::FCmpOge
                                       : (isSigned ? MirOpcode::ICmpSge : MirOpcode::ICmpUge);
            case HirOpKind::Neg: case HirOpKind::Not: case HirOpKind::BitNot:
            case HirOpKind::Count_:
                return MirOpcode::Invalid;
        }
        return MirOpcode::Invalid;
    }

    // Map (sourceKind, targetKind) to the right MIR cast opcode. Categories:
    // integer-to-integer (width + sign), integer↔float, float-to-float,
    // integer↔pointer, pointer-to-pointer (Bitcast). Same-kind casts collapse
    // to Bitcast (e.g. signed↔unsigned of the same width — no value change at
    // the bit level). Returns `MirOpcode::Invalid` for unrecognized pairs.
    // D-CSUBSET-INT-TO-F32-CODEGEN / si_to_fp sub-int source (c78): true
    // for an integer type NARROWER than `int` (Char/I8/U8/I16/U16/Bool/
    // Byte) — the kinds that must integer-PROMOTE to I32 before an
    // int→float conversion (CVTSI2SD reads r32/r64; SCVTF reads Wn/Xn —
    // neither has a sub-32 form). I32 and wider are already ≥int and pass
    // through. Mirrors mapCast's own 8/16-bit-width classification.
    [[nodiscard]] static bool isSubIntPromotable(TypeKind k) noexcept {
        switch (k) {
            case TypeKind::Bool: case TypeKind::Byte:
            case TypeKind::Char:
            case TypeKind::I8:   case TypeKind::U8:
            case TypeKind::I16:  case TypeKind::U16:
                return true;
            default:
                return false;
        }
    }

    [[nodiscard]] static MirOpcode mapCast(TypeKind from, TypeKind to) noexcept {
        auto isInt = [](TypeKind k) noexcept {
            return k == TypeKind::I8  || k == TypeKind::I16 || k == TypeKind::I32
                || k == TypeKind::I64 || k == TypeKind::I128
                || k == TypeKind::U8  || k == TypeKind::U16 || k == TypeKind::U32
                || k == TypeKind::U64 || k == TypeKind::U128
                || k == TypeKind::Char || k == TypeKind::Byte || k == TypeKind::Bool;
        };
        auto isSignedInt = [](TypeKind k) noexcept {
            return k == TypeKind::I8  || k == TypeKind::I16 || k == TypeKind::I32
                || k == TypeKind::I64 || k == TypeKind::I128 || k == TypeKind::Char;
        };
        auto isFloat = [](TypeKind k) noexcept {
            return k == TypeKind::F16 || k == TypeKind::F32
                || k == TypeKind::F64 || k == TypeKind::F128;
        };
        auto bitWidth = [](TypeKind k) noexcept -> int {
            switch (k) {
                case TypeKind::Bool: case TypeKind::I8: case TypeKind::U8:
                case TypeKind::Char: case TypeKind::Byte:        return 8;
                case TypeKind::I16:  case TypeKind::U16: case TypeKind::F16: return 16;
                case TypeKind::I32:  case TypeKind::U32: case TypeKind::F32: return 32;
                case TypeKind::I64:  case TypeKind::U64: case TypeKind::F64: return 64;
                case TypeKind::I128: case TypeKind::U128: case TypeKind::F128: return 128;
                default: return 0;
            }
        };
        if (from == to) return MirOpcode::Bitcast;
        if (isInt(from) && isInt(to)) {
            int const fw = bitWidth(from);
            int const tw = bitWidth(to);
            if (fw == 0 || tw == 0) return MirOpcode::Invalid;
            if (tw <  fw) return MirOpcode::Trunc;
            if (tw == fw) return MirOpcode::Bitcast;
            return isSignedInt(from) ? MirOpcode::SExt : MirOpcode::ZExt;
        }
        if (isFloat(from) && isFloat(to)) {
            int const fw = bitWidth(from);
            int const tw = bitWidth(to);
            if (fw == 0 || tw == 0) return MirOpcode::Invalid;
            if (tw <  fw) return MirOpcode::FPTrunc;
            if (tw == fw) return MirOpcode::Bitcast;
            return MirOpcode::FPExt;
        }
        if (isInt(from)   && isFloat(to)) {
            return isSignedInt(from) ? MirOpcode::SIToFP : MirOpcode::UIToFP;
        }
        if (isFloat(from) && isInt(to)) {
            return isSignedInt(to) ? MirOpcode::FPToSI : MirOpcode::FPToUI;
        }
        if (from == TypeKind::Ptr && isInt(to))   return MirOpcode::PtrToInt;
        if (isInt(from)   && to == TypeKind::Ptr) return MirOpcode::IntToPtr;
        if (from == TypeKind::Ptr && to == TypeKind::Ptr) return MirOpcode::Bitcast;
        // c12 (C 6.3.2.1p4) function-to-pointer decay: a function DESIGNATOR
        // (FnSig) reaching a Ptr<FnSig> context. CST→HIR's `coerce` emits this
        // synthetic Cast for a brace-init element (`struct Ops a = { dbl };`); the
        // operand is a function Ref, which already lowers to a `GlobalAddr` (the
        // function's code address), so the conversion is representation-free — a
        // Bitcast that just re-types the pointer-width value as `Ptr<FnSig>`.
        if (from == TypeKind::FnSig && to == TypeKind::Ptr) return MirOpcode::Bitcast;
        return MirOpcode::Invalid;
    }

    // ── C23 _BitInt(N) codegen — the mod-2^N wrap (D-CSUBSET-BITINT) ──────────
    //
    // A `_BitInt(N≤64)` value lives in its native CONTAINER (I8/I16/I32/I64 by
    // size + signedness — `reprKind` at the LIR tier); its arithmetic COMPUTES at a
    // native ALU width B (32 for N≤32, 64 for N≤64) and MASKS the result back to
    // exactly N bits, reusing the bit-field extract/insert shift+mask primitive
    // (signed: Shl(B-N)+AShr(B-N); unsigned: And((1<<N)-1)). A SUB-32 container
    // (N≤16) computes PROMOTED to I32 then Truncs back — the `emitBitfieldExtract`
    // `promote = B0 < 32` precedent — so a `_BitInt(4)` never trips the sub-native
    // ALU gate. N≥17's container IS the compute width, so it computes at the BitInt
    // type directly (reprKind → I32/I64 passes the gate). Comparisons promote their
    // operands and emit an ICmp at the compute width.

    // The plain compute integer kind for a `_BitInt(N)`: I32/U32 (N≤32) or I64/U64
    // (N≤64), by signedness. Where the ALU op + mask are computed for a sub-32
    // container (N≤16); IS the container kind for N≥17.
    [[nodiscard]] TypeKind bitIntComputeKind(TypeId bitIntTy) const {
        std::int64_t const n = interner.bitIntWidth(bitIntTy);
        bool const s = interner.bitIntIsSigned(bitIntTy);
        if (n <= 32) return s ? TypeKind::I32 : TypeKind::U32;
        return s ? TypeKind::I64 : TypeKind::U64;
    }

    // Materialize a `_BitInt(N)` operand at its compute width: a no-op for N≥17
    // (the container IS the compute kind), else SExt/ZExt the sub-32 container up
    // to I32/U32 (`mapCast(container, compute)`).
    [[nodiscard]] MirInstId bitIntToCompute(MirInstId v, TypeId bitIntTy) {
        TypeKind const container = interner.bitIntContainerKind(bitIntTy);
        TypeKind const compute   = bitIntComputeKind(bitIntTy);
        if (container == compute) return v;
        std::array<MirInstId, 1> a{v};
        return mir.addInst(mapCast(container, compute), a,
                           interner.primitive(compute));
    }

    // Mask `raw` (typed `ty`, whose compute width is B bits) to the low N bits: the
    // signed sign-extend (Shl(B-N) then arithmetic AShr(B-N)) or the unsigned
    // zero-extend (And with (1<<N)-1). A no-op when N==B (a _BitInt(32)/_BitInt(64)
    // whose native op already wraps at the full width). Reuses the bit-field
    // primitive's exact shift+mask sequence.
    [[nodiscard]] MirInstId
    bitIntMask(MirInstId raw, std::int64_t n, bool signd, int B, TypeId ty) {
        if (n >= B) return raw;
        if (signd) {
            MirInstId const s1 = constIntOfType(B - n, ty);
            std::array<MirInstId, 2> a1{raw, s1};
            MirInstId const shl = mir.addInst(MirOpcode::Shl, a1, ty);
            MirInstId const s2 = constIntOfType(B - n, ty);
            std::array<MirInstId, 2> a2{shl, s2};
            return mir.addInst(MirOpcode::AShr, a2, ty);
        }
        std::uint64_t const mask = (n >= 64) ? ~0ull : ((1ull << n) - 1);
        MirInstId const m = constIntOfType(static_cast<std::int64_t>(mask), ty);
        std::array<MirInstId, 2> a{raw, m};
        return mir.addInst(MirOpcode::And, a, ty);
    }

    // ★ CRIT-2 — the ONE by-construction WRAP CHOKEPOINT. Emit `op` over `operands`
    // producing a `_BitInt(N)`-typed value, masked to N iff `needsMask`. Every
    // producer of a BitInt value (Add/Sub/Mul/Neg/Shl + the compound-assign/++/--
    // that desugar to them; the no-mask Div/Mod/And/Or/Xor/right-shift pass
    // `needsMask=false`) routes through here. N≥17 computes at the BitInt type
    // directly; N≤16 promotes to I32, computes, masks, and Truncs to the container.
    [[nodiscard]] MirInstId
    emitBitIntOp(MirOpcode op, std::span<MirInstId const> operands, TypeId bitIntTy,
                 bool needsMask) {
        std::int64_t const n = interner.bitIntWidth(bitIntTy);
        bool const signd     = interner.bitIntIsSigned(bitIntTy);
        bool const sub32     = n <= 16;   // container < 32 bits → promote to I32
        int const B          = (n <= 32) ? 32 : 64;
        if (!sub32) {
            // Compute at the BitInt type (reprKind → I32/I64 → passes the ALU gate).
            MirInstId const raw = mir.addInst(op, operands, bitIntTy);
            if (!raw.valid()) return InvalidMirInst;
            return needsMask ? bitIntMask(raw, n, signd, B, bitIntTy) : raw;
        }
        // Sub-32 container: promote operands to I32/U32, compute, mask, Trunc back.
        TypeKind const compute   = bitIntComputeKind(bitIntTy);
        TypeId   const computeTy = interner.primitive(compute);
        TypeKind const container = interner.bitIntContainerKind(bitIntTy);
        // A SHIFT's count (operand[1]) is NOT a `_BitInt` — it keeps its own integer
        // type (C 6.5.7: a shift's operands are NOT converted to a common type). Only
        // the shifted VALUE (operand[0]) promotes; the count passes through so a
        // sub-32 `_BitInt(4) << n` never mis-widens `n` through the container decode.
        bool const isShift = op == MirOpcode::Shl || op == MirOpcode::LShr
                          || op == MirOpcode::AShr;
        std::array<MirInstId, 2> promoted{};
        for (std::size_t i = 0; i < operands.size() && i < 2; ++i)
            promoted[i] = (isShift && i == 1)
                              ? operands[i]
                              : bitIntToCompute(operands[i], bitIntTy);
        MirInstId raw = mir.addInst(
            op, std::span<MirInstId const>{promoted.data(), operands.size()},
            computeTy);
        if (!raw.valid()) return InvalidMirInst;
        if (needsMask) raw = bitIntMask(raw, n, signd, B, computeTy);
        std::array<MirInstId, 1> ta{raw};
        return mir.addInst(mapCast(compute, container), ta, bitIntTy);   // Trunc → N
    }

    // ★ CRIT-1 — a conversion TO `_BitInt(N)` masks UNCONDITIONALLY (incl. the
    // same-container case: `int→_BitInt(17)` shares the i32 container, so a plain
    // Bitcast would leave bits 17-31 DIRTY). `srcK` is the source's already-resolved
    // plain integer kind (enum→underlying / BitInt→container / else its kind). Emits
    // the container conversion (Trunc/SExt/ZExt/Bitcast) THEN the mask-to-N, then
    // (sub-32) the Trunc to the container.
    [[nodiscard]] MirInstId
    emitCastToBitInt(MirInstId src, TypeKind srcK, TypeId bitIntTy) {
        std::int64_t const n = interner.bitIntWidth(bitIntTy);
        bool const signd     = interner.bitIntIsSigned(bitIntTy);
        bool const sub32     = n <= 16;
        int const B          = (n <= 32) ? 32 : 64;
        TypeKind const compute   = bitIntComputeKind(bitIntTy);
        TypeId   const computeTy = interner.primitive(compute);
        TypeKind const container = interner.bitIntContainerKind(bitIntTy);
        // 1. Convert the source to the compute kind (no-op when it already IS it).
        MirInstId inCompute = src;
        if (srcK != compute) {
            MirOpcode const conv = mapCast(srcK, compute);
            if (conv == MirOpcode::Invalid) return InvalidMirInst;
            std::array<MirInstId, 1> ca{src};
            inCompute = mir.addInst(conv, ca, computeTy);
            if (!inCompute.valid()) return InvalidMirInst;
        }
        // 2. Mask to N (UNCONDITIONAL — the CRIT-1 dirty-bits fix), at compute width.
        MirInstId const masked = bitIntMask(inCompute, n, signd, B, computeTy);
        // 3. Re-type to the BitInt container: Trunc (sub-32) or a same-width Bitcast.
        std::array<MirInstId, 1> ta{masked};
        return mir.addInst(sub32 ? mapCast(compute, container) : MirOpcode::Bitcast,
                           ta, bitIntTy);
    }

    // ══ C23 _BitInt(N>64) — the multi-limb (C2) codegen (D-CSUBSET-BITINT-C2-WIDE) ══
    //
    // A wide `_BitInt(N>64)` value is MEMORY-RESIDENT: ceil(N/64) little-endian 64-bit
    // limbs (limb 0 = least significant) in an alloca — EXACTLY the by-value aggregate
    // model. It has NO SSA register value; every consumer reaches it BY ADDRESS. Each
    // wide OP works on limb ADDRESSES, computes limb-by-limb via a bounded RUNTIME loop
    // over the (compile-time-known) limb count — ONE code shape for N = 65 … 8388608
    // (the O(1)-code scalability mandate) — and a value-producing op ends by masking
    // the TOP limb (`maskTopLimb`) to re-establish the clean-N invariant (the wide
    // analog of C1's by-construction wrap chokepoint). The counter / carry / accumulator
    // are small i64 alloca slots (the C-`for`-loop precedent; mem2reg promotes them in
    // the release arm, the debug arm runs the memory loop correctly). NO source /
    // target / format identity anywhere — the limb work is generic MIR (Add/Sub/And/…/
    // Gep + Br/CondBr over the closed verb set), agnostic by construction.

    // Signed native integer kind (the `mapCast` local `isSignedInt`, hoisted for the
    // wide-op reuse). Char is signed on both shipped targets' data models.
    [[nodiscard]] static bool isSignedIntKind(TypeKind k) noexcept {
        return k == TypeKind::I8  || k == TypeKind::I16 || k == TypeKind::I32
            || k == TypeKind::I64 || k == TypeKind::I128 || k == TypeKind::Char;
    }

    // A C floating type (the F-prefixed kinds). Used to fail loud on a float<->WIDE
    // `_BitInt(N>64)` conversion (D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV): the multi-limb
    // FP<->limbs path is a later cycle; the naive scalar path would drop the upper limbs.
    [[nodiscard]] static bool isFloatingKind(TypeKind k) noexcept {
        return k == TypeKind::F16 || k == TypeKind::F32
            || k == TypeKind::F64 || k == TypeKind::F128;
    }

    // ceil(N/64) — the limb count of a wide `_BitInt(N)`.
    [[nodiscard]] std::int64_t wideLimbCount(TypeId bitIntTy) const {
        return (interner.bitIntWidth(bitIntTy) + 63) / 64;
    }
    [[nodiscard]] TypeId i64Ty()  { return interner.primitive(TypeKind::I64); }
    [[nodiscard]] TypeId ptrI64() { return interner.pointer(i64Ty()); }
    [[nodiscard]] MirInstId ci64(std::int64_t v) { return constIntOfType(v, i64Ty()); }
    // A binary/unary i64 op (the workhorse limb emitter).
    [[nodiscard]] MirInstId i64bin(MirOpcode op, MirInstId a, MirInstId b) {
        std::array<MirInstId, 2> ops{a, b};
        return mir.addInst(op, ops, i64Ty());
    }
    [[nodiscard]] MirInstId i64un(MirOpcode op, MirInstId a) {
        std::array<MirInstId, 1> ops{a};
        return mir.addInst(op, ops, i64Ty());
    }
    // &slot[limbIdx] (Ptr<I64>) for a COMPILE-TIME limb index.
    [[nodiscard]] MirInstId limbAddrConst(MirInstId slot, std::int64_t limbIdx) {
        std::array<MirInstId, 2> g{slot, ci64(limbIdx * 8)};
        return mir.addInst(MirOpcode::Gep, g, ptrI64());
    }
    // &slot[idxVal] (Ptr<I64>) for a RUNTIME limb index (a loop counter value).
    [[nodiscard]] MirInstId limbAddrRuntime(MirInstId slot, MirInstId idxVal) {
        MirInstId const off = i64bin(MirOpcode::Mul, idxVal, ci64(8));
        std::array<MirInstId, 2> g{slot, off};
        return mir.addInst(MirOpcode::Gep, g, ptrI64());
    }
    [[nodiscard]] MirInstId loadLimb(MirInstId addr) {
        std::array<MirInstId, 1> a{addr};
        return mir.addInst(MirOpcode::Load, a, i64Ty());
    }
    void storeLimb(MirInstId value, MirInstId addr) {
        std::array<MirInstId, 2> a{value, addr};
        mir.addInst(MirOpcode::Store, a, InvalidType);
    }
    [[nodiscard]] MirInstId zextBoolToI64(MirInstId b) {
        std::array<MirInstId, 1> a{b};
        return mir.addInst(MirOpcode::ZExt, a, i64Ty());
    }
    // A limb comparison — result is Bool (NOT i64: `i64bin` would mis-type it and the
    // ZExt would see an i64 source). `zextCmp` widens the Bool result to an i64 0/1.
    [[nodiscard]] MirInstId icmp(MirOpcode op, MirInstId a, MirInstId b) {
        std::array<MirInstId, 2> ops{a, b};
        return mir.addInst(op, ops, interner.primitive(TypeKind::Bool));
    }
    [[nodiscard]] MirInstId zextCmp(MirOpcode op, MirInstId a, MirInstId b) {
        return zextBoolToI64(icmp(op, a, b));
    }

    // The TOP-limb mask — the wide analog of C1's `bitIntMask`, applied to ONE limb.
    // The highest limb keeps `hb = ((N-1)%64)+1` significant bits; the rest are 0
    // (unsigned) or a sign-extension of bit hb-1 (signed). L1: at hb==64 (N a multiple
    // of 64) the top limb is ALREADY full-width — a NO-OP (unsigned `(1<<64)-1` is UB;
    // signed shift-by-0 is identity). Idempotent on an already-clean limb, so a
    // producer may always end here. `slot` is the result's Ptr<BitInt(N)>.
    [[nodiscard]] bool maskTopLimb(MirInstId slot, TypeId bitIntTy) {
        std::int64_t const n  = interner.bitIntWidth(bitIntTy);
        bool const signd      = interner.bitIntIsSigned(bitIntTy);
        std::int64_t const hb = ((n - 1) % 64) + 1;   // significant bits in the top limb
        if (hb == 64) return true;                      // L1: full limb — nothing to clear
        MirInstId const topAddr = limbAddrConst(slot, wideLimbCount(bitIntTy) - 1);
        MirInstId const top     = loadLimb(topAddr);
        MirInstId masked;
        if (signd) {
            // Sign-extend bit hb-1 across [hb,64): Shl(64-hb) then arithmetic AShr(64-hb).
            MirInstId const sh = ci64(64 - hb);
            masked = i64bin(MirOpcode::AShr, i64bin(MirOpcode::Shl, top, sh), sh);
        } else {
            masked = i64bin(MirOpcode::And, top,
                            ci64(static_cast<std::int64_t>((1ull << hb) - 1)));
        }
        if (!masked.valid()) return false;
        storeLimb(masked, topAddr);
        return true;
    }

    // Emit a bounded runtime loop `for (i64 i=0; i<count; ++i) body(i)` in the CURRENT
    // block, leaving the loop-EXIT block open. `body(idxVal)` emits the per-iteration
    // work (false ⇒ fail-loud already reported). ONE code shape for any count → O(1)
    // MIR at any width. The counter is a small i64 slot (the C-`for` precedent).
    template <class BodyFn>
    [[nodiscard]] bool emitLimbLoop(std::int64_t count, BodyFn&& body) {
        if (count <= 0) return true;   // no limbs — nothing to emit (defensive)
        MirInstId const ctr = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!ctr.valid()) return false;
        storeLimb(ci64(0), ctr);
        MirBlockId const header = mir.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const bodyBB = mir.createBlock(StructCfMarker::Linear);
        MirBlockId const exitBB = mir.createBlock(StructCfMarker::LoopExit);
        mir.addBr(header);
        mir.beginBlock(header);
        std::array<MirInstId, 2> c{loadLimb(ctr), ci64(count)};
        MirInstId const cond =
            mir.addInst(MirOpcode::ICmpSlt, c, interner.primitive(TypeKind::Bool));
        if (!cond.valid()) {
            sealCreatedAsUnreachable(bodyBB);
            sealCreatedAsUnreachable(exitBB);
            return false;
        }
        mir.addCondBr(cond, bodyBB, exitBB);
        mir.beginBlock(bodyBB);
        MirInstId const iv = loadLimb(ctr);
        if (!body(iv)) {
            if (!mir.openBlockHasTerminator()) mir.addUnreachable();
            sealCreatedAsUnreachable(exitBB);
            return false;
        }
        storeLimb(i64bin(MirOpcode::Add, iv, ci64(1)), ctr);
        mir.addBr(header);
        mir.beginBlock(exitBB);
        return true;
    }

    // The RUNTIME-count sibling of `emitLimbLoop`: `for (i64 i=0; i<countVal; ++i)
    // body(i)` where `countVal` is an i64 SSA VALUE (not a compile-time constant). Same
    // LoopHeader/Linear/LoopExit shape; the guard is `ICmpSlt(ctr, countVal)` so a
    // count<=0 runs the body zero times (no compile-time special-case needed). Used by
    // the wide-multiply inner loop whose bound `limbCount - i` is only known at run time.
    // `countVal` must dominate the header (it does: the caller computes it in the
    // enclosing block, and it is loop-invariant across the back-edge).
    template <class BodyFn>
    [[nodiscard]] bool emitLimbLoopN(MirInstId countVal, BodyFn&& body) {
        if (!countVal.valid()) return false;
        MirInstId const ctr = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!ctr.valid()) return false;
        storeLimb(ci64(0), ctr);
        MirBlockId const header = mir.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const bodyBB = mir.createBlock(StructCfMarker::Linear);
        MirBlockId const exitBB = mir.createBlock(StructCfMarker::LoopExit);
        mir.addBr(header);
        mir.beginBlock(header);
        std::array<MirInstId, 2> c{loadLimb(ctr), countVal};
        MirInstId const cond =
            mir.addInst(MirOpcode::ICmpSlt, c, interner.primitive(TypeKind::Bool));
        if (!cond.valid()) {
            sealCreatedAsUnreachable(bodyBB);
            sealCreatedAsUnreachable(exitBB);
            return false;
        }
        mir.addCondBr(cond, bodyBB, exitBB);
        mir.beginBlock(bodyBB);
        MirInstId const iv = loadLimb(ctr);
        if (!body(iv)) {
            if (!mir.openBlockHasTerminator()) mir.addUnreachable();
            sealCreatedAsUnreachable(exitBB);
            return false;
        }
        storeLimb(i64bin(MirOpcode::Add, iv, ci64(1)), ctr);
        mir.addBr(header);
        mir.beginBlock(exitBB);
        return true;
    }

    // dst[i] = a[i] <op> b[i], limb-parallel (& | ^). NO mask — a bitwise op of two
    // clean-N operands yields a clean-N result (the high bits are a uniform function
    // of the two sign/zero fills, i.e. the result's own fill).
    [[nodiscard]] bool emitWideBitwise(MirOpcode op, MirInstId dst, MirInstId aAddr,
                                       MirInstId bAddr, TypeId bitIntTy) {
        return emitLimbLoop(wideLimbCount(bitIntTy), [&](MirInstId i) {
            MirInstId const av = loadLimb(limbAddrRuntime(aAddr, i));
            MirInstId const bv = loadLimb(limbAddrRuntime(bAddr, i));
            MirInstId const r  = i64bin(op, av, bv);
            if (!r.valid()) return false;
            storeLimb(r, limbAddrRuntime(dst, i));
            return true;
        });
    }

    // dst[i] = ~a[i], then mask the top limb (unsigned: ~ dirties bits ≥N → must clear;
    // signed: the ~sign fill IS the new sign — the mask is a no-op belt).
    [[nodiscard]] bool emitWideNot(MirInstId dst, MirInstId aAddr, TypeId bitIntTy) {
        bool const ok = emitLimbLoop(wideLimbCount(bitIntTy), [&](MirInstId i) {
            MirInstId const r = i64un(MirOpcode::Not, loadLimb(limbAddrRuntime(aAddr, i)));
            if (!r.valid()) return false;
            storeLimb(r, limbAddrRuntime(dst, i));
            return true;
        });
        return ok && maskTopLimb(dst, bitIntTy);
    }

    // dst = a ± b, a carry (add) / borrow (sub) chain — MIR has no ADC/SBB, so the
    // carry is `(sum < operand)` (generic). Per limb: `s=a±b; c1=carry(s,a); s±=cin;
    // c2=carry(s,cin); dst=s; cin=c1|c2`. The carry rides an i64 slot across iterations.
    // Top-limb mask after (a ± can overflow bit N).
    [[nodiscard]] bool emitWideAddSub(bool isSub, MirInstId dst, MirInstId aAddr,
                                      MirInstId bAddr, TypeId bitIntTy) {
        MirInstId const carry = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!carry.valid()) return false;
        storeLimb(ci64(0), carry);
        bool const ok = emitLimbLoop(wideLimbCount(bitIntTy), [&](MirInstId i) {
            MirInstId const av  = loadLimb(limbAddrRuntime(aAddr, i));
            MirInstId const bv  = loadLimb(limbAddrRuntime(bAddr, i));
            MirInstId const cin = loadLimb(carry);
            MirInstId s, c1, c2;
            if (isSub) {
                s  = i64bin(MirOpcode::Sub, av, bv);
                c1 = zextCmp(MirOpcode::ICmpUlt, av, bv);   // a<b ⇒ borrow
                MirInstId const s2 = i64bin(MirOpcode::Sub, s, cin);
                c2 = zextCmp(MirOpcode::ICmpUlt, s, cin);   // s<cin ⇒ borrow
                s  = s2;
            } else {
                s  = i64bin(MirOpcode::Add, av, bv);
                c1 = zextCmp(MirOpcode::ICmpUlt, s, av);    // sum<a ⇒ carry
                MirInstId const s2 = i64bin(MirOpcode::Add, s, cin);
                c2 = zextCmp(MirOpcode::ICmpUlt, s2, cin);  // s2<cin ⇒ carry
                s  = s2;
            }
            if (!s.valid() || !c1.valid() || !c2.valid()) return false;
            storeLimb(s, limbAddrRuntime(dst, i));
            storeLimb(i64bin(MirOpcode::Or, c1, c2), carry);
            return true;
        });
        return ok && maskTopLimb(dst, bitIntTy);
    }

    // dst = -a, computed as `0 - a` with a borrow chain (minuend limbs are constant 0).
    // Per limb: `d = 0 - a[i] = Neg(a[i]); b1 = (a[i]!=0); d2 = d - borrow;
    // b2 = (d<borrow); dst=d2; borrow = b1|b2`. Top-limb mask after.
    [[nodiscard]] bool emitWideNeg(MirInstId dst, MirInstId aAddr, TypeId bitIntTy) {
        MirInstId const borrow = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!borrow.valid()) return false;
        storeLimb(ci64(0), borrow);
        bool const ok = emitLimbLoop(wideLimbCount(bitIntTy), [&](MirInstId i) {
            MirInstId const av  = loadLimb(limbAddrRuntime(aAddr, i));
            MirInstId const bin = loadLimb(borrow);
            MirInstId const d   = i64un(MirOpcode::Neg, av);                 // 0 - a[i]
            MirInstId const b1  = zextCmp(MirOpcode::ICmpNe, av, ci64(0));
            MirInstId const d2  = i64bin(MirOpcode::Sub, d, bin);
            MirInstId const b2  = zextCmp(MirOpcode::ICmpUlt, d, bin);
            if (!d2.valid() || !b1.valid() || !b2.valid()) return false;
            storeLimb(d2, limbAddrRuntime(dst, i));
            storeLimb(i64bin(MirOpcode::Or, b1, b2), borrow);
            return true;
        });
        return ok && maskTopLimb(dst, bitIntTy);
    }

    // Load src[idxVal] (i64) when idxVal ∈ [0,limbCount); else `fillVal`. Branchless:
    // clamp the index for the LOAD (so no OOB memory access) then select via a 0/~0
    // mask. Used by the shift's cross-limb reads (a source limb below 0 / at-or-above
    // the top is out of range).
    [[nodiscard]] MirInstId loadLimbOrFill(MirInstId src, MirInstId idxVal,
                                           std::int64_t limbCount, MirInstId fillVal) {
        TypeId const boolTy = interner.primitive(TypeKind::Bool);
        std::array<MirInstId, 2> ge{idxVal, ci64(0)};
        MirInstId const geB = mir.addInst(MirOpcode::ICmpSge, ge, boolTy);
        std::array<MirInstId, 2> lt{idxVal, ci64(limbCount)};
        MirInstId const ltB = mir.addInst(MirOpcode::ICmpSlt, lt, boolTy);
        MirInstId const inRange =
            i64bin(MirOpcode::And, zextBoolToI64(geB), zextBoolToI64(ltB));   // 0/1
        MirInstId const clamped = i64bin(MirOpcode::Mul, idxVal, inRange);    // in-range idx
        MirInstId const loaded  = loadLimb(limbAddrRuntime(src, clamped));
        MirInstId const mask    = i64un(MirOpcode::Neg, inRange);             // 0 or ~0
        MirInstId const keep    = i64bin(MirOpcode::And, loaded, mask);
        MirInstId const fill    = i64bin(MirOpcode::And, fillVal,
                                         i64un(MirOpcode::Not, mask));
        return i64bin(MirOpcode::Or, keep, fill);
    }

    // Wide shift `a << k` / `a >> k` (k a RUNTIME count value, already widened to i64).
    // wordShift=k/64, bitShift=k%64. Each dst limb combines two source limbs; the
    // `x >> (64-bitShift)` / `x << (64-bitShift)` sub-shift is `(x >> (63-bitShift))>>1`
    // / `(x << (63-bitShift))<<1` so bitShift==0 is well-defined (no UB `<<64`). `>>`
    // is arithmetic (sign fill) for a signed type, logical for unsigned. A left shift
    // masks the top limb after (it can push bits past N). `isArith` = signed `>>`.
    [[nodiscard]] bool emitWideShift(bool isLeft, bool isArith, MirInstId dst,
                                     MirInstId aAddr, MirInstId kI64, TypeId bitIntTy) {
        std::int64_t const limbCount = wideLimbCount(bitIntTy);
        MirInstId const wordShift = i64bin(MirOpcode::LShr, kI64, ci64(6));   // k / 64
        MirInstId const bitShift  = i64bin(MirOpcode::And,  kI64, ci64(63));  // k % 64
        MirInstId const sh63      = i64bin(MirOpcode::Sub,  ci64(63), bitShift);
        // Right-shift fill for a source limb at/above the top: sign fill (arith) or 0.
        MirInstId fill = ci64(0);
        if (isArith)
            fill = i64bin(MirOpcode::AShr,
                          loadLimb(limbAddrConst(aAddr, limbCount - 1)), ci64(63));
        return emitLimbLoop(limbCount, [&](MirInstId j) {
            MirInstId lo, hi;
            if (isLeft) {
                // dst[j] = (a[j-word] << bit) | (a[j-word-1] >> (64-bit))
                MirInstId const hiIdx = i64bin(MirOpcode::Sub, j, wordShift);
                MirInstId const loIdx = i64bin(MirOpcode::Sub, hiIdx, ci64(1));
                MirInstId const hs = loadLimbOrFill(aAddr, hiIdx, limbCount, ci64(0));
                MirInstId const ls = loadLimbOrFill(aAddr, loIdx, limbCount, ci64(0));
                lo = i64bin(MirOpcode::Shl, hs, bitShift);
                hi = i64bin(MirOpcode::LShr, i64bin(MirOpcode::LShr, ls, sh63), ci64(1));
            } else {
                // dst[j] = (a[j+word] >> bit) | (a[j+word+1] << (64-bit))
                MirInstId const loIdx = i64bin(MirOpcode::Add, j, wordShift);
                MirInstId const hiIdx = i64bin(MirOpcode::Add, loIdx, ci64(1));
                MirInstId const ls = loadLimbOrFill(aAddr, loIdx, limbCount, fill);
                MirInstId const hs = loadLimbOrFill(aAddr, hiIdx, limbCount, fill);
                lo = i64bin(MirOpcode::LShr, ls, bitShift);
                hi = i64bin(MirOpcode::Shl, i64bin(MirOpcode::Shl, hs, sh63), ci64(1));
            }
            MirInstId const r = i64bin(MirOpcode::Or, lo, hi);
            if (!r.valid()) return false;
            storeLimb(r, limbAddrRuntime(dst, j));
            return true;
        }) && (!isLeft || maskTopLimb(dst, bitIntTy));
    }

    // int/enum/narrow-container scalar → wide `_BitInt`. Write the (sign/zero-extended)
    // source into limb 0, fill the higher limbs with the source's sign fill (signed
    // negative → ~0, else 0), mask the top limb. `srcVal` is a scalar MirInstId; its
    // SIGNEDNESS drives the extension (a signed source sign-extends into the wide value).
    [[nodiscard]] bool emitWideFromScalar(MirInstId dst, MirInstId srcVal,
                                          TypeKind srcKind, TypeId bitIntTy) {
        bool const srcSigned = isSignedIntKind(srcKind);
        // Widen the source to i64 (SExt signed / ZExt unsigned; a no-op if already 64).
        MirInstId src64 = srcVal;
        if (srcKind != TypeKind::I64 && srcKind != TypeKind::U64) {
            MirOpcode const conv = mapCast(srcKind, srcSigned ? TypeKind::I64 : TypeKind::U64);
            if (conv == MirOpcode::Invalid) return false;
            std::array<MirInstId, 1> a{srcVal};
            src64 = mir.addInst(conv, a, i64Ty());
            if (!src64.valid()) return false;
        }
        storeLimb(src64, limbAddrConst(dst, 0));
        // Higher limbs: sign fill (arith shift of the source) for signed, else 0.
        MirInstId const fill =
            srcSigned ? i64bin(MirOpcode::AShr, src64, ci64(63)) : ci64(0);
        std::int64_t const limbCount = wideLimbCount(bitIntTy);
        for (std::int64_t li = 1; li < limbCount; ++li)
            storeLimb(fill, limbAddrConst(dst, li));
        return maskTopLimb(dst, bitIntTy);
    }

    // wide `_BitInt` → wide `_BitInt` (different N). Copy min(src,dst) limbs, fill the
    // rest with the SOURCE's sign fill (signed → sign of its top limb, else 0), mask
    // the DEST top limb. Handles both widening and narrowing.
    [[nodiscard]] bool emitWideFromWide(MirInstId dst, MirInstId srcAddr,
                                        TypeId srcTy, TypeId dstTy) {
        std::int64_t const srcLimbs = wideLimbCount(srcTy);
        std::int64_t const dstLimbs = wideLimbCount(dstTy);
        std::int64_t const common   = std::min(srcLimbs, dstLimbs);
        MirInstId const fill = interner.bitIntIsSigned(srcTy)
            ? i64bin(MirOpcode::AShr, loadLimb(limbAddrConst(srcAddr, srcLimbs - 1)), ci64(63))
            : ci64(0);
        for (std::int64_t li = 0; li < common; ++li)
            storeLimb(loadLimb(limbAddrConst(srcAddr, li)), limbAddrConst(dst, li));
        for (std::int64_t li = common; li < dstLimbs; ++li)
            storeLimb(fill, limbAddrConst(dst, li));
        return maskTopLimb(dst, dstTy);
    }

    // wide `_BitInt` → a scalar VALUE: the low bits truncated to `targetKind` (C
    // 6.3.1.3). Read limb 0 and convert (Trunc/Bitcast). Returns the scalar MirInstId.
    [[nodiscard]] MirInstId emitScalarFromWide(MirInstId srcAddr, TypeKind targetKind) {
        MirInstId const limb0 = loadLimb(limbAddrConst(srcAddr, 0));   // I64
        if (targetKind == TypeKind::I64) return limb0;
        // Every other target (incl. U64 → a same-width Bitcast so the RESULT value's
        // type matches the cast's declared kind) routes through the container mapCast.
        MirOpcode const conv = mapCast(TypeKind::I64, targetKind);
        if (conv == MirOpcode::Invalid) return InvalidMirInst;
        std::array<MirInstId, 1> a{limb0};
        return mir.addInst(conv, a, interner.primitive(targetKind));
    }

    // wide `_BitInt` → Bool: `(OR of all limbs) != 0` (a nonzero-test, e.g. an if-cond
    // or a `(_Bool)` cast) or `== 0` (a logical `!`). Returns the Bool MirInstId.
    [[nodiscard]] MirInstId emitBoolFromWide(MirInstId srcAddr, TypeId bitIntTy,
                                             bool zeroIsTrue) {
        MirInstId const acc = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!acc.valid()) return InvalidMirInst;
        storeLimb(ci64(0), acc);
        if (!emitLimbLoop(wideLimbCount(bitIntTy), [&](MirInstId i) {
                MirInstId const v = loadLimb(limbAddrRuntime(srcAddr, i));
                storeLimb(i64bin(MirOpcode::Or, loadLimb(acc), v), acc);
                return true;
            }))
            return InvalidMirInst;
        std::array<MirInstId, 2> c{loadLimb(acc), ci64(0)};
        return mir.addInst(zeroIsTrue ? MirOpcode::ICmpEq : MirOpcode::ICmpNe, c,
                           interner.primitive(TypeKind::Bool));
    }

    // wide == / != : `(OR of per-limb XORs) == 0` (equal) / `!= 0` (differ). No order.
    [[nodiscard]] MirInstId emitWideEq(bool isNe, MirInstId aAddr, MirInstId bAddr,
                                       TypeId bitIntTy) {
        MirInstId const acc = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!acc.valid()) return InvalidMirInst;
        storeLimb(ci64(0), acc);
        if (!emitLimbLoop(wideLimbCount(bitIntTy), [&](MirInstId i) {
                MirInstId const av = loadLimb(limbAddrRuntime(aAddr, i));
                MirInstId const bv = loadLimb(limbAddrRuntime(bAddr, i));
                MirInstId const x  = i64bin(MirOpcode::Xor, av, bv);
                storeLimb(i64bin(MirOpcode::Or, loadLimb(acc), x), acc);
                return true;
            }))
            return InvalidMirInst;
        std::array<MirInstId, 2> c{loadLimb(acc), ci64(0)};
        return mir.addInst(isNe ? MirOpcode::ICmpNe : MirOpcode::ICmpEq, c,
                           interner.primitive(TypeKind::Bool));
    }

    // wide ordered compare `< <= > >=`. MS-limb decides: scan the LOWER limbs LS→MS
    // (a more-significant differing limb OVERWRITES a less-significant one) UNSIGNED,
    // then the TOP limb (SIGNED for a signed type, unsigned otherwise) overwrites all.
    // Accumulate `lt`/`gt` (i64 0/1) in slots; the operator result is lt / gt / !gt /
    // !lt. Branchless select `x = differ ? cur : prev` = `cur | (keep & prev)`,
    // keep = !differ (cur is 0 when equal, so an OR-combine is exact).
    [[nodiscard]] MirInstId emitWideOrder(HirOpKind op, MirInstId aAddr, MirInstId bAddr,
                                          TypeId bitIntTy) {
        bool const signd = interner.bitIntIsSigned(bitIntTy);
        std::int64_t const limbCount = wideLimbCount(bitIntTy);
        MirInstId const ltS = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        MirInstId const gtS = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!ltS.valid() || !gtS.valid()) return InvalidMirInst;
        storeLimb(ci64(0), ltS);
        storeLimb(ci64(0), gtS);
        // The lower limbs (0 .. limbCount-2), UNSIGNED, LS→MS (later = more significant
        // → its result overwrites). Skipped when limbCount==1 (no lower limbs).
        if (!emitLimbLoop(limbCount - 1, [&](MirInstId i) {
                MirInstId const av  = loadLimb(limbAddrRuntime(aAddr, i));
                MirInstId const bv  = loadLimb(limbAddrRuntime(bAddr, i));
                MirInstId const lti = zextCmp(MirOpcode::ICmpUlt, av, bv);
                MirInstId const gti = zextCmp(MirOpcode::ICmpUgt, av, bv);
                MirInstId const keep =
                    i64un(MirOpcode::Not, i64bin(MirOpcode::Or, lti, gti));   // ~differ
                storeLimb(i64bin(MirOpcode::Or, lti,
                                 i64bin(MirOpcode::And, keep, loadLimb(ltS))), ltS);
                storeLimb(i64bin(MirOpcode::Or, gti,
                                 i64bin(MirOpcode::And, keep, loadLimb(gtS))), gtS);
                return true;
            }))
            return InvalidMirInst;
        // The TOP limb (signed compare for a signed type) overwrites all lower limbs.
        MirInstId const at = loadLimb(limbAddrConst(aAddr, limbCount - 1));
        MirInstId const bt = loadLimb(limbAddrConst(bAddr, limbCount - 1));
        MirInstId const ltT =
            zextCmp(signd ? MirOpcode::ICmpSlt : MirOpcode::ICmpUlt, at, bt);
        MirInstId const gtT =
            zextCmp(signd ? MirOpcode::ICmpSgt : MirOpcode::ICmpUgt, at, bt);
        MirInstId const keepT =
            i64un(MirOpcode::Not, i64bin(MirOpcode::Or, ltT, gtT));
        MirInstId const lt = i64bin(MirOpcode::Or, ltT,
                                    i64bin(MirOpcode::And, keepT, loadLimb(ltS)));
        MirInstId const gt = i64bin(MirOpcode::Or, gtT,
                                    i64bin(MirOpcode::And, keepT, loadLimb(gtS)));
        // Operator result (Bool): < → lt!=0 ; > → gt!=0 ; <= → gt==0 ; >= → lt==0.
        TypeId const boolTy = interner.primitive(TypeKind::Bool);
        MirInstId sel; MirOpcode cmp;
        switch (op) {
            case HirOpKind::Lt: sel = lt; cmp = MirOpcode::ICmpNe; break;
            case HirOpKind::Gt: sel = gt; cmp = MirOpcode::ICmpNe; break;
            case HirOpKind::Le: sel = gt; cmp = MirOpcode::ICmpEq; break;
            case HirOpKind::Ge: sel = lt; cmp = MirOpcode::ICmpEq; break;
            default: return InvalidMirInst;
        }
        std::array<MirInstId, 2> c{sel, ci64(0)};
        return mir.addInst(cmp, c, boolTy);
    }

    // ══ C23 _BitInt(N>64) — the multi-limb HARD arithmetic (C3, D-CSUBSET-BITINT-C3-
    // MULDIV): `*` schoolbook via UMulH, `/`·`%` binary long division. Builds ENTIRELY
    // on the C2 substrate + the small limb helpers below. Sign handling: multiply's low
    // N bits are two's-complement sign-AGNOSTIC (one path, signedness enters only at the
    // final maskTopLimb); divide operates on magnitudes with a C99 trunc-toward-zero sign
    // fixup. Fully agnostic generic MIR — no source/target/format identity. ══

    // dst[i] = 0 for all limbs (a RUNTIME loop — O(1) code at any width; freshAggregate-
    // Temp allocas are NOT zero-initialized).
    [[nodiscard]] bool zeroWide(MirInstId dst, std::int64_t limbCount) {
        return emitLimbLoop(limbCount, [&](MirInstId i) {
            storeLimb(ci64(0), limbAddrRuntime(dst, i));
            return true;
        });
    }
    // dst[i] = src[i] for all limbs (limb-wise copy; src/dst distinct).
    [[nodiscard]] bool copyWide(MirInstId dst, MirInstId src, std::int64_t limbCount) {
        return emitLimbLoop(limbCount, [&](MirInstId i) {
            storeLimb(loadLimb(limbAddrRuntime(src, i)), limbAddrRuntime(dst, i));
            return true;
        });
    }
    // Bit N-1 (the sign bit) of the wide value at `addr`, as an i64 0/1. N-1 is a
    // COMPILE-TIME position → a const limb index + shift.
    [[nodiscard]] MirInstId signBitI64(MirInstId addr, TypeId bitIntTy) {
        std::int64_t const n = interner.bitIntWidth(bitIntTy);
        MirInstId const top = loadLimb(limbAddrConst(addr, (n - 1) / 64));
        return i64bin(MirOpcode::And,
                      i64bin(MirOpcode::LShr, top, ci64((n - 1) % 64)), ci64(1));
    }

    // dst = `signBit ? (-src mod 2^N) : src`, BRANCHLESS (a 0/~0 select mask). Used for
    // the divide's operand magnitudes and its result sign-fixup. `emitWideNeg` masks the
    // scratch to `uType`; the caller re-masks dst to the SIGNED result type afterwards
    // when it wants sign-extension. src/dst/scratch are distinct slots. `signBit` is an
    // i64 0/1.
    [[nodiscard]] bool emitWideCondNeg(MirInstId dst, MirInstId srcAddr,
                                       MirInstId signBit, TypeId uType) {
        std::int64_t const limbCount = wideLimbCount(uType);
        MirInstId const scratch = freshAggregateTemp(uType);
        if (!scratch.valid()) return false;
        if (!emitWideNeg(scratch, srcAddr, uType)) return false;   // scratch = -src mod 2^N
        MirInstId const mask = i64un(MirOpcode::Neg, signBit);      // 0 or ~0
        MirInstId const notMask = i64un(MirOpcode::Not, mask);
        return emitLimbLoop(limbCount, [&](MirInstId i) {
            MirInstId const ni = loadLimb(limbAddrRuntime(scratch, i));
            MirInstId const si = loadLimb(limbAddrRuntime(srcAddr, i));
            MirInstId const sel = i64bin(MirOpcode::Or,
                                         i64bin(MirOpcode::And, ni, mask),
                                         i64bin(MirOpcode::And, si, notMask));
            if (!sel.valid()) return false;
            storeLimb(sel, limbAddrRuntime(dst, i));
            return true;
        });
    }

    // IN-PLACE `slot = (slot << 1) | carryIn0` over `limbCount` limbs (the divide's
    // per-bit shift, with the next dividend bit injected at position 0). In-place-safe:
    // each iteration reads then writes ONLY limb i and threads the shifted-out top bit
    // forward through a scalar carry slot (unlike the cross-limb `emitWideShift`, which
    // is NOT in-place-safe and masks away the bit that long division must inspect). The
    // top limb's final carry-out is DROPPED; by the divide's loop invariant (rem <
    // divisor ≤ 2^N-1 entering each shift), the shifted-out bit is provably 0, so nothing
    // is lost (the caller then maskTopLimbs the result to the N-bit window).
    [[nodiscard]] bool emitWideShl1InPlace(MirInstId slot, MirInstId carryIn0,
                                           std::int64_t limbCount) {
        MirInstId const carry = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!carry.valid()) return false;
        storeLimb(carryIn0, carry);
        return emitLimbLoop(limbCount, [&](MirInstId i) {
            MirInstId const v    = loadLimb(limbAddrRuntime(slot, i));
            MirInstId const cin  = loadLimb(carry);
            MirInstId const newv = i64bin(MirOpcode::Or,
                                          i64bin(MirOpcode::Shl, v, ci64(1)), cin);
            MirInstId const cout = i64bin(MirOpcode::LShr, v, ci64(63)); // bit leaving limb i
            if (!newv.valid() || !cout.valid()) return false;
            storeLimb(newv, limbAddrRuntime(slot, i));
            storeLimb(cout, carry);
            return true;
        });
    }

    // dst = (a * b) mod 2^N — schoolbook multiply of the LOW `limbCount` limbs via the
    // 64×64→128 UMulH primitive. The low N bits are two's-complement sign-agnostic, so
    // this ONE path serves signed AND unsigned; signedness enters only via the final
    // maskTopLimb. dst must be DISTINCT from a/b (it is a freshAggregateTemp). Products
    // a[i]*b[j] with i+j >= limbCount contribute only to bits >= limbCount*64 → dropped
    // (they wrap away mod 2^N); the inner bound is therefore `limbCount - i` (runtime).
    [[nodiscard]] bool emitWideMul(MirInstId dst, MirInstId aAddr, MirInstId bAddr,
                                   TypeId bitIntTy) {
        std::int64_t const limbCount = wideLimbCount(bitIntTy);
        if (!zeroWide(dst, limbCount)) return false;
        MirInstId const carry = mir.addInst(MirOpcode::Alloca, {}, ptrI64(), 0);
        if (!carry.valid()) return false;
        bool const ok = emitLimbLoop(limbCount, [&](MirInstId i) {   // outer: a[i]
            storeLimb(ci64(0), carry);                               // carry := 0 per i
            MirInstId const ai = loadLimb(limbAddrRuntime(aAddr, i));
            MirInstId const innerCount = i64bin(MirOpcode::Sub, ci64(limbCount), i);
            return emitLimbLoopN(innerCount, [&](MirInstId j) {      // inner: b[j], j<limbCount-i
                MirInstId const bj = loadLimb(limbAddrRuntime(bAddr, j));
                MirInstId const lo = i64bin(MirOpcode::Mul,   ai, bj);
                MirInstId const hi = i64bin(MirOpcode::UMulH, ai, bj);   // high 64 of a[i]*b[j]
                MirInstId const dAddr = limbAddrRuntime(dst, i64bin(MirOpcode::Add, i, j));
                MirInstId const dij = loadLimb(dAddr);
                MirInstId const cin = loadLimb(carry);
                MirInstId const s1 = i64bin(MirOpcode::Add, dij, lo);
                MirInstId const c1 = zextCmp(MirOpcode::ICmpUlt, s1, lo);  // s1<lo ⇒ carry
                MirInstId const s2 = i64bin(MirOpcode::Add, s1, cin);
                MirInstId const c2 = zextCmp(MirOpcode::ICmpUlt, s2, cin); // s2<cin ⇒ carry
                // ★ CRIT-A: the column carry-out is `hi + c1 + c2` via INTEGER Add —
                // NEVER Or(c1,c2). A multiply column can carry out 2 (c1+c2==2); OR would
                // silently drop 2^64. Fits u64 by the aggregate invariant
                // dst+a·b+carry ≤ (B-1)+(B-1)²+(B-1) = B²-1 ⟹ new carry ≤ B-1.
                MirInstId const newCarry =
                    i64bin(MirOpcode::Add, i64bin(MirOpcode::Add, hi, c1), c2);
                if (!s2.valid() || !newCarry.valid()) return false;
                storeLimb(s2, dAddr);
                storeLimb(newCarry, carry);
                return true;
            });
        });
        return ok && maskTopLimb(dst, bitIntTy);
    }

    // dst = a / b (wantRem=false) or a % b (wantRem=true), bit-precise wrap. Binary long
    // division of the operand MAGNITUDES (unsigned) with a C99 trunc-toward-zero sign
    // fixup. ★ CRIT-B: every magnitude/rem/quotient temp is driven by an UNSIGNED
    // `_BitInt(N)` type — the reused helpers bake signedness into their tail (emitWideNeg
    // sign-extends, emitWideOrder's top-limb compare goes signed), which would corrupt a
    // magnitude ≥ 2^(N-1); under the unsigned type they are correct and every internal
    // maskTopLimb stays a no-op. Only the FINAL sign fixup + mask use the SIGNED type.
    // Div-by-zero → a hard trap (Unreachable ⇒ ud2 / BRK #0), narrow-idiv #DE parity.
    [[nodiscard]] bool emitWideDivMod(MirInstId dst, MirInstId aAddr, MirInstId bAddr,
                                      TypeId resultTy, bool wantRem) {
        std::int64_t const n = interner.bitIntWidth(resultTy);
        std::int64_t const limbCount = wideLimbCount(resultTy);
        bool const signd = interner.bitIntIsSigned(resultTy);
        TypeId const uType = interner.bitInt(n, /*isSigned=*/false);   // CRIT-B

        // ── divide-by-zero → hard trap (narrow idiv #DE / SIGFPE parity) ──
        MirInstId const isZero = emitBoolFromWide(bAddr, resultTy, /*zeroIsTrue=*/true);
        if (!isZero.valid()) return false;
        MirBlockId const trapBB = mir.createBlock(StructCfMarker::IfThen);
        MirBlockId const okBB   = mir.createBlock(StructCfMarker::IfElse);
        mir.addCondBr(isZero, trapBB, okBB);
        mir.beginBlock(trapBB);
        mir.addUnreachable();   // ud2 (x86_64) / BRK #0 (arm64) — a real hardware fault
        mir.beginBlock(okBB);

        // ── operand magnitudes (signed: |a|,|b| in unsigned temps; unsigned: direct) ──
        MirInstId divA = aAddr, divB = bAddr;
        MirInstId sa = InvalidMirInst, sb = InvalidMirInst;
        if (signd) {
            MirInstId const ua = freshAggregateTemp(uType);
            MirInstId const ub = freshAggregateTemp(uType);
            if (!ua.valid() || !ub.valid()) return false;
            sa = signBitI64(aAddr, resultTy);
            sb = signBitI64(bAddr, resultTy);
            if (!emitWideCondNeg(ua, aAddr, sa, uType)) return false;   // ua = |a|
            if (!emitWideCondNeg(ub, bAddr, sb, uType)) return false;   // ub = |b|
            divA = ua; divB = ub;
        }

        // ── unsigned binary long division: divA / divB → rem (remainder), uq (quotient) ──
        MirInstId const rem = freshAggregateTemp(uType);
        MirInstId const uq  = wantRem ? InvalidMirInst : freshAggregateTemp(uType);
        if (!rem.valid() || (!wantRem && !uq.valid())) return false;
        if (!zeroWide(rem, limbCount)) return false;
        if (!wantRem && !zeroWide(uq, limbCount)) return false;
        bool const ok = emitLimbLoop(n, [&](MirInstId iv) {   // iv=0..N-1 ; bit k=N-1-iv
            MirInstId const k     = i64bin(MirOpcode::Sub, ci64(n - 1), iv);
            MirInstId const kLimb = i64bin(MirOpcode::LShr, k, ci64(6));   // k/64
            MirInstId const kBit  = i64bin(MirOpcode::And,  k, ci64(63));  // k%64
            MirInstId const dv    = loadLimb(limbAddrRuntime(divA, kLimb));
            MirInstId const bitk  = i64bin(MirOpcode::And,
                                       i64bin(MirOpcode::LShr, dv, kBit), ci64(1));
            // rem = ((rem << 1) | bitk) mod 2^N.
            if (!emitWideShl1InPlace(rem, bitk, limbCount)) return false;
            if (!maskTopLimb(rem, uType)) return false;
            // doSub = (rem >=u divisor). No captured "overflow bit N" is needed: by the
            // loop invariant rem < divisor ≤ 2^N-1 entering each shift (a divisor >
            // 2^(N-1) forces quotient ≤ 1 ⇒ no mid-stream subtract keeps rem small), the
            // shifted rem is always < 2^N — the shifted-out top bit is provably 0. This is
            // the design-audit's original sizing, verified exhaustively by the C3
            // code-audit (a captured bit N fired on 0 of hundreds of thousands of trials).
            MirInstId const doSub = emitWideOrder(HirOpKind::Ge, rem, divB, uType);
            if (!doSub.valid()) return false;
            // ★ N-1: the conditional subtract is a CondBr diamond inside the loop body —
            // it MUST converge to a SINGLE open block (contBB) before the body returns,
            // so emitLimbLoop's ctr++/back-edge form off it.
            MirBlockId const subBB  = mir.createBlock(StructCfMarker::IfThen);
            MirBlockId const contBB = mir.createBlock(StructCfMarker::IfJoin);
            mir.addCondBr(doSub, subBB, contBB);
            mir.beginBlock(subBB);
            if (!emitWideAddSub(/*isSub=*/true, rem, rem, divB, uType)) {   // rem -= divisor
                if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                sealCreatedAsUnreachable(contBB);
                return false;
            }
            if (!wantRem) {   // set quotient bit k: uq[kLimb] |= (1 << kBit)
                MirInstId const qAddr = limbAddrRuntime(uq, kLimb);
                MirInstId const qbit  = i64bin(MirOpcode::Shl, ci64(1), kBit);
                storeLimb(i64bin(MirOpcode::Or, loadLimb(qAddr), qbit), qAddr);
            }
            mir.addBr(contBB);
            mir.beginBlock(contBB);
            return true;
        });
        if (!ok) return false;

        // ── select the magnitude, apply the sign, mask to the result type ──
        MirInstId const mag = wantRem ? rem : uq;
        if (!signd) {
            if (!copyWide(dst, mag, limbCount)) return false;
            return maskTopLimb(dst, resultTy);
        }
        // C99 trunc-toward-zero: q takes sign (sa^sb); r takes the DIVIDEND's sign (sa).
        MirInstId const flip = wantRem ? sa : i64bin(MirOpcode::Xor, sa, sb);
        if (!emitWideCondNeg(dst, mag, flip, uType)) return false;   // dst = flip ? -mag : mag
        return maskTopLimb(dst, resultTy);                          // re-mask SIGNED (sign-ext)
    }

    // The plain integer kind a scalar SOURCE projects to (enum→underlying, narrow
    // `_BitInt`→container, else its own kind) — the scalar feeding a wide conversion
    // or a shift count. A WIDE `_BitInt` source is handled separately by the caller
    // (this must NOT be called on one — `bitIntContainerKind` is fatal for N>64).
    [[nodiscard]] TypeKind resolveScalarIntKind(TypeId ty) {
        if (!ty.valid()) return TypeKind::I32;
        TypeKind const k = interner.kind(ty);
        if (k == TypeKind::Enum) {
            auto const sc = interner.scalars(ty);
            return sc.empty() ? k : static_cast<TypeKind>(sc[0]);
        }
        if (k == TypeKind::BitInt) return interner.bitIntContainerKind(ty);
        return k;
    }

    // Lower a shift COUNT expression to an i64 value (C 6.5.7: the count keeps its own
    // type — it is NOT converted to the shifted operand's type). Widens to i64 for the
    // multi-limb word/bit split. A wide-`_BitInt` count (pathological) reads its low limb.
    [[nodiscard]] MirInstId lowerShiftCountToI64(HirNodeId countNode) {
        TypeId const ct = hir.typeId(countNode);
        if (isWideBitInt(interner, ct)) {
            MirInstId const addr = lowerLvalueAddress(countNode);
            if (!addr.valid()) return InvalidMirInst;
            return emitScalarFromWide(addr, TypeKind::I64);
        }
        MirInstId const cv = lowerExpr(countNode);
        if (!cv.valid()) return InvalidMirInst;
        TypeKind const ck = resolveScalarIntKind(ct);
        if (ck == TypeKind::I64 || ck == TypeKind::U64) return cv;
        MirOpcode const conv =
            mapCast(ck, isSignedIntKind(ck) ? TypeKind::I64 : TypeKind::U64);
        if (conv == MirOpcode::Invalid) return InvalidMirInst;
        std::array<MirInstId, 1> a{cv};
        return mir.addInst(conv, a, i64Ty());
    }

    // ── Model A: materialize a wide-`_BitInt` RVALUE into a fresh slot, return its
    // ADDRESS. lowerLvalueAddressNode routes a wide-BitInt-typed BinaryOp/UnaryOp/Cast
    // here (they are the FIRST aggregates in C produced by arithmetic). Operands are
    // reached BY ADDRESS (a wide operand is itself memory-resident; nested wide
    // arithmetic materializes into its own slot). This is the C2 wrap chokepoint: every
    // producing op ends by masking the top limb.
    [[nodiscard]] MirInstId materializeWideBinaryOp(HirNodeId node) {
        TypeId const t = hir.typeId(node);
        HirOpKind const op = decodeCoreOp(hir.payload(node));
        auto kids = hir.children(node);
        if (kids.size() != 2) {
            unsupported(node, "malformed wide _BitInt BinaryOp (expect 2 children)");
            return InvalidMirInst;
        }
        // C3 (D-CSUBSET-BITINT-C3-MULDIV): * / % on a wide `_BitInt` now LOWER to the
        // multi-limb `emitWideMul` / `emitWideDivMod` below — the C2 fail-loud
        // (S_BitIntWideMulDivUnsupported) is retired for these ops.
        MirInstId const aAddr = lowerLvalueAddress(kids[0]);
        if (!aAddr.valid()) return InvalidMirInst;
        MirInstId const dst = freshAggregateTemp(t);
        if (!dst.valid()) {
            unsupported(node, "wide _BitInt result requires a sizeable layout");
            return InvalidMirInst;
        }
        bool ok = false;
        if (op == HirOpKind::Shl || op == HirOpKind::Shr) {
            MirInstId const kI64 = lowerShiftCountToI64(kids[1]);
            if (!kI64.valid()) return InvalidMirInst;
            bool const isLeft = (op == HirOpKind::Shl);
            ok = emitWideShift(isLeft,
                               /*isArith=*/!isLeft && interner.bitIntIsSigned(t),
                               dst, aAddr, kI64, t);
        } else {
            MirInstId const bAddr = lowerLvalueAddress(kids[1]);
            if (!bAddr.valid()) return InvalidMirInst;
            switch (op) {
                case HirOpKind::BitAnd:
                    ok = emitWideBitwise(MirOpcode::And, dst, aAddr, bAddr, t); break;
                case HirOpKind::BitOr:
                    ok = emitWideBitwise(MirOpcode::Or,  dst, aAddr, bAddr, t); break;
                case HirOpKind::BitXor:
                    ok = emitWideBitwise(MirOpcode::Xor, dst, aAddr, bAddr, t); break;
                case HirOpKind::Add:
                    ok = emitWideAddSub(false, dst, aAddr, bAddr, t); break;
                case HirOpKind::Sub:
                    ok = emitWideAddSub(true,  dst, aAddr, bAddr, t); break;
                case HirOpKind::Mul:
                    ok = emitWideMul(dst, aAddr, bAddr, t); break;
                case HirOpKind::Div:
                    ok = emitWideDivMod(dst, aAddr, bAddr, t, /*wantRem=*/false); break;
                case HirOpKind::Rem:
                    ok = emitWideDivMod(dst, aAddr, bAddr, t, /*wantRem=*/true); break;
                default:
                    unsupported(node, std::format(
                        "BinaryOp '{}' producing a wide _BitInt is not supported",
                        opName(op)));
                    return InvalidMirInst;
            }
        }
        return ok ? dst : InvalidMirInst;
    }

    [[nodiscard]] MirInstId materializeWideUnaryOp(HirNodeId node) {
        TypeId const t = hir.typeId(node);
        HirOpKind const op = decodeCoreOp(hir.payload(node));
        auto kids = hir.children(node);
        if (kids.size() != 1) {
            unsupported(node, "malformed wide _BitInt UnaryOp (expect 1 child)");
            return InvalidMirInst;
        }
        MirInstId const aAddr = lowerLvalueAddress(kids[0]);
        if (!aAddr.valid()) return InvalidMirInst;
        MirInstId const dst = freshAggregateTemp(t);
        if (!dst.valid()) {
            unsupported(node, "wide _BitInt result requires a sizeable layout");
            return InvalidMirInst;
        }
        bool ok = false;
        switch (op) {
            case HirOpKind::Neg:    ok = emitWideNeg(dst, aAddr, t); break;
            case HirOpKind::BitNot: ok = emitWideNot(dst, aAddr, t); break;
            default:
                // Logical `!` produces a Bool (not a wide result) → handled in the
                // value path (combineUnary), never here.
                unsupported(node, std::format(
                    "UnaryOp '{}' producing a wide _BitInt is not supported",
                    opName(op)));
                return InvalidMirInst;
        }
        return ok ? dst : InvalidMirInst;
    }

    // A wide `_BitInt` LITERAL materialized into a slot — the synthetic `1` that ++/--
    // desugars to (`incDecArithValue` synths a `_BitInt(N)`-typed one), or a small
    // `wb`/`uwb` literal whose value fits in 64 bits. Value → limb 0 (sign/zero-extended
    // per the type's signedness), fill the rest, mask the top limb. An arbitrary-
    // magnitude literal (value beyond i64 — D-CSUBSET-BITINT-WIDE-LITERAL) is C4-
    // deferred; a non-integer literal is a front-end invariant break — both fail loud,
    // never a silent truncation.
    [[nodiscard]] MirInstId materializeWideLiteral(HirNodeId node) {
        TypeId const t = hir.typeId(node);
        HirLiteralValue const& src = literals.at(hir.payload(node));
        MirInstId const dst = freshAggregateTemp(t);
        if (!dst.valid()) {
            unsupported(node, "wide _BitInt literal requires a sizeable layout");
            return InvalidMirInst;
        }
        // C4b (D-CSUBSET-BITINT-WIDE-LITERAL): an arbitrary-magnitude `wb`/`uwb`
        // literal lives in the `BitIntValue` pool arm. Fill the ceil(N/64) Model-A
        // limbs DIRECTLY from the host value (converted to the node's exact
        // (width, signed) so the limb count + top-limb wrap match the slot) — a
        // COMPILE-TIME limb fill, no runtime FROM-scalar sign/zero-extension.
        if (auto const* bv = std::get_if<BitIntValue>(&src.value)) {
            std::uint32_t const n   = static_cast<std::uint32_t>(interner.bitIntWidth(t));
            bool const          sgn = interner.bitIntIsSigned(t);
            BitIntValue const   conv = bv->withType(n, sgn);
            auto const&         limbs = conv.limbs();
            std::int64_t const  count = wideLimbCount(t);
            for (std::int64_t i = 0; i < count; ++i) {
                std::uint64_t const limbVal =
                    (static_cast<std::size_t>(i) < limbs.size()) ? limbs[i] : 0ull;
                storeLimb(ci64(static_cast<std::int64_t>(limbVal)), limbAddrConst(dst, i));
            }
            return dst;
        }
        // Legacy path: the synthetic `1` that ++/-- desugars to (a `_BitInt(N)`-typed
        // one) is a plain int64-arm literal. Extend per the DECLARED signedness: an
        // unsigned wide literal zero-fills, a signed one sign-fills (a negative
        // value's 2's-complement high limbs).
        if (!std::holds_alternative<std::int64_t>(src.value)) {
            unsupported(node, "a wide _BitInt literal must be a bit-precise value or a "
                              "64-bit integer constant");
            return InvalidMirInst;
        }
        TypeKind const srcK =
            interner.bitIntIsSigned(t) ? TypeKind::I64 : TypeKind::U64;
        return emitWideFromScalar(dst, ci64(std::get<std::int64_t>(src.value)), srcK, t)
                   ? dst : InvalidMirInst;
    }

    [[nodiscard]] MirInstId materializeWideCast(HirNodeId node) {
        TypeId const t = hir.typeId(node);   // wide `_BitInt` target
        auto kids = hir.children(node);
        if (kids.size() != 1) {
            unsupported(node, "malformed wide _BitInt Cast (expect 1 child)");
            return InvalidMirInst;
        }
        TypeId const srcTy = hir.typeId(kids[0]);
        // D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV: `float -> wide _BitInt(N>64)` is NOT
        // yet supported — the naive scalar path (emitWideFromScalar) would emit an
        // FPToUI/FPToSI keyed off the source and fill only limb 0 (wrong sign, wrong
        // value, dropped upper limbs). Fail LOUD here rather than silently miscompile;
        // the correct multi-limb FP->limbs conversion lands in a later cycle. NARROW
        // (N<=64) float->_BitInt is unaffected — it never reaches materializeWideCast.
        if (srcTy.valid() && isFloatingKind(interner.kind(srcTy))) {
            diagnoseCode(node, DiagnosticCode::S_BitIntWideFloatConvUnsupported,
                "conversion from a floating type to a `_BitInt` wider than 64 bits is "
                "not yet supported — the multi-limb float-to-bit-precise conversion "
                "lands in a later cycle (D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV)");
            return InvalidMirInst;
        }
        MirInstId const dst = freshAggregateTemp(t);
        if (!dst.valid()) {
            unsupported(node, "wide _BitInt result requires a sizeable layout");
            return InvalidMirInst;
        }
        bool ok = false;
        if (isWideBitInt(interner, srcTy)) {
            MirInstId const srcAddr = lowerLvalueAddress(kids[0]);
            if (!srcAddr.valid()) return InvalidMirInst;
            ok = emitWideFromWide(dst, srcAddr, srcTy, t);
        } else {
            MirInstId const srcVal = lowerExpr(kids[0]);
            if (!srcVal.valid()) return InvalidMirInst;
            ok = emitWideFromScalar(dst, srcVal, resolveScalarIntKind(srcTy), t);
        }
        return ok ? dst : InvalidMirInst;
    }

    // A `_BitInt` opcode is an integer comparison (result Bool, operands promoted).
    [[nodiscard]] static bool isIntCompareOpcode(MirOpcode op) noexcept {
        switch (op) {
            case MirOpcode::ICmpEq:  case MirOpcode::ICmpNe:
            case MirOpcode::ICmpSlt: case MirOpcode::ICmpSle:
            case MirOpcode::ICmpSgt: case MirOpcode::ICmpSge:
            case MirOpcode::ICmpUlt: case MirOpcode::ICmpUle:
            case MirOpcode::ICmpUgt: case MirOpcode::ICmpUge:
                return true;
            default:
                return false;
        }
    }

    // Lower ONE HIR expression node in the currently-open MIR block, given
    // that its child sub-expressions are lowered by RE-ENTERING `lowerExpr`
    // (the driver below). Returns the MirInstId that produces the value
    // (`InvalidMirInst` on error — caller decides whether to keep emitting).
    //
    // Plan 24 Stage 4 (D-PARSE-DEEP-NEST-RECURSION-MEMORY): the public entry
    // `lowerExpr` is now an explicit heap work-stack DRIVER. The deep
    // STRAIGHT-LINE arms (UnaryOp / BinaryOp / Deref / non-array-decay Cast —
    // the ones whose only recursion is `lowerExpr(child)` into the SAME block,
    // no CFG) are flattened onto that work-stack so a deeply-nested `a+b+c…` /
    // `-(-(-x))` / `*(*(*p))` / `(T)(T)x` chain carries FLAT O(1) host-stack
    // cost per nesting level. This per-NODE handler is the byte-identical
    // emission body for EVERY OTHER arm (leaves, Call, the CFG arms
    // Ternary/LogicalAnd/Or/SeqExpr, the by-address MemberAccess/Index/
    // AddressOf delegations); `enterValue` routes those here unchanged (their
    // own `lowerExpr(child)` calls re-enter the driver, so deep sub-expressions
    // inside a shallow complex arm still flatten). The four flattened arms here
    // call the SAME `combine*` epilogues the frames do (one source of truth)
    // and are unreachable through the driver — kept as the recursive fallback.
    [[nodiscard]] MirInstId lowerExprNode(HirNodeId node) {
        HirKind const k = hir.kind(node);
        TypeId const  t = hir.typeId(node);
        switch (k) {
            case HirKind::Literal: {
                // The HIR literal's payload is its index into the
                // HirLiteralPool. Copy the variant into a MirLiteralValue
                // (same shape — the two pools are structurally identical)
                // and emit a Const instruction.
                std::uint32_t const idx = hir.payload(node);
                HirLiteralValue const& src = literals.at(idx);
                // FC2 Part B (F64 constant materialization), WIDENED
                // by FC3.5 sweep-c2 (D-CSUBSET-F32-CODEGEN): an F64 OR
                // F32 float literal in a function body lowers the way
                // STRING literals do (the D-LK4-RODATA-PRODUCER-STRING
                // shape below) — mint an anonymous module global whose
                // constant-init carries the value, then GlobalAddr +
                // Load. Register machines have no float-immediate
                // instruction form; the prior `Const` route dead-ended
                // in the LirLiteralPool (no encoding variant consumes
                // a LiteralIndex operand). Per-occurrence global, no
                // dedup — mirrors the string path exactly. The data
                // item is the TYPE's width: lowerMirGlobalsToDataItems'
                // F32 arm narrows double→float before the bit-cast
                // (4-byte item, alignment 4) and the F32-width movss/
                // LDUR-S load reads exactly 4 bytes. (Decimal→double→
                // float can double-round vs a direct decimal→float
                // parse in rare cases — the literal pool carries
                // `double` only; a typed-float pool arm is the
                // D-LK4-RODATA-PRODUCER-EXOTIC-FLOAT successor's
                // concern. Exactly-representable corpus values are
                // unaffected.) F16/F128 fall through to `addConst`
                // and fail loud at MIR→LIR
                // (D-TARGET-ENCODING-WIDTH-GUARD — promoting them
                // here would silently pair them with wrong-width
                // load/arithmetic encodings).
                if (std::holds_alternative<double>(src.value)
                    && t.valid()
                    && (interner.kind(t) == TypeKind::F64
                        || interner.kind(t) == TypeKind::F32)) {
                    SymbolId const sym = mintSyntheticGlobalSymbol();
                    if (!sym.valid()) {
                        unsupported(node,
                            "float-literal promotion failed: synthetic "
                            "SymbolId space exhausted (UINT32_MAX "
                            "wraparound) — same guard as the string-"
                            "literal minter.");
                        return InvalidMirInst;
                    }
                    std::uint32_t const mirLitIdx =
                        mir.literalPoolAdd(toMirLiteral(src));
                    (void)mir.addGlobal(t, sym, mirLitIdx, MirFuncId{},
                                        SymbolBinding::Global,
                                        SymbolVisibility::Default,
                                        /*isConst=*/false,
                                        MirThreadStorage::Shared);
                    TypeId const ptrTy = interner.pointer(t);
                    MirInstId const addr = mir.addGlobalAddr(sym, ptrTy);
                    std::array<MirInstId, 1> ops{addr};
                    return mir.addInst(MirOpcode::Load, ops, t);
                }
                // C4b (D-CSUBSET-BITINT-WIDE-LITERAL): a `wb`/`uwb` literal's value
                // lives in the `BitIntValue` pool arm. A NARROW (N≤64) one
                // materializes into its C1 container: emit a container-int Const
                // typed as the `_BitInt` (reprKind projects it to the native
                // container at LIR). The value is already mod-2^N wrapped and a
                // literal is a non-negative magnitude, so its low bits ARE the
                // container value. A WIDE (N>64) literal is memory-resident (reached
                // by address via materializeWideLiteral) — never a scalar value.
                if (auto const* bv = std::get_if<BitIntValue>(&src.value)) {
                    if (t.valid() && interner.kind(t) == TypeKind::BitInt
                        && interner.bitIntWidth(t) <= 64) {
                        MirLiteralValue lit;
                        lit.core = TypeKind::BitInt;
                        if (interner.bitIntIsSigned(t)) lit.value = bv->asI64();
                        else                            lit.value = bv->low64();
                        return mir.addConst(std::move(lit), t);
                    }
                    unsupported(node,
                        "a wide _BitInt(N>64) literal cannot be produced as a scalar "
                        "value (it is memory-resident, reached by address)");
                    return InvalidMirInst;
                }
                return mir.addConst(toMirLiteral(src), t);
            }
            case HirKind::SizeOf: {
                // FC6: the TypeRef child carries the type being sized (its typeId).
                auto kids = hir.children(node);
                if (kids.empty()) {
                    unsupported(node, "SizeOf has no type-ref child");
                    return InvalidMirInst;
                }
                TypeId const sized = hir.typeId(kids.front());
                // VLA C2/C3 (D-CSUBSET-VLA): a `sizeof <vla-object>` (whole `sizeof a`
                // OR row `sizeof a[0]`) is a RUNTIME value — the byte size frozen at the
                // VLA's declaration (C 6.7.6.2p2), NOT a static layout fold (a VLA's size
                // is not a constant expression, C 6.6). If this SizeOf node is the
                // recorded VLA-object form, LOAD its decl-frozen `(sym, sized-type)` slot
                // (U64 = `t`) — the SAME per-object slot family the index path Loads
                // (CRITICAL-1: `sized` IS the slot's shape-type key, so `sizeof a` Loads
                // the whole total and `sizeof a[0]` Loads the row stride, by
                // construction). This runs ONLY for a genuine runtime sizeof: a
                // constant-required context (`_Static_assert`, an array bound) const-
                // evaluates the sizeof and DECLINES it (the VLA TypeRef → `computeLayout`
                // nullopt) BEFORE MIR, so it never reaches here.
                if (sizeofVlaSymMap != nullptr) {
                    if (auto it = sizeofVlaSymMap->find(node.v);
                        it != sizeofVlaSymMap->end()) {
                        auto slotIt =
                            vlaStrideSlot.find(vlaSlotKey(it->second, sized.v));
                        if (slotIt == vlaStrideSlot.end()) {
                            // The VLA's decl (which materializes the slot) dominates every
                            // use of the object, so the slot MUST exist. Absent ⇒ an
                            // internal side-table desync — fail loud, never a stale/guessed
                            // size (mirrors `vlaAllocaForLocal`'s desync guard).
                            unsupported(node,
                                        "sizeof of a variable-length array whose "
                                        "decl-frozen size slot was not materialized "
                                        "(internal side-table desync)");
                            return InvalidMirInst;
                        }
                        std::array<MirInstId, 1> ld{slotIt->second};
                        return mir.addInst(MirOpcode::Load, ld, t);
                    }
                }
                // FC6: fold sizeof(T) to T's byte size (result type size_t = U64,
                // = `t`) via the type_layout engine. Fail loud — never a guessed size
                // — if the target declared no layout params or the type is
                // incomplete/un-sizeable.
                if (!config.aggregateLayoutLoaded) {
                    unsupported(node, "sizeof requires the target to declare its "
                                      "'aggregateLayout' params");
                    return InvalidMirInst;
                }
                auto const layout = computeLayout(sized, interner,
                                                  config.aggregateLayout,
                                                  config.dataModel);
                if (!layout) {
                    unsupported(node, "sizeof of an incomplete or un-sizeable type");
                    return InvalidMirInst;
                }
                MirLiteralValue lit;
                lit.value = static_cast<std::uint64_t>(layout->size);
                lit.core  = TypeKind::U64;
                return mir.addConst(std::move(lit), t);
            }
            case HirKind::AlignOf: {
                // C11/C23 6.5.3.4: fold _Alignof(T) to T's alignment (result type
                // size_t = U64, = `t`) via the type_layout engine. An ADDITIVE
                // mirror of the SizeOf case reading `align` instead of `size`. The
                // TypeRef child carries the queried type. Fail loud — never a
                // guessed alignment — if the target declared no layout params or
                // the type is incomplete/un-alignable.
                if (!config.aggregateLayoutLoaded) {
                    unsupported(node, "_Alignof requires the target to declare its "
                                      "'aggregateLayout' params");
                    return InvalidMirInst;
                }
                auto kids = hir.children(node);
                if (kids.empty()) {
                    unsupported(node, "AlignOf has no type-ref child");
                    return InvalidMirInst;
                }
                TypeId const queried = hir.typeId(kids.front());
                auto const layout = computeLayout(queried, interner,
                                                  config.aggregateLayout,
                                                  config.dataModel);
                if (!layout) {
                    unsupported(node, "_Alignof of an incomplete or un-alignable type");
                    return InvalidMirInst;
                }
                MirLiteralValue lit;
                lit.value = static_cast<std::uint64_t>(layout->align.bytes());
                lit.core  = TypeKind::U64;
                return mir.addConst(std::move(lit), t);
            }
            case HirKind::VaStart: return lowerVaStart(node);
            case HirKind::VaArg:   return lowerVaArg(node);
            case HirKind::VaEnd:   return lowerVaEnd(node);
            case HirKind::Ref: {
                // Resolution order:
                //   1. Addressable local (slot-backed: body-VarDecl or address-
                //      taken param) — emit `Load(alloca)`; the value's type IS
                //      the HIR node's type (`t`), which is the pointee type.
                //   2. Local SSA value (param NOT address-taken / pure-SSA
                //      temporary) — return its already-emitted MirInstId.
                //   3. Module global — emit `GlobalAddr` for the pointer-to-
                //      storage, then `Load` for the rvalue read. The lvalue
                //      path in `lowerLvalueAddress` returns the GlobalAddr
                //      directly so `Store` / `AddressOf` can use it.
                //   4. Module function — emit `GlobalAddr` to the symbol. The
                //      result type IS the FnSig (matching HIR's convention
                //      where Ref-to-function's typeId is the FnSig directly);
                //      MIR's Call accepts that uniformly.
                //   5. Anything else (externs) → unbound at this lowering
                //      tier (FFI plan 11 owns extern-symbol resolution).
                std::uint32_t const sym = hir.payload(node);
                if (auto it = addressableLocal.find(sym);
                    it != addressableLocal.end()) {
                    // c63 (D-CSUBSET-ARRAY-DECAY-AT-CALL-ARG): an ARRAY rvalue
                    // decays to the ADDRESS of its first element (C 6.3.2.1p3) — an
                    // aggregate can never live in a register, so a bare `Ref` to an
                    // array-typed addressable local must NEVER `Load`. Most array
                    // rvalues reach MIR pre-decayed (CST→HIR `coerce()` inserts a
                    // synthetic `Cast<Array→Ptr>` — the c59 `*(array+i)` /
                    // string-literal-arg / `int a[5]; f(a)` paths), but when the
                    // array type MATCHES the consuming context's type NO cast is
                    // inserted (a SysV `va_list` `__va_list_tag[1]` arg forwarded to
                    // a `va_list` PARAM — same Array type), so the bare `Ref` lands
                    // here. Decay exactly as the `Cast<Array→Ptr>` arm does (a byte-
                    // offset-0 `Gep` re-typing the base address to `Ptr<elem>` =
                    // `&arr[0]`). `it->second` is already the array's ADDRESS — an
                    // alloca `Ptr<Array>` for a body array, or (c63) the registered
                    // incoming `Ptr<__va_list_tag>` for the forwarded va_list param;
                    // the Gep(0) lands the correct `Ptr<elem>` value either way. The
                    // decayed pointer rides a GPR (`scalarArgClass(Array)` → Gpr),
                    // so the scalar call-arg accounting is unchanged.
                    if (t.valid() && interner.kind(t) == TypeKind::Array) {
                        auto const elems = interner.operands(t);
                        if (elems.empty() || !elems[0].valid()) {
                            unsupported(node,
                                "array-typed Ref has no element type "
                                "(interner invariant violated)");
                            return InvalidMirInst;
                        }
                        TypeId const decayed = interner.pointer(elems[0]);
                        std::array<MirInstId, 2> gep{it->second, constInt(0)};
                        return mir.addInst(MirOpcode::Gep, gep, decayed);
                    }
                    std::array<MirInstId, 1> ops{it->second};
                    // c21/c27: a Ref to a `volatile` address-taken local — its
                    // rvalue Load carries the flag. c21 via the node's VolatileAttr
                    // (object-volatile symbol); c27 also OR's the value type `t`
                    // (top-level VolatileQual) so the two halves agree by
                    // construction. FC17.9(d): an `_Atomic` local's Load becomes
                    // AtomicLoad via the scalar-access chokepoint.
                    return emitScalarLoad(ops, t, node);
                }
                if (auto it = symbolToValue.find(sym); it != symbolToValue.end()) {
                    return it->second;
                }
                if (globalSymbols.contains(sym)) {
                    // c91 (D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY): the GLOBAL
                    // twin of the c63 addressable-local arm above — an ARRAY
                    // rvalue decays to the ADDRESS of its first element
                    // (C 6.3.2.1p3); an aggregate can never live in a
                    // register, so a bare Ref to an array-typed GLOBAL must
                    // NEVER `Load`. Pre-c91 this arm loaded the array's first
                    // bytes as the "value" — `gp != g0` compared g0's CONTENT
                    // against gp (always-unequal for a live pointer; EQUAL to
                    // a null pointer for a zero-filled global — both silent
                    // wrong-branch miscompiles; the c90r4 global witness).
                    // Nearly every array rvalue reaches MIR pre-decayed (the
                    // HIR coerce Cast<Array→Ptr> funnel — comparisons,
                    // conditions and `!` joined it in c91), but a NO-CAST
                    // same-type context (a file-scope `va_list` forwarded to
                    // a `va_list` param — the c63 shape at global scope)
                    // still lands a bare Array-typed Ref here, and this arm
                    // keeps any FUTURE consumer gap a correct decay instead
                    // of a silent content-compare. Same emission as c63: a
                    // byte-offset-0 `Gep` re-typing the base address to
                    // `Ptr<elem>` = `&arr[0]`.
                    if (t.valid() && interner.kind(t) == TypeKind::Array) {
                        auto const elems = interner.operands(t);
                        if (elems.empty() || !elems[0].valid()) {
                            unsupported(node,
                                "array-typed global Ref has no element type "
                                "(interner invariant violated)");
                            return InvalidMirInst;
                        }
                        MirInstId const base = mir.addGlobalAddr(
                            SymbolId{sym}, interner.pointer(t));
                        std::array<MirInstId, 2> gep{base, constInt(0)};
                        return mir.addInst(MirOpcode::Gep, gep,
                                           interner.pointer(elems[0]));
                    }
                    // Globals: GlobalAddr's result type is pointer(t); a
                    // following Load reads the value. The HIR Ref's typeId
                    // is the global's declared type, not pointer(type).
                    TypeId const ptrTy = interner.pointer(t);
                    MirInstId const addr = mir.addGlobalAddr(SymbolId{sym}, ptrTy);
                    std::array<MirInstId, 1> ops{addr};
                    // c21/c27: a Ref to a `volatile` global — its rvalue Load
                    // carries the flag (c21 VolatileAttr OR c27 value-type
                    // VolatileQual). FC17.9(d): an `_Atomic` global's Load becomes
                    // AtomicLoad via the scalar-access chokepoint.
                    return emitScalarLoad(ops, t, node);
                }
                if (functionSymbols.contains(sym)) {
                    return mir.addGlobalAddr(SymbolId{sym}, t);
                }
                unsupported(node,
                    std::format("HIR Ref to unbound symbol {} (not a local "
                                "SSA value, addressable local, module global, "
                                "or module function)", sym));
                return InvalidMirInst;
            }
            case HirKind::UnaryOp: {
                std::uint32_t const payload = hir.payload(node);
                if (!isCoreOp(payload)) {
                    unsupported(node, "extension UnaryOp (post-v1)");
                    return InvalidMirInst;
                }
                auto kids = hir.children(node);
                if (kids.size() != 1) {
                    unsupported(node, "malformed UnaryOp (verifier should have flagged)");
                    return InvalidMirInst;
                }
                MirInstId const operand = lowerExpr(kids[0]);
                if (!operand.valid()) return InvalidMirInst;
                return combineUnary(node, operand);
            }
            case HirKind::Call: {
                // children: [callee, args...]. Lower the callee (a Ref-to-
                // function becomes a `GlobalAddr`; a function-pointer
                // expression becomes whatever MirInstId it lowers to) and
                // every arg, then emit a MIR Call. The result type comes
                // from the HIR node's typeId (the call's result type — HIR
                // already pulled it from the callee's FnSig at lowering
                // time). A void-returning callee has typeId == InvalidType,
                // which Call's MirResultRule::Optional accepts.
                //
                // Plan 24 (hir_to_mir Call residual): this RECURSIVE body is now
                // the byte-identical FALLBACK — it drives the SAME shared helpers
                // (`callSetup` / `processOneCallArg` / `finishScalarCallArg` /
                // `finishCall`) as the iterative `runExprDriver` Call frame, which
                // is what actually lowers a call at runtime (so a deep
                // `f(f(f(…)))` chain carries flat host-stack cost — only the arg
                // VALUE-lowering recursion is hoisted; the struct ABI synthesis
                // stays inline). The two stay in lockstep because the emission
                // sequence lives in ONE place. This arm is unreachable via the
                // driver but kept as the recursive source of truth.
                auto kids = hir.children(node);
                if (kids.empty()) {
                    unsupported(node, "malformed Call (no callee child)");
                    return InvalidMirInst;
                }
                MirInstId const callee = lowerExpr(kids[0]);
                if (!callee.valid()) return InvalidMirInst;
                CallLowerCtx ctx{.node = node, .resultTy = t};
                if (!callSetup(callee, ctx)) return InvalidMirInst;
                for (;;) {
                    CallArgStep const step = processOneCallArg(ctx);
                    if (step == CallArgStep::Error) return InvalidMirInst;
                    if (step == CallArgStep::Done) break;
                    if (step == CallArgStep::StructDone) continue;
                    // ScalarPending: lower the in-flight scalar arg's VALUE
                    // (recursively here; the driver frame routes it onto the
                    // work-stack), then collect it.
                    MirInstId const arg = lowerExpr(kids[ctx.argIdx]);
                    if (!arg.valid()) return InvalidMirInst;
                    finishScalarCallArg(ctx, arg);
                }
                return finishCall(ctx);
            }
            case HirKind::IntrinsicCall: {
                // children: [args...]; the intrinsic id lives in payload.
                // MirOpcode::IntrinsicCall has the same Optional result rule.
                // Plan 24: this recursive body is the byte-identical fallback for
                // the driver's IntrinsicCall frame (all-scalar args; no callee
                // child, no struct ABI). Emission order: lower each arg left→
                // right, push, then emit the IntrinsicCall.
                auto kids = hir.children(node);
                CallLowerCtx ctx{.node = node, .resultTy = t,
                                 .isIntrinsic = true, .intrinsicId = hir.payload(node)};
                ctx.operands.reserve(kids.size());
                for (HirNodeId argN : kids) {
                    MirInstId const arg = lowerExpr(argN);
                    if (!arg.valid()) return InvalidMirInst;
                    ctx.operands.push_back(arg);
                }
                return mir.addInst(MirOpcode::IntrinsicCall, ctx.operands, t,
                                   ctx.intrinsicId);
            }
            case HirKind::BuiltinCall: {
                // c103 (D-CSUBSET-INTRINSIC-UMULH): children = [args...]; the payload
                // is a BuiltinLowering value. Map it to the DEDICATED MirOpcode — the
                // ONE place MIR names the intrinsic vocabulary (a uniform enum→enum
                // table, never an arch/name identity branch). Unlike IntrinsicCall (a
                // generic opaque op dispatched at the encoder), this yields a REAL MIR
                // op the optimizer + verifier + target-capability lowering understand.
                auto kids = hir.children(node);
                std::vector<MirInstId> operands;
                operands.reserve(kids.size());
                for (HirNodeId argN : kids) {
                    MirInstId const arg = lowerExpr(argN);
                    if (!arg.valid()) return InvalidMirInst;
                    operands.push_back(arg);
                }
                switch (static_cast<BuiltinLowering>(hir.payload(node))) {
                    case BuiltinLowering::UMulHigh:
                        return mir.addInst(MirOpcode::UMulH, operands, t);
                    // FC17.9(b) (D-CSUBSET-BITCOUNT-INTRINSICS): the 3 bit-count
                    // primitives. `t` is the builtin's result type (I32, since GCC's
                    // __builtin_popcount/clz/ctz all return `int`); the single
                    // operand is U32 (…) / U64 (…ll) and the mir_to_lir lowering
                    // reads the OPERAND's width via mir.instType(operand) to pick the
                    // 32- vs 64-bit realization. The count (0..P, P≤64) has zero
                    // upper bits, so it reads correctly at the I32 result with no
                    // explicit Trunc (the ICmp→Bool narrowing precedent).
                    case BuiltinLowering::Popcount:
                        return mir.addInst(MirOpcode::Popcount, operands, t);
                    case BuiltinLowering::Clz:
                        return mir.addInst(MirOpcode::Clz, operands, t);
                    case BuiltinLowering::Ctz:
                        return mir.addInst(MirOpcode::Ctz, operands, t);
                    // FC17.9(b) C23 <stdbit.h> (D-FULLC-STDBIT): the 14 stdc_* ops
                    // COMPOSE the 3 primitives above + universal ALU verbs into the
                    // N3096 §7.18 formula — one shared, width-correct, single-eval,
                    // branchless emitter (emitStdbitOp) the 14 leaf lowerings route
                    // through (NO new MIR op; the operand is bound ONCE in operands).
                    case BuiltinLowering::StdcLeadingZeros:
                    case BuiltinLowering::StdcLeadingOnes:
                    case BuiltinLowering::StdcTrailingZeros:
                    case BuiltinLowering::StdcTrailingOnes:
                    case BuiltinLowering::StdcFirstLeadingZero:
                    case BuiltinLowering::StdcFirstLeadingOne:
                    case BuiltinLowering::StdcFirstTrailingZero:
                    case BuiltinLowering::StdcFirstTrailingOne:
                    case BuiltinLowering::StdcCountZeros:
                    case BuiltinLowering::StdcCountOnes:
                    case BuiltinLowering::StdcHasSingleBit:
                    case BuiltinLowering::StdcBitWidth:
                    case BuiltinLowering::StdcBitFloor:
                    case BuiltinLowering::StdcBitCeil: {
                        if (kids.size() != 1) {
                            unsupported(node,
                                "a stdc_* bit builtin expects exactly 1 argument");
                            return InvalidMirInst;
                        }
                        // The operand's width is read from its coerced HIR type
                        // (its builtin param core), not the MirBuilder value.
                        return emitStdbitOp(
                            static_cast<BuiltinLowering>(hir.payload(node)),
                            operands[0], hir.typeId(kids[0]), node, t);
                    }
                    case BuiltinLowering::AtomicCas: {
                        // c104: MIR AtomicCas is the UNIVERSAL CAS order
                        // [ptr, comparand(expected), newval(desired)] — the
                        // C11 atomic_compare_exchange / LLVM cmpxchg shape a
                        // future frontend would also target. The WIN32
                        // intrinsic this builtin binds spells its args
                        // (dest, EXCHANGE, comparand) — exchange BEFORE
                        // comparand — so THIS arm (the one place the Win32
                        // binding is defined) reorders [c0, c2, c1]. Passing
                        // the args through positionally silently INVERTS the
                        // CAS (the compare tests the new value, the store
                        // writes the comparand — the exit-26 corpus catch).
                        if (operands.size() != 3) {
                            unsupported(node, "AtomicCas expects exactly 3 args");
                            return InvalidMirInst;
                        }
                        std::array<MirInstId, 3> const casOrder{
                            operands[0], operands[2], operands[1]};
                        return mir.addInst(MirOpcode::AtomicCas, casOrder, t);
                    }
                    case BuiltinLowering::Barrier:
                        // c113 (D-CSUBSET-INTRINSIC-BARRIER): _ReadWriteBarrier —
                        // a 0-operand, void, side-effecting compiler fence
                        // (`operands` is empty; the builtin declares no params).
                        // Emits NO runtime instruction; the side-effect flag makes
                        // the CSE/LICM clobber walk forbid memory motion across it.
                        // R::None ⇒ InvalidType (the Store convention — supplying
                        // a result type is a MirBuilder fatal).
                        return mir.addInst(MirOpcode::CompilerBarrier, operands,
                                           InvalidType);
                    case BuiltinLowering::SehExceptionCode:
                        // c115 SEH: `_exception_code()` — a 0-operand value op
                        // (u32) reading the dispatch context. Position legality
                        // (filter expr / handler body only) was proven by
                        // HirVerifier::checkSehContext before lowering ran.
                        return mir.addInst(MirOpcode::SehExceptionCode, operands, t);
                    case BuiltinLowering::SehExceptionInfo:
                        // c115 SEH: `_exception_info()` — a 0-operand value op
                        // (void* → EXCEPTION_POINTERS), filter-expression only.
                        return mir.addInst(MirOpcode::SehExceptionInfo, operands, t);
                    case BuiltinLowering::None:
                        break;
                }
                unsupported(node, "BuiltinCall carries no valid lowering");
                return InvalidMirInst;
            }
            case HirKind::Ternary: {
                // children: [cond, thenExpr, elseExpr]. Lower as a diamond
                // CFG with a phi at the join — same shape as IfStmt but
                // value-producing. Each arm lowers its expression in its
                // own block, branches to the join, and the phi at the join
                // takes the two incoming values keyed by their predecessor
                // blocks.
                //
                // INVARIANT (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): a phi of
                // an AGGREGATE type is forbidden — the aggregate model is
                // MEMORY-based (no LIR aggregate-width SSA value, and a bit-field
                // packs only by read-modify-write into its allocation unit). An
                // aggregate-typed ternary is therefore NEVER lowered as a bare
                // rvalue here; every consumer reaches it by ADDRESS via
                // lowerLvalueAddress's Ternary arm (a CFG diamond into ONE common
                // slot — no phi), and the discard position routes through it too
                // (the ExprStmt chokepoint). Reaching here with an aggregate type
                // means a NEW consumer lowered an aggregate ternary by value
                // instead of by address — fail loud rather than silently
                // synthesizing a phi-of-aggregate (plus aggregate-width arm
                // Loads) that flows to LIR as a latent miscompile.
                {
                    // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` is memory-resident
                    // too — it MUST reach a by-address carrier, never a bare SSA value.
                    if (isMemoryResidentType(interner, t)) {
                        unsupported(node,
                            "internal: an aggregate-typed ternary must be lowered "
                            "into a slot by ADDRESS (lowerLvalueAddress — a CFG "
                            "diamond into one common slot), never as a bare SSA "
                            "rvalue — the MIR has no aggregate-width value and a "
                            "phi-of-aggregate cannot pack bit-fields "
                            "(D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR)");
                        return InvalidMirInst;
                    }
                }
                auto kids = hir.children(node);
                if (kids.size() != 3) {
                    unsupported(node, "malformed Ternary (expect 3 children)");
                    return InvalidMirInst;
                }
                MirInstId const cond = lowerExpr(kids[0]);
                if (!cond.valid()) return InvalidMirInst;
                MirBlockId const thenBB = mir.createBlock(StructCfMarker::IfThen);
                MirBlockId const elseBB = mir.createBlock(StructCfMarker::IfElse);
                MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
                mir.addCondBr(cond, thenBB, elseBB);

                mir.beginBlock(thenBB);
                MirInstId const thenVal = lowerExpr(kids[1]);
                if (!thenVal.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(elseBB);
                    sealCreatedAsUnreachable(joinBB);
                    return InvalidMirInst;
                }
                MirBlockId const thenPred = mir.currentlyOpenBlock();
                mir.addBr(joinBB);

                mir.beginBlock(elseBB);
                MirInstId const elseVal = lowerExpr(kids[2]);
                if (!elseVal.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(joinBB);
                    return InvalidMirInst;
                }
                MirBlockId const elsePred = mir.currentlyOpenBlock();
                mir.addBr(joinBB);

                mir.beginBlock(joinBB);
                std::array<MirPhiIncoming, 2> incomings{
                    MirPhiIncoming{thenVal, thenPred},
                    MirPhiIncoming{elseVal, elsePred},
                };
                return mir.addPhi(t, incomings);
            }
            case HirKind::LogicalAnd:
            case HirKind::LogicalOr: {
                // Short-circuit lowering: lhs is evaluated in the current
                // block; if (LogicalAnd && !lhs) OR (LogicalOr && lhs) we
                // short-circuit to the join with lhs's value; otherwise we
                // evaluate rhs in a new block and join with its value. The
                // join's phi takes [lhs (short-circuit block), rhs (rhs block)].
                auto kids = hir.children(node);
                if (kids.size() != 2) {
                    unsupported(node, "malformed LogicalAnd/Or (expect 2 children)");
                    return InvalidMirInst;
                }
                bool const isAnd = (k == HirKind::LogicalAnd);
                MirInstId const lhs = lowerExpr(kids[0]);
                if (!lhs.valid()) return InvalidMirInst;
                MirBlockId const lhsPred = mir.currentlyOpenBlock();
                // FC3.5 sweep-c1 (chip task_bd58aa3d): the rhs block is
                // the CONDITIONAL ARM of this one-armed diamond — mark
                // it IfThen, exactly like IfStmt's else-less shape
                // (CondBr(cond, arm, join)). The previous `Linear`
                // marker left the IfJoin UNPAIRED, so any function
                // mixing `&&`/`||` with an `if`/ternary tripped the
                // verifier's count-pairing invariant (IfThen-count !=
                // IfJoin-count → I_StructCfMismatch; `if (a < 2 &&
                // a < 3)` = IfThen 1 vs IfJoin 2). For OR the arm sits
                // on the FALSE edge — the marker means "the diamond's
                // single conditional arm", not an edge polarity (the
                // verifier pairs counts; Ternary set the value-level-
                // diamond precedent for the If-family markers).
                MirBlockId const rhsBB   = mir.createBlock(StructCfMarker::IfThen);
                MirBlockId const joinBB  = mir.createBlock(StructCfMarker::IfJoin);
                // AND: lhs true → rhs, lhs false → join (short-circuit).
                // OR:  lhs true → join (short-circuit), lhs false → rhs.
                mir.addCondBr(lhs, isAnd ? rhsBB : joinBB,
                                   isAnd ? joinBB : rhsBB);

                mir.beginBlock(rhsBB);
                MirInstId const rhs = lowerExpr(kids[1]);
                if (!rhs.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(joinBB);
                    return InvalidMirInst;
                }
                MirBlockId const rhsPred = mir.currentlyOpenBlock();
                mir.addBr(joinBB);

                mir.beginBlock(joinBB);
                std::array<MirPhiIncoming, 2> incomings{
                    MirPhiIncoming{lhs, lhsPred},
                    MirPhiIncoming{rhs, rhsPred},
                };
                return mir.addPhi(t, incomings);
            }
            case HirKind::BinaryOp: {
                std::uint32_t const payload = hir.payload(node);
                if (!isCoreOp(payload)) {
                    unsupported(node, "extension BinaryOp (post-v1)");
                    return InvalidMirInst;
                }
                auto kids = hir.children(node);
                if (kids.size() != 2) {
                    unsupported(node, "malformed BinaryOp (verifier should "
                                       "have flagged this)");
                    return InvalidMirInst;
                }
                // LHS then RHS — two SEQUENTIAL statements (not function-call
                // arguments), so left-to-right and platform-independent.
                MirInstId const lhs = lowerExpr(kids[0]);
                MirInstId const rhs = lowerExpr(kids[1]);
                if (!lhs.valid() || !rhs.valid()) return InvalidMirInst;
                return combineBinaryOp(node, lhs, rhs);
            }
            case HirKind::AddressOf: {
                // children: [lvalue-operand]. The address of any supported
                // lvalue IS the pointer that `lowerLvalueAddress` produces;
                // factor through that helper so the two paths stay in sync
                // (cycle 3c added MemberAccess + Index lvalues — `&p.x` and
                // `&arr[i]` work because of that single delegation).
                auto kids = hir.children(node);
                if (kids.size() != 1) {
                    unsupported(node, "malformed AddressOf (expect 1 operand)");
                    return InvalidMirInst;
                }
                return lowerLvalueAddress(kids[0]);
            }
            case HirKind::LabelAddressOf: {
                // D-CSUBSET-COMPUTED-GOTO: `&&label` — materialize the target
                // label block's runtime address as a value. The payload is the
                // label's per-function ordinal; map it to the label's MIR block
                // (creating it forward if not yet emitted — getOrCreateLabelBlock
                // is the same map GotoStmt resolves through). Emit BlockAddress(b),
                // typed as the node's pointer type (void*). The mere existence of
                // this BlockAddress is ALSO what marks `b` address-taken
                // (Mir::isBlockAddressTaken), so opt + codegen see it.
                std::uint32_t const ordinal = hir.labelAddressOrdinal(node);
                MirBlockId const target = getOrCreateLabelBlock(ordinal);
                return mir.addBlockAddress(target, t);
            }
            case HirKind::Deref: {
                // children: [pointer]. Lower the pointer expression, then
                // emit `Load(ptr)` with the HIR node's type as the result
                // (which is the pointee — HIR already resolved it).
                auto kids = hir.children(node);
                if (kids.size() != 1) {
                    unsupported(node, "malformed Deref (expect 1 operand)");
                    return InvalidMirInst;
                }
                MirInstId const ptr = lowerExpr(kids[0]);
                if (!ptr.valid()) return InvalidMirInst;
                return combineDeref(node, ptr);
            }
            case HirKind::MemberAccess:
            case HirKind::Index: {
                // Rvalue read of an lvalue: compute the field/element
                // address via the shared lvalue path, then emit `Load`.
                MirInstId const ptr = lowerLvalueAddress(node);
                if (!ptr.valid()) return InvalidMirInst;
                // c91 (D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY): an
                // ARRAY-typed member/element rvalue (`p->yystk0`,
                // `s.in.arr`, an `a2d[i]` row) decays to its first
                // element's ADDRESS (C 6.3.2.1p3) — NEVER a Load (the c63
                // rule, applied uniformly: there is no aggregate-width
                // register value). Pre-c91 this arm loaded the array's
                // first bytes as the "value": sqlite sqlite3ParserFinalize
                // `pParser->yystack != pParser->yystk0` compared yystack
                // against yystk0's CONTENT → always unequal → YYFREE'd the
                // on-stack parser → glibc `free(): invalid pointer` SIGABRT
                // on EVERY SQL statement (the c91 wall). The HIR decay
                // funnel (coerce Cast<Array→Ptr>) covers every KNOWN
                // consumer; this arm keeps a no-Cast same-type context (a
                // struct-member `va_list` forwarded to a `va_list` param)
                // and any FUTURE consumer gap a correct decay instead of a
                // silent content-compare. A bit-field is never array-typed,
                // so this precedes the bit-field unit-load untouched.
                if (t.valid() && interner.kind(t) == TypeKind::Array) {
                    auto const elems = interner.operands(t);
                    if (elems.empty() || !elems[0].valid()) {
                        unsupported(node,
                            "array-typed member/index rvalue has no element "
                            "type (interner invariant violated)");
                        return InvalidMirInst;
                    }
                    std::array<MirInstId, 2> gep{ptr, constInt(0)};
                    return mir.addInst(MirOpcode::Gep, gep,
                                       interner.pointer(elems[0]));
                }
                // FC8 D-CSUBSET-BITFIELD: a bit-field read loads the whole
                // allocation unit (the Gep already targets the unit), then
                // extracts the field's bits (shift + mask / sign-extend).
                // c21/c27: an rvalue read of a `volatile` struct/union MEMBER or a
                // `volatile`-element INDEX is volatile. c21 carries an
                // object-volatile member via the node's VolatileAttr; c27 carries
                // a volatile ELEMENT/FIELD type via `t` (the accessed value type —
                // e.g. `va[i]` where `va`'s element is VolatileQual, or a member
                // whose field type is top-level VolatileQual). OR both so neither
                // form is missed (a missed flag = silent miscompile).
                MirInstFlags const vf =
                    volatileFlagFor(node) | volatileFlagForType(t);
                if (BitFieldPlacement const* bf = bitfieldPlacementOf(node)) {
                    // Bit-field read-modify-write of the allocation unit stays a
                    // plain Load (an `_Atomic` bit-field is not a supported form;
                    // were `t` ever atomic here the verifier belt fails loud).
                    std::array<MirInstId, 1> lo{ptr};
                    MirInstId const unit = mir.addInst(MirOpcode::Load, lo, t,
                                                       /*payload=*/0, vf);
                    if (!unit.valid()) return InvalidMirInst;
                    return emitBitfieldExtract(unit, *bf, t);
                }
                // FC17.9(d): an `_Atomic` member/element read becomes AtomicLoad.
                std::array<MirInstId, 1> ops{ptr};
                return emitScalarLoad(ops, t, node);
            }
            case HirKind::Cast: {
                // children: [operand]. The HIR node's typeId is the target
                // type; the operand's typeId is the source. Pick the MIR
                // cast opcode from (sourceKind, targetKind). HIR has already
                // validated the cast is well-typed; the verifier rejects
                // illegal lattice transitions before we reach here.
                auto kids = hir.children(node);
                if (kids.size() != 1) {
                    unsupported(node, "malformed Cast (expect 1 operand)");
                    return InvalidMirInst;
                }
                TypeId const fromTy = hir.typeId(kids[0]);
                TypeKind const fromK = fromTy.valid()
                    ? interner.kind(fromTy) : TypeKind::Void;
                TypeKind const toK   = t.valid()
                    ? interner.kind(t) : TypeKind::Void;
                // D-LK4-RODATA-PRODUCER-STRING closure (2026-06-02):
                // C-standard array-to-pointer decay. CST→HIR's coerce()
                // emits a synthetic `Cast` HIR node when an Array<T,N>
                // rvalue reaches a Ptr<T> context. Lower it BEFORE the
                // general mapCast() path so the operand's array-typed
                // SSA value never materializes (it would have no encoder
                // path: an aggregate Const cannot land in a single
                // register). Cycle scope: string literals only —
                // synthesize a MirGlobal carrying the string bytes +
                // emit GlobalAddr returning the Ptr. Local-array decay
                // (`int arr[10]; foo(arr);`) is anchored as
                // D-LK4-RODATA-PRODUCER-LOCAL-ARRAY-DECAY (needs
                // `AddressOf(Alloca) + Gep(0)` lowering); non-string
                // module-scope arrays anchored as
                // D-LK4-RODATA-PRODUCER-LOCAL-ARRAY-DECAY.
                // Both fail loud here so a future producer can't slip
                // a silent miscompile through this arm.
                if (fromK == TypeKind::Array && toK == TypeKind::Ptr) {
                    // FC8: NON-string-literal array→pointer DECAY (C 6.3.2.1) —
                    // the decayed pointer is the address of the array's FIRST
                    // ELEMENT. Any array LVALUE (a local alloca, a module global
                    // whose bytes the aggregate-global producer already emits to
                    // rodata, a struct field, an indexed sub-array) yields its
                    // base address via `lowerLvalueAddress`; a byte-offset-0 Gep
                    // re-types it as `Ptr<elem>` (`t`) — the C-standard `&arr[0]`
                    // (array base == first-element address). Closes
                    // D-LK4-RODATA-PRODUCER-LOCAL-ARRAY-DECAY +
                    // D-LK4-RODATA-PRODUCER-NONSTRING-GLOBAL-ARRAY-DECAY. A string
                    // literal is an RVALUE (no lvalue address) → it keeps the
                    // synthesize-a-rodata-global path below.
                    if (hir.kind(kids[0]) != HirKind::Literal) {
                        MirInstId const arrAddr = lowerLvalueAddress(kids[0]);
                        if (!arrAddr.valid()) return InvalidMirInst;
                        MirInstId const zero = constInt(0);
                        std::array<MirInstId, 2> gep{arrAddr, zero};
                        return mir.addInst(MirOpcode::Gep, gep, t);
                    }
                    // F5: the SINGLE rodata-string producer (shared with the
                    // lvalue-address `"abc"[i]` arm). `t` is the Cast's Ptr<Char>
                    // decay target. (Was inline here; factored so the value-decay
                    // and lvalue-index paths cannot drift.)
                    return materializeStringLiteralGlobal(kids[0], t);
                }
                MirInstId const operand = lowerExpr(kids[0]);
                if (!operand.valid()) return InvalidMirInst;
                return combineCast(node, operand);
            }
            case HirKind::SeqExpr: {
                // Lower the side-effect statements in order, then lower the
                // result expression. The result's value IS the SeqExpr's
                // value; the typeId on the SeqExpr equals the result's type.
                //
                // INVARIANT (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR): an
                // aggregate-typed comma/SeqExpr has no SSA rvalue (memory-based
                // model) — its consumers reach it by ADDRESS via
                // lowerLvalueAddress's SeqExpr arm (run the side effects, then
                // recurse to the result's lvalue), and the discard position
                // routes through it too. Reaching here with an aggregate type is
                // a misroute — fail loud rather than running the side effects and
                // then producing an aggregate-width rvalue of the result.
                {
                    // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` is memory-resident
                    // too — it MUST reach a by-address carrier, never a bare SSA value.
                    if (isMemoryResidentType(interner, t)) {
                        unsupported(node,
                            "internal: an aggregate-typed comma/SeqExpr must be "
                            "lowered by ADDRESS (lowerLvalueAddress), never as a "
                            "bare SSA rvalue (D-CSUBSET-AGGREGATE-VALUED-CONTROL-"
                            "EXPR)");
                        return InvalidMirInst;
                    }
                }
                for (HirNodeId stmt : hir.seqExprStmts(node)) {
                    if (!lowerStmt(stmt)) return InvalidMirInst;
                }
                return lowerExpr(hir.seqExprResult(node));
            }
            case HirKind::ConstructAggregate: {
                // INVARIANT (D-CSUBSET-BITFIELD-RVALUE-RUNTIME): the compiler's
                // aggregate model is MEMORY-based — there is no LIR aggregate-
                // width SSA value, and a bit-field can only be packed by a
                // read-modify-write into its allocation unit (impossible on an
                // SSA aggregate). So an aggregate `ConstructAggregate` is NEVER
                // lowered as a bare rvalue here; every consumer reaches it by
                // ADDRESS:
                //   - a LOCAL `= {…}` init   → lowerAggregateInitIntoSlot /
                //                               lowerArrayInitIntoSlot (VarDecl);
                //   - a GLOBAL `= {…}` init  → evaluateConstant + the static-data
                //                               byte encoder (emitGlobals_);
                //   - a by-value RVALUE      → lowerLvalueAddress's compound-
                //     (arg/return/assign/        literal arm materializes a slot,
                //      member-of-literal)        inits in place, returns its addr.
                // The former SSA-aggregate path (a `Const(zero)` seed + an
                // `InsertValue` chain) is removed: LIR realized only a zero-index
                // InsertValue and could not pack bit-fields — a latent silent
                // miscompile. Reaching here means a NEW consumer lowered an
                // aggregate ConstructAggregate as a bare rvalue instead of by
                // address — fail loud so it routes through the slot model rather
                // than silently resurrecting the SSA-aggregate chain.
                unsupported(node,
                    "internal: an aggregate compound-literal/initializer must be "
                    "lowered into a slot by ADDRESS (lowerLvalueAddress / "
                    "lowerAggregateInitIntoSlot), never as a bare SSA-aggregate "
                    "rvalue — the MIR has no aggregate-width value and cannot "
                    "pack bit-fields in a register (D-CSUBSET-BITFIELD-RVALUE-"
                    "RUNTIME)");
                return InvalidMirInst;
            }
            default: break;
        }
        unsupported(node,
            std::format("HIR expression kind ordinal {} not yet supported "
                        "(HIR id {})", static_cast<unsigned>(k), node.v));
        return InvalidMirInst;
    }

    // ── Plan 24 Stage 4 — straight-line expression-arm epilogues ───────────
    // Each `combine*` is the BYTE-IDENTICAL emission slice of a flattened arm
    // AFTER its child sub-expression(s) have been lowered (their MirInstId(s)
    // passed in). Shared by `lowerExprNode`'s recursive arms AND the driver's
    // frames below — ONE source of truth, so the iterative path emits the exact
    // same MIR (opcode order, operand identity, fail-loud sites) the recursive
    // path did. They emit ONLY into the currently-open block — no CFG.

    // UnaryOp epilogue (operand already lowered to `operand`). Reproduces the
    // recursive arm exactly: Neg→Neg/FNeg, BitNot→Not, logical Not→`cmp eq
    // operand, 0` (a zero Const THEN ICmpEq/FCmpOeq), else fail loud.
    [[nodiscard]] MirInstId combineUnary(HirNodeId node, MirInstId operand) {
        TypeId const   t  = hir.typeId(node);
        HirOpKind const op = decodeCoreOp(hir.payload(node));
        auto kids = hir.children(node);
        TypeId const operandType = hir.typeId(kids[0]);
        TypeKind const tk = operandType.valid()
            ? interner.kind(operandType) : TypeKind::Void;
        // C23 _BitInt(N) (D-CSUBSET-BITINT, CRIT-2): a unary op on a `_BitInt`.
        if (tk == TypeKind::BitInt) {
            // D-CSUBSET-BITINT-C2-WIDE (Model A divert): a WIDE operand arrives as an
            // ADDRESS (request flipped it). Only logical `!` (a Bool result) stays in
            // the value path — via the MS-first-free limb-OR, NOT bitIntToCompute.
            // Neg/BitNot produce a WIDE result → materialized by ADDRESS
            // (materializeWideUnaryOp); reaching here is a misroute → fail loud.
            if (isWideBitInt(interner, operandType)) {
                if (op == HirOpKind::Not)
                    return emitBoolFromWide(operand, operandType, /*zeroIsTrue=*/true);
                unsupported(node, std::format(
                    "internal: UnaryOp '{}' producing a wide _BitInt must be "
                    "materialized by ADDRESS (lowerLvalueAddress), never as a bare SSA "
                    "value (D-CSUBSET-BITINT-C2-WIDE)", opName(op)));
                return InvalidMirInst;
            }
            switch (op) {
                case HirOpKind::Neg: {
                    std::array<MirInstId, 1> a{operand};
                    return emitBitIntOp(MirOpcode::Neg, a, operandType,
                                        /*needsMask=*/true);
                }
                case HirOpKind::BitNot: {
                    // ~ flips the high (zero/sign) bits → unsigned needs re-masking;
                    // the mask is a no-op-safe belt for signed (sign-ext preserved).
                    std::array<MirInstId, 1> a{operand};
                    return emitBitIntOp(MirOpcode::Not, a, operandType,
                                        /*needsMask=*/true);
                }
                case HirOpKind::Not: {
                    // logical `!bitInt` ≡ `bitInt == 0`: promote to the compute width,
                    // compare against a compute-width zero (result Bool).
                    MirInstId const v = bitIntToCompute(operand, operandType);
                    if (!v.valid()) return InvalidMirInst;
                    TypeId const computeTy =
                        interner.primitive(bitIntComputeKind(operandType));
                    MirLiteralValue zero;
                    zero.value = std::int64_t{0};
                    zero.core  = interner.kind(computeTy);
                    MirInstId const z = mir.addConst(std::move(zero), computeTy);
                    std::array<MirInstId, 2> ops{v, z};
                    return mir.addInst(MirOpcode::ICmpEq, ops, t);   // t is Bool
                }
                default:
                    unsupported(node, std::format(
                        "UnaryOp '{}' on _BitInt not yet supported", opName(op)));
                    return InvalidMirInst;
            }
        }
        bool const isFloat = (tk == TypeKind::F16 || tk == TypeKind::F32
                           || tk == TypeKind::F64 || tk == TypeKind::F128);
        MirOpcode mop = MirOpcode::Invalid;
        switch (op) {
            case HirOpKind::Neg:    mop = isFloat ? MirOpcode::FNeg : MirOpcode::Neg; break;
            case HirOpKind::BitNot: mop = MirOpcode::Not; break;
            case HirOpKind::Not: {
                // Logical not: MIR has no dedicated opcode. Lower as
                // `cmp eq operand, 0`. Policy-neutral on Bool-vs-I1
                // — any `==` already produces whatever type the
                // result-type rule says, so this is symmetric with
                // the cycle-1 BinaryOp Eq path. (Review I-5)
                MirLiteralValue zero;
                if (isFloat) { zero.value = 0.0; }
                else { zero.value = std::int64_t{0}; }
                zero.core = tk;
                MirInstId const zeroConst = mir.addConst(std::move(zero),
                                                          operandType);
                std::array<MirInstId, 2> ops2{operand, zeroConst};
                return mir.addInst(
                    isFloat ? MirOpcode::FCmpOeq : MirOpcode::ICmpEq,
                    ops2, t);
            }
            default:
                unsupported(node,
                    std::format("UnaryOp '{}' not yet supported",
                                opName(op)));
                return InvalidMirInst;
        }
        std::array<MirInstId, 1> operands{operand};
        return mir.addInst(mop, operands, t);
    }

    // BinaryOp epilogue (lhs+rhs already lowered, IN THAT ORDER). The operand
    // TypeKind (from kids[0]) drives the signed/unsigned/float opcode choice.
    [[nodiscard]] MirInstId combineBinaryOp(HirNodeId node, MirInstId lhs,
                                            MirInstId rhs) {
        TypeId const   t  = hir.typeId(node);
        HirOpKind const op = decodeCoreOp(hir.payload(node));
        auto kids = hir.children(node);
        TypeId const operandType = hir.typeId(kids[0]);
        TypeKind const tk = operandType.valid()
            ? interner.kind(operandType) : TypeKind::Void;
        // c40 (D-CSUBSET-POINTER-SUBTRACTION) C 6.5.6p9: the HIR tier types
        // `p - q` (both Ptr<T>) as I64 (ptrdiff_t) while the OPERANDS stay Ptr —
        // that (op==Sub, operand kind Ptr, RESULT kind I64) is the signal to
        // lower as a SIGNED element-count difference: PtrToInt both sides, Sub
        // (the byte difference), then — unless sizeof(pointee)==1 — SDiv by the
        // element stride. Signed throughout (q>p ⇒ negative). `p ± n` (Ptr
        // result, deferred c41) is NOT this path — its result kind is Ptr, not I64.
        if (op == HirOpKind::Sub && tk == TypeKind::Ptr
            && t.valid() && interner.kind(t) == TypeKind::I64) {
            TypeId const i64ty = interner.primitive(TypeKind::I64);
            std::array<MirInstId, 1> la{lhs};
            std::array<MirInstId, 1> ra{rhs};
            MirInstId const li = mir.addInst(MirOpcode::PtrToInt, la, i64ty);
            MirInstId const ri = mir.addInst(MirOpcode::PtrToInt, ra, i64ty);
            std::array<MirInstId, 2> sa{li, ri};
            MirInstId const diff = mir.addInst(MirOpcode::Sub, sa, i64ty);
            TypeId const pointee = interner.operands(operandType)[0];
            auto const stride = elementStride(pointee);
            if (!stride.has_value()) {
                unsupported(node, std::format(
                    "pointer subtraction: pointee TypeKind {} has no computable "
                    "element stride (incomplete/void pointee)",
                    static_cast<unsigned>(interner.kind(pointee))));
                return InvalidMirInst;
            }
            if (*stride <= 1) return diff;   // 1-byte pointee: byte diff == element count
            MirInstId const sc =
                constIntOfType(static_cast<std::int64_t>(*stride), i64ty);
            std::array<MirInstId, 2> da{diff, sc};
            return mir.addInst(MirOpcode::SDiv, da, i64ty);
        }
        // c41 (D-CSUBSET-POINTER-INT-ARITHMETIC) C 6.5.6p8: `p ± n` → a new
        // pointer at byte offset n*sizeof(*p). cst_to_hir canonicalizes `n + p`
        // so kids[0] is ALWAYS the Ptr operand. Fires when the RESULT type is Ptr
        // (NOT I64 — that is c40's p-q) AND kids[0] is Ptr. Add: scale n, Gep(p,
        // scaled). Sub (p - n): Neg n, scale, Gep(p, -scaled). Reuses F1's
        // scaleIndexToBytes (the SAME stride machinery p[i]/p++ use → stride 1
        // emits no Mul, no double-scaling). `n - p` does not reach here (kids[0]
        // stays Int, not Ptr). void*/fn-ptr pointee → fail loud (no stride).
        if ((op == HirOpKind::Add || op == HirOpKind::Sub)
            && t.valid() && interner.kind(t) == TypeKind::Ptr
            && operandType.valid() && tk == TypeKind::Ptr) {
            auto const ptOps = interner.operands(operandType);
            if (ptOps.empty()) {
                unsupported(node, "pointer-integer arithmetic: Ptr has no pointee");
                return InvalidMirInst;
            }
            TypeId const pointee = ptOps[0];
            TypeId const indexTy = hir.typeId(kids[1]);
            if (!indexTy.valid()) {
                unsupported(node,
                    "pointer-integer arithmetic: integer operand has no type");
                return InvalidMirInst;
            }
            // Widen the index to POINTER width (I64) BEFORE Neg/scale: x86-64
            // 32-bit ops zero-extend into the 64-bit register, so a 32-bit
            // NEGATIVE byte offset (from `p - n`) would become a huge positive
            // address → access violation. SExt signed / ZExt unsigned (mapCast)
            // — the SAME widen the Index path (p[i]) already does (the positive
            // p+n cases worked only because their high bits were 0).
            TypeId const i64ty = interner.primitive(TypeKind::I64);
            MirInstId intIdx = rhs;
            if (interner.kind(indexTy) != TypeKind::I64) {
                MirOpcode const ext = mapCast(interner.kind(indexTy), TypeKind::I64);
                // c65: a non-integer index kind (Array/Enum/…) has no widening
                // cast → mapCast returns Invalid. FAIL LOUD here — passing Invalid
                // to addInst std::abort()s the whole compiler (the c65 sqlite
                // crash class: `p - arrayName`, now fixed at the HIR tier by array
                // decay → ptrSub; an Enum index is the deferred
                // D-CSUBSET-POINTER-ARITH-ENUM-INDEX). The pre-existing
                // `.valid()` guard below runs AFTER addInst, i.e. too late.
                if (ext == MirOpcode::Invalid) {
                    unsupported(node, std::format(
                        "pointer arithmetic: index TypeKind {} has no widening "
                        "cast to a 64-bit offset (an array index must decay to a "
                        "pointer difference; an enum index is deferred — "
                        "D-CSUBSET-POINTER-ARITH-ENUM-INDEX)",
                        static_cast<unsigned>(interner.kind(indexTy))));
                    return InvalidMirInst;
                }
                std::array<MirInstId, 1> eo{rhs};
                intIdx = mir.addInst(ext, eo, i64ty);
                if (!intIdx.valid()) return InvalidMirInst;
            }
            if (op == HirOpKind::Sub) {
                std::array<MirInstId, 1> ni{intIdx};
                intIdx = mir.addInst(MirOpcode::Neg, ni, i64ty);
                if (!intIdx.valid()) return InvalidMirInst;
            }
            MirInstId const byteOff =
                scaleIndexToBytes(intIdx, pointee, node, i64ty);
            if (!byteOff.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> gepOps{lhs, byteOff};
            return mir.addInst(MirOpcode::Gep, gepOps, t);
        }
        // C23 _BitInt(N) (D-CSUBSET-BITINT, CRIT-2): both operands are `_BitInt(N)`
        // (a `_BitInt op int` was coerced to a standard int by the UAC, so it never
        // reaches here with a BitInt operand). Route through the wrap chokepoint: the
        // container kind picks the signed/unsigned opcode (SDiv/UDiv/AShr/LShr), a
        // comparison promotes its operands + emits the ICmp typed Bool, and an
        // arithmetic op masks its result to N (Add/Sub/Mul/Shl) or not (Div/Mod/And/
        // Or/Xor/right-shift — clean-N-extension-preserving).
        if (tk == TypeKind::BitInt) {
            // D-CSUBSET-BITINT-C2-WIDE (Model A divert): WIDE operands arrive as
            // ADDRESSES (request flipped them). Only a COMPARISON (a Bool result)
            // stays in the value path — via the limb-OR (== !=) / MS-limb-first scan
            // (< <= > >=), NOT bitIntToCompute. Arithmetic/bitwise/shift produce a
            // WIDE result → materialized by ADDRESS (materializeWideBinaryOp);
            // reaching here with one is a misroute → fail loud (never a silent scalar
            // op that would truncate the value to its low limb).
            if (isWideBitInt(interner, operandType)) {
                switch (op) {
                    case HirOpKind::Eq: return emitWideEq(false, lhs, rhs, operandType);
                    case HirOpKind::Ne: return emitWideEq(true,  lhs, rhs, operandType);
                    case HirOpKind::Lt: case HirOpKind::Le:
                    case HirOpKind::Gt: case HirOpKind::Ge:
                        return emitWideOrder(op, lhs, rhs, operandType);
                    default:
                        unsupported(node, std::format(
                            "internal: BinaryOp '{}' producing a wide _BitInt must be "
                            "materialized by ADDRESS (lowerLvalueAddress), never as a "
                            "bare SSA value (D-CSUBSET-BITINT-C2-WIDE)", opName(op)));
                        return InvalidMirInst;
                }
            }
            TypeKind const container = interner.bitIntContainerKind(operandType);
            MirOpcode const bop = mapBinaryOp(op, container);
            if (bop == MirOpcode::Invalid) {
                unsupported(node, std::format(
                    "BinaryOp '{}' on _BitInt not yet supported", opName(op)));
                return InvalidMirInst;
            }
            if (isIntCompareOpcode(bop)) {
                MirInstId const l = bitIntToCompute(lhs, operandType);
                MirInstId const r = bitIntToCompute(rhs, operandType);
                if (!l.valid() || !r.valid()) return InvalidMirInst;
                std::array<MirInstId, 2> cops{l, r};
                return mir.addInst(bop, cops, t);   // t is Bool
            }
            bool const needsMask =
                bop == MirOpcode::Add || bop == MirOpcode::Sub
                || bop == MirOpcode::Mul || bop == MirOpcode::Shl;
            std::array<MirInstId, 2> bops{lhs, rhs};
            return emitBitIntOp(bop, bops, t, needsMask);
        }
        MirOpcode const mop = mapBinaryOp(op, tk);
        if (mop == MirOpcode::Invalid) {
            unsupported(node,
                std::format("BinaryOp '{}' on TypeKind {} not yet "
                            "supported", opName(op),
                            static_cast<unsigned>(tk)));
            return InvalidMirInst;
        }
        std::array<MirInstId, 2> operands{lhs, rhs};
        return mir.addInst(mop, operands, t);
    }

    // Deref epilogue (pointer already lowered to `ptr`): `Load(ptr)` typed as
    // the node's (pointee) type.
    //
    // c27 (D-CSUBSET-VOLATILE-POINTEE): if the pointee type is `volatile`-
    // qualified (the pointer was `Ptr<VolatileQual(T)>`, i.e. a `volatile T *`),
    // the Load is VOLATILE — the optimizer must not elide/cache/reorder it. The
    // pointee type IS this node's type. OR the type-derived flag with any
    // symbol-level c21 flag on the deref node (a deref node itself is never a
    // c21-flagged Ref, but ORing keeps the funnel uniform).
    [[nodiscard]] MirInstId combineDeref(HirNodeId node, MirInstId ptr) {
        TypeId const pointeeTy = hir.typeId(node);
        // c91 (D-CSUBSET-ARRAY-DECAY-POINTER-IDENTITY): `*p` where the
        // pointee is an ARRAY (`p : Ptr<Array<T,N>>`) — the deref VALUE is
        // an array rvalue, which decays to the address of its first element
        // (C 6.3.2.1p3): `*p` ≡ `&(*p)[0]` (numerically p itself, re-typed
        // `Ptr<elem>`). NEVER a Load — the same c63 rule as the Ref /
        // member / index rvalue arms (there is no aggregate-width register
        // value; the pre-c91 Load read the array's first bytes as the
        // "value"). Volatility is a property of the (non-)ACCESS: the decay
        // performs no memory access, so no volatile flag applies.
        if (pointeeTy.valid()
            && interner.kind(pointeeTy) == TypeKind::Array) {
            auto const elems = interner.operands(pointeeTy);
            if (elems.empty() || !elems[0].valid()) {
                unsupported(node,
                    "array-typed Deref rvalue has no element type "
                    "(interner invariant violated)");
                return InvalidMirInst;
            }
            std::array<MirInstId, 2> gep{ptr, constInt(0)};
            return mir.addInst(MirOpcode::Gep, gep,
                               interner.pointer(elems[0]));
        }
        // FC17.9(d): a deref of a `_Atomic`-pointee pointer (`*p`, `p[i]`) becomes
        // AtomicLoad via the scalar-access chokepoint (else the exact c21|c27 flag).
        std::array<MirInstId, 1> ops{ptr};
        return emitScalarLoad(ops, pointeeTy, node);
    }

    // Scalar-Cast epilogue (operand already lowered): the array→pointer DECAY
    // sub-cases are NOT routed here (they delegate in `lowerExprNode` /
    // `castFlattens` returns false), so this is purely the `mapCast` opcode +
    // single-operand emit. Enum operands cast AS their underlying integer.
    [[nodiscard]] MirInstId combineCast(HirNodeId node, MirInstId operand) {
        TypeId const t = hir.typeId(node);
        auto kids = hir.children(node);
        TypeId const fromTy = hir.typeId(kids[0]);
        TypeKind const fromK = fromTy.valid()
            ? interner.kind(fromTy) : TypeKind::Void;
        TypeKind const toK = t.valid() ? interner.kind(t) : TypeKind::Void;
        // C 6.7.2.2: an enum casts AS its underlying integer (the kind in
        // the enum TypeId's `scalars[0]`). Resolve here so `mapCast`
        // (TypeKind-only, can't read the interner) sees the real width —
        // NO I32 assumption: a non-I32-underlying enum lowers via its
        // declared kind. The Cast's RESULT type stays `t` (enum-typed for
        // an int→enum cast). D-CSUBSET-ENUM-INT-CONVERSION.
        auto const enumUnderlying =
            [&](TypeId ty, TypeKind kk) noexcept -> TypeKind {
                if (kk != TypeKind::Enum || !ty.valid()) return kk;
                auto const sc = interner.scalars(ty);
                return sc.empty() ? kk : static_cast<TypeKind>(sc[0]);
            };
        // D-CSUBSET-BITINT-C2-WIDE (Model A divert): a cast involving a WIDE `_BitInt`.
        // A wide TARGET makes the whole Cast a wide result → materialized by ADDRESS
        // (materializeWideCast); reaching the value path is a misroute → fail loud. A
        // wide SOURCE with a scalar target reaches here with `operand` = the ADDRESS of
        // the wide value (request flipped it): produce the low bits (C 6.3.1.3) — bool
        // (nonzero-test over all limbs), a narrow `_BitInt` (limb 0 masked to its N),
        // or a plain scalar (limb 0 truncated). Placed BEFORE any `bitIntContainerKind`
        // query (fatal for N>64).
        if (isWideBitInt(interner, t)) {
            unsupported(node, "internal: a cast producing a wide _BitInt must be "
                              "materialized by ADDRESS (lowerLvalueAddress), never as a "
                              "bare SSA value (D-CSUBSET-BITINT-C2-WIDE)");
            return InvalidMirInst;
        }
        if (isWideBitInt(interner, fromTy)) {
            // D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV: `wide _BitInt(N>64) -> float` is NOT
            // yet supported — emitScalarFromWide reads only limb 0, so everything above
            // 2^64 (and the true magnitude) is lost. Fail LOUD rather than silently
            // miscompile; the correct multi-limb limbs->FP conversion lands in a later
            // cycle. NARROW (N<=64) _BitInt->float rides the container and never gets here.
            if (isFloatingKind(toK)) {
                diagnoseCode(node, DiagnosticCode::S_BitIntWideFloatConvUnsupported,
                    "conversion from a `_BitInt` wider than 64 bits to a floating type is "
                    "not yet supported — the multi-limb bit-precise-to-float conversion "
                    "lands in a later cycle (D-CSUBSET-BITINT-FLOAT-CHAR-ENUM-CONV)");
                return InvalidMirInst;
            }
            if (toK == TypeKind::Bool)
                return emitBoolFromWide(operand, fromTy, /*zeroIsTrue=*/false);
            if (toK == TypeKind::BitInt)      // wide → NARROW _BitInt: low limb, masked
                return emitCastToBitInt(loadLimb(limbAddrConst(operand, 0)),
                                        TypeKind::I64, t);
            MirInstId const r = emitScalarFromWide(operand, enumUnderlying(t, toK));
            if (!r.valid()) {
                unsupported(node, std::format(
                    "cast from a wide _BitInt to TypeKind {} has no MIR opcode",
                    static_cast<unsigned>(toK)));
                return InvalidMirInst;
            }
            return r;
        }
        // C23 _BitInt(N) (D-CSUBSET-BITINT): a BitInt SOURCE casts AS its native
        // container (I8/I16/I32/I64 — the enum→underlying twin), so a `_BitInt→int`
        // is the ordinary container→target `mapCast`.
        auto const resolveScalarKind =
            [&](TypeId ty, TypeKind kk) -> TypeKind {
                if (kk == TypeKind::BitInt && ty.valid())
                    return interner.bitIntContainerKind(ty);
                return enumUnderlying(ty, kk);
            };
        TypeKind const fromResolved = resolveScalarKind(fromTy, fromK);
        // ★ CRIT-1: a cast TO `_BitInt(N)` masks UNCONDITIONALLY (even the same-
        // container `int→_BitInt(17)` case — a plain Bitcast would leave bits 17-31
        // dirty). Route through the dedicated masking path; `mapCast` stays
        // BYTE-IDENTICAL for its non-BitInt callers (M-7).
        if (toK == TypeKind::BitInt) {
            MirInstId const r = emitCastToBitInt(operand, fromResolved, t);
            if (!r.valid()) {
                unsupported(node, std::format(
                    "Cast from TypeKind {} to _BitInt has no MIR opcode",
                    static_cast<unsigned>(fromK)));
                return InvalidMirInst;
            }
            return r;
        }
        TypeKind const toResolved    = enumUnderlying(t, toK);
        MirOpcode const mop = mapCast(fromResolved, toResolved);
        if (mop == MirOpcode::Invalid) {
            unsupported(node, std::format(
                "Cast from TypeKind {} to {} has no MIR opcode",
                static_cast<unsigned>(fromK),
                static_cast<unsigned>(toK)));
            return InvalidMirInst;
        }
        // D-CSUBSET-INT-TO-F32-CODEGEN / si_to_fp sub-int source (c78): a
        // sub-int (Char/I8/U8/I16/U16) → float conversion has NO
        // sub-32-bit int→float instruction form on x86 (CVTSI2SD reads
        // r32/r64) OR arm64 (SCVTF reads Wn/Xn). gcc integer-PROMOTES the
        // source to `int` FIRST (`movsx/movzx ecx, cl` then `cvtsi2sd
        // xmm, ecx`; C 6.3.1.1). Insert the SAME promotion: widen the
        // source to I32 (SExt signed / ZExt unsigned via mapCast) before
        // the SIToFP/UIToFP, so the conversion reads a sign/zero-extended
        // 32-bit source. Non-sub-int sources (I32/I64 already ≥32) skip
        // this — byte-identical to the prior single-op emit.
        MirInstId castOperand = operand;
        if ((mop == MirOpcode::SIToFP || mop == MirOpcode::UIToFP)
            && isSubIntPromotable(fromResolved)) {
            MirOpcode const ext = mapCast(fromResolved, TypeKind::I32);
            if (ext == MirOpcode::Invalid) {
                unsupported(node, std::format(
                    "sub-int→float source promotion from TypeKind {} to I32 "
                    "has no MIR opcode",
                    static_cast<unsigned>(fromResolved)));
                return InvalidMirInst;
            }
            std::array<MirInstId, 1> extOps{operand};
            castOperand = mir.addInst(ext, extOps,
                                      interner.primitive(TypeKind::I32));
        }
        std::array<MirInstId, 1> ops{castOperand};
        return mir.addInst(mop, ops, t);
    }

    // True iff a `Cast` node lowers as a CLEAN single-operand scalar cast (the
    // form the driver flattens): exactly ONE child AND not an array→pointer
    // decay. Array→pointer decay (`fromK==Array && toK==Ptr`) is NOT a plain
    // `lowerExpr(operand)`-then-emit — it routes through `lowerLvalueAddress`
    // (the local/global-array arm) or mints a rodata global (the string-literal
    // arm), so it stays delegating (`lowerExprNode` keeps it intact).
    [[nodiscard]] bool castFlattens(HirNodeId node) const {
        auto kids = hir.children(node);
        if (kids.size() != 1) return false;            // malformed → delegate (fail loud)
        TypeId const fromTy = hir.typeId(kids[0]);
        TypeKind const fromK = fromTy.valid()
            ? interner.kind(fromTy) : TypeKind::Void;
        TypeId const t = hir.typeId(node);
        TypeKind const toK = t.valid() ? interner.kind(t) : TypeKind::Void;
        return !(fromK == TypeKind::Array && toK == TypeKind::Ptr);
    }

    // ── Plan 24 Stage 4-address — by-ADDRESS arm epilogues ─────────────────
    // The BYTE-IDENTICAL emission slice of a flattened lvalue-address arm AFTER
    // its base lvalue (and, for Index, its already-scaled byte index) is
    // resolved. Shared by `lowerLvalueAddressNode`'s recursive arms AND the
    // driver's Address frames — ONE source of truth, so the iterative path
    // emits the exact same MIR the recursive `lowerLvalueAddress` did. They emit
    // ONLY into the currently-open block — no CFG.

    // MemberAccess address epilogue (base lvalue already resolved to `basePtr`).
    // FC7 (D-FC7-MEMBER-ACCESS): resolve the field's BYTE OFFSET via the FC6
    // `computeLayout` engine and emit a 2-op base+disp GEP `[basePtr,
    // Const(byteOffset)]` (MIR→LIR → a base+disp `lea`). The aggregate TypeId is
    // the BASE child's HIR type (`s`→struct S; `*p`→struct S; `a.b`→struct B;
    // `arr[i]`→struct Elem) — robust across every nested/indexed base shape. The
    // GEP result type is `pointer(fieldType)`. Emission order matches the
    // recursive arm exactly: base chain (already in `basePtr`), THEN the offset
    // Const, THEN the Gep.
    [[nodiscard]] MirInstId combineMemberAddr(HirNodeId node, MirInstId basePtr) {
        auto kids = hir.children(node);
        std::uint32_t const fieldIdx = hir.payload(node);
        TypeId const fieldTy = hir.typeId(node);
        if (!fieldTy.valid()) {
            unsupported(node, "MemberAccess with invalid field type "
                               "(HIR verifier should have flagged)");
            return InvalidMirInst;
        }
        TypeId const aggTy = hir.typeId(kids[0]);
        if (!aggTy.valid()) {
            unsupported(node, "MemberAccess base has no type "
                               "(HIR verifier should have flagged)");
            return InvalidMirInst;
        }
        auto const byteOffset = fieldByteOffset(aggTy, fieldIdx);
        if (!byteOffset.has_value()) {
            unsupported(node, "MemberAccess field-offset resolution failed "
                               "(target lacks 'aggregateLayout', the aggregate "
                               "is un-sizeable, or the field index is out of "
                               "range)");
            return InvalidMirInst;
        }
        MirInstId const offK =
            constInt(static_cast<std::int64_t>(*byteOffset));
        if (!offK.valid()) return InvalidMirInst;
        std::array<MirInstId, 2> ops{basePtr, offK};
        return mir.addInst(MirOpcode::Gep, ops, interner.pointer(fieldTy));
    }

    // Index address epilogue (the byte-scaled index already in `byteIdx`, the
    // base pointer/storage-address already in `basePtr`). Both the pointer-base
    // and storage-base sub-cases of the recursive arm end here: the single 2-op
    // byte-offset Gep `[basePtr, byteIdx]` typed `pointer(elemTy)`. The elem
    // type is the Index node's own type; `resTy` is recomputed here (pure, no
    // emission) so the epilogue needs no extra plumbing.
    [[nodiscard]] MirInstId combineIndexAddr(HirNodeId node, MirInstId byteIdx,
                                             MirInstId basePtr) {
        TypeId const elemTy = hir.typeId(node);
        std::array<MirInstId, 2> ops{basePtr, byteIdx};
        return mir.addInst(MirOpcode::Gep, ops, interner.pointer(elemTy));
    }

    // ── Plan 24 Stage 4-cfg — control-flow value-arm guard slices ──────────
    // The fail-loud anti-resurrection guards lifted VERBATIM out of the
    // recursive Ternary / SeqExpr value arms, so the flattened CFG frames run
    // the EXACT SAME diagnostic at the EXACT SAME point (before any block is
    // minted). Returns true (and reports) iff the node's value type is an
    // aggregate — which must reach this carrier by ADDRESS, never as a bare SSA
    // rvalue (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR). False ⇒ proceed.

    [[nodiscard]] bool ternaryAggregateGuardFails(HirNodeId node) {
        TypeId const t = hir.typeId(node);
        {
            // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` is memory-resident too — it
            // MUST reach a by-address carrier, never a bare SSA value.
            if (isMemoryResidentType(interner, t)) {
                unsupported(node,
                    "internal: an aggregate-typed ternary must be lowered "
                    "into a slot by ADDRESS (lowerLvalueAddress — a CFG "
                    "diamond into one common slot), never as a bare SSA "
                    "rvalue — the MIR has no aggregate-width value and a "
                    "phi-of-aggregate cannot pack bit-fields "
                    "(D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR)");
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool seqExprAggregateGuardFails(HirNodeId node) {
        TypeId const t = hir.typeId(node);
        {
            // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` is memory-resident too — it
            // MUST reach a by-address carrier, never a bare SSA value.
            if (isMemoryResidentType(interner, t)) {
                unsupported(node,
                    "internal: an aggregate-typed comma/SeqExpr must be "
                    "lowered by ADDRESS (lowerLvalueAddress), never as a "
                    "bare SSA rvalue (D-CSUBSET-AGGREGATE-VALUED-CONTROL-"
                    "EXPR)");
                return true;
            }
        }
        return false;
    }

    // ── Plan 24 Stage 4 — the iterative value+address-lowering driver ──────
    // A POD work-stack frame for ONE flattened straight-line arm. `phase`
    // 0 = enter the (last) child; for a 2-child arm (BinaryOp) phase 1 = enter
    // the second child (after stashing the first in `c0`); the final phase
    // pops and `combine*`s into `result`. (UnaryOp/Deref/Cast are single-child:
    // phase 0 enters, phase 1 combines.) Mirrors the cst_to_hir `ExprFrame`
    // idiom (the Stage-1/2/3 realloc-safe rule: copy frame fields to locals and
    // advance `phase` BEFORE any `enterValue`/`push_back`; copy `result` out
    // before `pop_back`).
    // A frame requests its children as VALUEs or ADDRESSes by its KIND:
    //   Unary/Binary/Deref/Cast  → straight-line VALUE arms (Stage 4).
    //   MemberAddr/IndexAddr      → by-ADDRESS arms (Stage 4-address): the deep
    //                               base-chain axes `a.b.c.d` / `a[i][j][k]`.
    // `phase` sequences a frame's child requests; the final phase pops + emits
    // via the matching `combine*`. `c0` stashes ONE child result across the next
    // request (Binary: the LHS; IndexAddr: the byte-scaled index).
    struct ValueFrame {
        enum class Kind : std::uint8_t {
            Unary, Binary, Deref, Cast, MemberAddr, IndexAddr,
            // Stage 4-cfg: the control-flow VALUE arms. Each is a multi-phase
            // machine that INTERLEAVES createBlock/addCondBr/addBr/beginBlock
            // with its sub-expression requests; the block handles + first-arm
            // value/predecessor it minted are carried in the frame fields below
            // (NEVER in a dangling `work.back()` reference) across phases.
            Ternary, Logical, SeqExpr,
            // Plan 24 (hir_to_mir Call residual): the {Call, IntrinsicCall}
            // VALUE arms. A Call frame builds the callee (phase 0→1), runs the
            // pre-loop setup (phase 1), then PUMPS each argument — a struct arg
            // inline, a scalar arg through the work-stack (phase 2) — so a deep
            // `f(f(f(…)))` chain carries flat host-stack cost. The accumulating
            // per-call state lives in a `callCtxs` LIFO vector (a nested call's
            // arg grows it mid-pump → a held reference would dangle); the frame
            // references its ctx by the STABLE index `aux`.
            Call, IntrinsicCall
        } kind;
        HirNodeId node;
        std::uint8_t phase;
        MirInstId c0;
        std::uint32_t aux{};   // Call/IntrinsicCall: index into the local callCtxs
        // CFG-frame state, carried across phases (realloc-safe — they live in
        // the vector element, copied to locals before any push). Unused by the
        // straight-line/address kinds. `bb0/bb1/bb2` are the minted blocks
        // (Ternary: then/else/join; Logical: rhs/join, bb2 unused); `v0` is the
        // first-arm value (Ternary: thenVal; Logical: lhs); `pred0` is the
        // first-arm predecessor block (Ternary: thenPred; Logical: lhsPred).
        MirBlockId bb0{};
        MirBlockId bb1{};
        MirBlockId bb2{};
        MirInstId  v0{};
        MirBlockId pred0{};
    };

    // The shared {Value,Address} expression-lowering driver over an explicit
    // heap work-stack. `rootWantAddr` selects the ROOT request: false → the
    // node's VALUE (`lowerExpr`), true → its lvalue ADDRESS (`lowerLvalueAddress`).
    // `request(n, wantAddr)` either PUSHES a frame for a deep flattenable arm or
    // delegates to the matching per-node body (`lowerExprNode` / `lowerLvalue
    // AddressNode`), which lowers that one node and RE-ENTERS this driver for its
    // children. The two families share ONE work-stack + ONE `result` slot, so a
    // by-value read that needs a base address (MemberAccess/Index rvalue) and a
    // by-address chain that needs a value (an Index subscript, a pointer base)
    // flatten through each other. Output-identity: the flattened arms reproduce
    // the recursive child-lowering ORDER + `combine*` exactly, so the emitted MIR
    // (inst order, vreg ids, operands) is byte-identical to the recursive form.
    [[nodiscard]] MirInstId runExprDriver(HirNodeId node, bool rootWantAddr) {
        std::vector<ValueFrame> work;
        // Plan 24: the flattened Call/IntrinsicCall accumulators (see `CallLowerCtx`
        // + the ValueFrame::Kind::Call note). A LIFO stack parallel to `work`; a
        // Call frame references its ctx by the STABLE index `aux` (a scalar arg
        // that is itself a call grows this vector mid-pump, so the index — never a
        // held reference — is what survives).
        std::vector<CallLowerCtx> callCtxs;
        // Default-init: `request` ALWAYS assigns `result` for a delegated node,
        // and every pushed frame delivers into `result` before it is read (then
        // popped), so this sentinel never leaks.
        MirInstId result = InvalidMirInst;

        // Classify `n` under the requested kind: push a frame for a flattenable
        // deep arm (and return), else lower it via the per-node body (delegating;
        // its children re-enter this driver). A VALUE request flattens the four
        // straight-line value arms; an ADDRESS request flattens MemberAccess and
        // Index (the deep base axes). NOTE: a push MUST be the LAST action of any
        // caller path that has copied out its frame fields — `work.back()` may
        // dangle after.
        auto const request = [&](HirNodeId n, bool wantAddr) {
            HirKind const nk = hir.kind(n);
            // C23 _BitInt(N>64) (D-CSUBSET-BITINT-C2-WIDE): a wide `_BitInt` is
            // MEMORY-RESIDENT — it has NO SSA rvalue. A VALUE read of a wide-BitInt-
            // typed node yields the ADDRESS of its limbs (exactly as an array rvalue
            // decays to its element address), so flip the request to by-ADDRESS. This
            // is the ONE chokepoint that keeps wide arithmetic/convert out of the
            // scalar value path: a wide arithmetic node materializes into a slot
            // (lowerLvalueAddressNode's new arms), a wide operand of a scalar-
            // producing op (a compare / logical-! / wide→scalar cast) is delivered as
            // an address to `combine*`. Call/IntrinsicCall/BuiltinCall are EXEMPT —
            // their value path already yields the result slot address (finishCall),
            // and flipping would loop through the by-address Call arm's `lowerExpr`.
            if (!wantAddr && isWideBitInt(interner, hir.typeId(n))
                && nk != HirKind::Call && nk != HirKind::IntrinsicCall
                && nk != HirKind::BuiltinCall) {
                wantAddr = true;
            }
            if (wantAddr) {
                switch (nk) {
                    case HirKind::MemberAccess:
                        // The plain field-of-lvalue arm: its ONLY recursion is
                        // the base lvalue address (the deep axis). A malformed
                        // arity delegates (fail loud, byte-identical guard).
                        if (hir.children(n).size() == 1) {
                            work.push_back({.kind = ValueFrame::Kind::MemberAddr,
                                            .node = n, .phase = 0});
                            return;
                        }
                        break;
                    case HirKind::Index:
                        // `base[idx]`: subscript VALUE then base (storage ADDRESS
                        // or pointer VALUE) — both re-enter this driver.
                        if (hir.children(n).size() == 2) {
                            work.push_back({.kind = ValueFrame::Kind::IndexAddr,
                                            .node = n, .phase = 0});
                            return;
                        }
                        break;
                    default: break;
                }
                // Every OTHER lvalue arm (Ref/global, Deref, Call-sret, the
                // CFG/slot arms ConstructAggregate/Ternary/SeqExpr/VaArg) keeps
                // its recursive body; its own re-entries still flatten.
                result = lowerLvalueAddressNode(n);
                return;
            }
            switch (nk) {
                case HirKind::UnaryOp:
                    // A core UnaryOp with exactly one child flattens; an
                    // extension op or malformed arity delegates (fail loud
                    // there, byte-identical to the recursive guards).
                    if (isCoreOp(hir.payload(n)) && hir.children(n).size() == 1) {
                        work.push_back({.kind = ValueFrame::Kind::Unary,
                                        .node = n, .phase = 0});
                        return;
                    }
                    break;
                case HirKind::BinaryOp:
                    if (isCoreOp(hir.payload(n)) && hir.children(n).size() == 2) {
                        work.push_back({.kind = ValueFrame::Kind::Binary,
                                        .node = n, .phase = 0});
                        return;
                    }
                    break;
                case HirKind::Deref:
                    if (hir.children(n).size() == 1) {
                        work.push_back({.kind = ValueFrame::Kind::Deref,
                                        .node = n, .phase = 0});
                        return;
                    }
                    break;
                case HirKind::Cast:
                    if (castFlattens(n)) {
                        work.push_back({.kind = ValueFrame::Kind::Cast,
                                        .node = n, .phase = 0});
                        return;
                    }
                    break;
                // Stage 4-cfg: the control-flow VALUE arms flatten so a deeply-
                // nested `a?b:c?d:e…` / `a&&b&&c…` chain (and a comma chain's
                // result tail) carries flat host-stack cost. Each pushes a
                // multi-phase frame whose machine reproduces the recursive arm's
                // createBlock ORDER, branch successors, and addPhi predecessor
                // ORDER byte-for-byte (see the driver loop). A malformed arity
                // delegates to the recursive body (its own fail-loud guard).
                case HirKind::Ternary:
                    if (hir.children(n).size() == 3) {
                        work.push_back({.kind = ValueFrame::Kind::Ternary,
                                        .node = n, .phase = 0});
                        return;
                    }
                    break;
                case HirKind::LogicalAnd:
                case HirKind::LogicalOr:
                    if (hir.children(n).size() == 2) {
                        work.push_back({.kind = ValueFrame::Kind::Logical,
                                        .node = n, .phase = 0});
                        return;
                    }
                    break;
                case HirKind::SeqExpr:
                    // The side-effect statements lower via `lowerStmt` (a
                    // separate machine, kept recursive); only the RESULT-tail
                    // re-enters this driver, so a deep comma chain's result
                    // expression flattens. Push unconditionally (no arity gate —
                    // SeqExpr has no `children()` arity; it carries stmts+result
                    // accessors) and run the guard+stmts in phase 0.
                    work.push_back({.kind = ValueFrame::Kind::SeqExpr,
                                    .node = n, .phase = 0});
                    return;
                // Plan 24: a Call flattens its callee + each SCALAR argument
                // through a Call frame (a struct arg stays inline in the pump) so
                // a deep `f(f(f(…)))` chain carries flat host-stack cost. A
                // malformed Call (no callee child) keeps the recursive body's
                // fail-loud. IntrinsicCall is the all-scalar variant (no callee
                // child, no struct ABI). Each pushes its ctx (stable index `aux`).
                case HirKind::Call:
                    if (!hir.children(n).empty()) {
                        std::uint32_t const ctxIdx =
                            static_cast<std::uint32_t>(callCtxs.size());
                        callCtxs.push_back(CallLowerCtx{
                            .node = n, .resultTy = hir.typeId(n)});
                        work.push_back({.kind = ValueFrame::Kind::Call,
                                        .node = n, .phase = 0, .aux = ctxIdx});
                        return;   // phase 0 enters the callee
                    }
                    break;
                case HirKind::IntrinsicCall: {
                    std::uint32_t const ctxIdx =
                        static_cast<std::uint32_t>(callCtxs.size());
                    callCtxs.push_back(CallLowerCtx{
                        .node = n, .resultTy = hir.typeId(n), .isIntrinsic = true,
                        .intrinsicId = hir.payload(n)});
                    callCtxs.back().argIdx = 0;   // intrinsic args start at 0
                    work.push_back({.kind = ValueFrame::Kind::IntrinsicCall,
                                    .node = n, .phase = 0, .aux = ctxIdx});
                    return;
                }
                default: break;
            }
            result = lowerExprNode(n);   // delegate (terminal / CFG / by-address)
        };

        // Plan 24: the per-arg pump for a flattened Call (ctx at stable index
        // `ctxIdx`). Advances `processOneCallArg` until a SCALAR arg is reached
        // (struct args materialize inline there), which it routes through `request`
        // (the Call frame's phase 2 then collects it). Returns true iff it entered
        // a scalar arg's value-lowering (the caller must wait for it); false when
        // all args are consumed (the caller finishes the call). `request` (if
        // called) is the LAST action, so the dangling-`work.back()` rule holds.
        // Re-addresses `callCtxs[ctxIdx]` fresh each access — a scalar arg that is
        // itself a call grows `callCtxs`, so the INDEX is stable where a reference
        // would dangle. Returns -1 to signal a fail-loud (the caller aborts).
        auto const pumpCallArgs = [&](std::uint32_t ctxIdx) -> int {
            for (;;) {
                CallArgStep const step = processOneCallArg(callCtxs[ctxIdx]);
                if (step == CallArgStep::Error) return -1;          // fail-loud
                if (step == CallArgStep::Done) return 0;            // finish
                if (step == CallArgStep::StructDone) continue;      // next arg
                // ScalarPending: route the in-flight scalar arg's VALUE onto the
                // work-stack (phase 2 collects it). `argIdx` stays put so phase 2
                // derives the same arg child.
                HirNodeId const argN =
                    hir.children(callCtxs[ctxIdx].node)[callCtxs[ctxIdx].argIdx];
                request(argN, false);
                return 1;   // entered a scalar arg — wait
            }
        };

        request(node, rootWantAddr);
        while (!work.empty()) {
            ValueFrame& f = work.back();
            switch (f.kind) {
            case ValueFrame::Kind::Unary:
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const operandN = hir.children(f.node)[0];
                    request(operandN, false);  // build operand — may invalidate `f`
                } else {
                    HirNodeId const node2 = f.node;
                    MirInstId const operand = result;
                    work.pop_back();
                    result = operand.valid() ? combineUnary(node2, operand)
                                             : InvalidMirInst;
                }
                break;
            case ValueFrame::Kind::Binary:
                // LHS first (phase 0→1), then RHS (phase 1→2) — matching the
                // recursive `lhs = lowerExpr(kids[0]); rhs = lowerExpr(kids[1]);`
                // which are two SEQUENTIAL statements → left-to-right, NOT
                // function-call arguments (so platform-independent; this differs
                // from cst_to_hir's Binary frame, whose recursive form passed
                // operands as call args and thus built rhs-then-lhs).
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const lhsN = hir.children(f.node)[0];
                    request(lhsN, false);      // build LHS — may invalidate `f`
                } else if (f.phase == 1) {
                    f.c0 = result;          // LHS result
                    f.phase = 2;
                    HirNodeId const rhsN = hir.children(f.node)[1];
                    request(rhsN, false);      // build RHS — may invalidate `f`
                } else {
                    HirNodeId const node2 = f.node;
                    MirInstId const lhs = f.c0;
                    MirInstId const rhs = result;
                    work.pop_back();
                    result = (lhs.valid() && rhs.valid())
                                 ? combineBinaryOp(node2, lhs, rhs)
                                 : InvalidMirInst;
                }
                break;
            case ValueFrame::Kind::Deref:
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const ptrN = hir.children(f.node)[0];
                    request(ptrN, false);      // build pointer — may invalidate `f`
                } else {
                    HirNodeId const node2 = f.node;
                    MirInstId const ptr = result;
                    work.pop_back();
                    result = ptr.valid() ? combineDeref(node2, ptr)
                                         : InvalidMirInst;
                }
                break;
            case ValueFrame::Kind::Cast:
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const operandN = hir.children(f.node)[0];
                    request(operandN, false);  // build operand — may invalidate `f`
                } else {
                    HirNodeId const node2 = f.node;
                    MirInstId const operand = result;
                    work.pop_back();
                    result = operand.valid() ? combineCast(node2, operand)
                                             : InvalidMirInst;
                }
                break;
            case ValueFrame::Kind::MemberAddr:
                // Base lvalue ADDRESS (the deep axis), then offset + Gep — the
                // recursive arm's `basePtr = lowerLvalueAddress(kids[0]);` then
                // `combineMemberAddr`. ONE child request → phase 0 enter, phase
                // 1 combine.
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const baseN = hir.children(f.node)[0];
                    request(baseN, true);      // build base address — may invalidate `f`
                } else {
                    HirNodeId const node2 = f.node;
                    MirInstId const basePtr = result;
                    work.pop_back();
                    result = basePtr.valid() ? combineMemberAddr(node2, basePtr)
                                             : InvalidMirInst;
                }
                break;
            case ValueFrame::Kind::IndexAddr:
                // Subscript VALUE first (phase 0→1), then — IN THE RECURSIVE
                // ORDER — scale the index to bytes (emit the stride Mul BEFORE
                // the base), then the base (storage ADDRESS or pointer VALUE)
                // (phase 1→2), then the Gep. The byte-scaled index is stashed in
                // `c0` across the base request. This reproduces the recursive arm
                // `idx=lowerExpr(kids[1]); byteIdx=scaleIndexToBytes(...);
                // base=lower...(kids[0]); Gep` emission order exactly.
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const idxN = hir.children(f.node)[1];
                    request(idxN, false);      // build subscript value — may invalidate `f`
                } else if (f.phase == 1) {
                    HirNodeId const node2 = f.node;
                    auto kids = hir.children(node2);
                    MirInstId const idx = result;
                    if (!idx.valid()) { work.pop_back(); result = InvalidMirInst; break; }
                    TypeId const elemTy  = hir.typeId(node2);
                    TypeId const indexTy = hir.typeId(kids[1]);
                    // Emit the stride Mul HERE (matching the recursive order:
                    // scale BEFORE the base) — a pure helper for stride 1.
                    MirInstId const byteIdx =
                        scaleIndexToBytes(idx, elemTy, node2, indexTy);
                    if (!byteIdx.valid()) { work.pop_back(); result = InvalidMirInst; break; }
                    TypeId const baseTy = hir.typeId(kids[0]);
                    TypeKind const baseKind = baseTy.valid()
                        ? interner.kind(baseTy) : TypeKind::Void;
                    // A pointer base uses its RVALUE (`int* p; p[i]` — the
                    // pointer may be a pure-SSA Arg); a storage base uses its
                    // lvalue ADDRESS (`int a[N]; a[i]` — the deep axis).
                    bool const baseWantsAddr = (baseKind != TypeKind::Ptr);
                    f.c0    = byteIdx;         // stash across the base request
                    f.phase = 2;
                    request(kids[0], baseWantsAddr);  // build base — may invalidate `f`
                } else {
                    HirNodeId const node2 = f.node;
                    MirInstId const byteIdx = f.c0;
                    MirInstId const basePtr = result;
                    work.pop_back();
                    result = basePtr.valid()
                                 ? combineIndexAddr(node2, byteIdx, basePtr)
                                 : InvalidMirInst;
                }
                break;
            case ValueFrame::Kind::Ternary:
                // `cond ? then : else` → a diamond CFG with a phi at the join.
                // Replicates the recursive Ternary arm BYTE-FOR-BYTE:
                //   phase 0: aggregate guard, then lower COND.
                //   phase 1: mint thenBB, elseBB, joinBB (IN THAT ORDER —
                //            createBlock id == creation order), CondBr(cond,
                //            thenBB, elseBB), beginBlock(thenBB), lower THEN.
                //   phase 2: thenPred = currentlyOpenBlock(), Br(joinBB),
                //            beginBlock(elseBB), lower ELSE.
                //   phase 3: elsePred = currentlyOpenBlock(), Br(joinBB),
                //            beginBlock(joinBB), Phi[{then,thenPred},
                //            {else,elsePred}] — predecessor order then-then-else.
                // The block handles + thenVal + thenPred are carried in the
                // frame fields (bb0/bb1/bb2/v0/pred0), NEVER in a `work.back()`
                // reference that dangles after a sub-request push.
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    if (ternaryAggregateGuardFails(node2)) {
                        work.pop_back(); result = InvalidMirInst; break;
                    }
                    f.phase = 1;
                    HirNodeId const condN = hir.children(node2)[0];
                    request(condN, false);    // lower cond — may invalidate `f`
                } else if (f.phase == 1) {
                    MirInstId const cond = result;
                    if (!cond.valid()) { work.pop_back(); result = InvalidMirInst; break; }
                    HirNodeId const node2 = f.node;
                    MirBlockId const thenBB = mir.createBlock(StructCfMarker::IfThen);
                    MirBlockId const elseBB = mir.createBlock(StructCfMarker::IfElse);
                    MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
                    mir.addCondBr(cond, thenBB, elseBB);
                    mir.beginBlock(thenBB);
                    f.bb0 = thenBB; f.bb1 = elseBB; f.bb2 = joinBB;
                    f.phase = 2;
                    HirNodeId const thenN = hir.children(node2)[1];
                    request(thenN, false);    // lower then-value — may invalidate `f`
                } else if (f.phase == 2) {
                    MirInstId const thenVal = result;
                    MirBlockId const elseBB = f.bb1;
                    MirBlockId const joinBB = f.bb2;
                    if (!thenVal.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(elseBB);
                        sealCreatedAsUnreachable(joinBB);
                        work.pop_back(); result = InvalidMirInst; break;
                    }
                    HirNodeId const node2 = f.node;
                    MirBlockId const thenPred = mir.currentlyOpenBlock();
                    mir.addBr(joinBB);
                    mir.beginBlock(elseBB);
                    f.v0 = thenVal; f.pred0 = thenPred;
                    f.phase = 3;
                    HirNodeId const elseN = hir.children(node2)[2];
                    request(elseN, false);    // lower else-value — may invalidate `f`
                } else {
                    MirInstId const elseVal = result;
                    MirBlockId const joinBB = f.bb2;
                    if (!elseVal.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(joinBB);
                        work.pop_back(); result = InvalidMirInst; break;
                    }
                    HirNodeId const node2 = f.node;
                    MirInstId const thenVal  = f.v0;
                    MirBlockId const thenPred = f.pred0;
                    MirBlockId const elsePred = mir.currentlyOpenBlock();
                    mir.addBr(joinBB);
                    mir.beginBlock(joinBB);
                    std::array<MirPhiIncoming, 2> incomings{
                        MirPhiIncoming{thenVal, thenPred},
                        MirPhiIncoming{elseVal, elsePred},
                    };
                    work.pop_back();
                    result = mir.addPhi(hir.typeId(node2), incomings);
                }
                break;
            case ValueFrame::Kind::Logical:
                // `lhs && rhs` / `lhs || rhs` short-circuit → a one-armed
                // diamond. Replicates the recursive LogicalAnd/Or arm
                // BYTE-FOR-BYTE:
                //   phase 0: lower LHS (in the CURRENT block).
                //   phase 1: lhsPred = currentlyOpenBlock(), mint rhsBB then
                //            joinBB (IN THAT ORDER), CondBr(lhs, AND?rhsBB:joinBB,
                //            AND?joinBB:rhsBB), beginBlock(rhsBB), lower RHS.
                //   phase 2: rhsPred = currentlyOpenBlock(), Br(joinBB),
                //            beginBlock(joinBB), Phi[{lhs,lhsPred},{rhs,rhsPred}]
                //            — predecessor order lhs-then-rhs.
                // lhs + lhsPred + the block handles carry in v0/pred0/bb0/bb1.
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const lhsN = hir.children(f.node)[0];
                    request(lhsN, false);     // lower lhs — may invalidate `f`
                } else if (f.phase == 1) {
                    MirInstId const lhs = result;
                    if (!lhs.valid()) { work.pop_back(); result = InvalidMirInst; break; }
                    HirNodeId const node2 = f.node;
                    bool const isAnd = (hir.kind(node2) == HirKind::LogicalAnd);
                    MirBlockId const lhsPred = mir.currentlyOpenBlock();
                    MirBlockId const rhsBB  = mir.createBlock(StructCfMarker::IfThen);
                    MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
                    mir.addCondBr(lhs, isAnd ? rhsBB : joinBB,
                                       isAnd ? joinBB : rhsBB);
                    mir.beginBlock(rhsBB);
                    f.v0 = lhs; f.pred0 = lhsPred; f.bb0 = rhsBB; f.bb1 = joinBB;
                    f.phase = 2;
                    HirNodeId const rhsN = hir.children(node2)[1];
                    request(rhsN, false);     // lower rhs — may invalidate `f`
                } else {
                    MirInstId const rhs = result;
                    MirBlockId const joinBB = f.bb1;
                    if (!rhs.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(joinBB);
                        work.pop_back(); result = InvalidMirInst; break;
                    }
                    HirNodeId const node2 = f.node;
                    MirInstId const lhs     = f.v0;
                    MirBlockId const lhsPred = f.pred0;
                    MirBlockId const rhsPred = mir.currentlyOpenBlock();
                    mir.addBr(joinBB);
                    mir.beginBlock(joinBB);
                    std::array<MirPhiIncoming, 2> incomings{
                        MirPhiIncoming{lhs, lhsPred},
                        MirPhiIncoming{rhs, rhsPred},
                    };
                    work.pop_back();
                    result = mir.addPhi(hir.typeId(node2), incomings);
                }
                break;
            case ValueFrame::Kind::SeqExpr:
                // `(s1, s2, …, result)` → run the side-effect statements in
                // order (via `lowerStmt`, a separate machine kept recursive —
                // it spins up its OWN local driver for any sub-expressions, so
                // it never touches THIS work-stack), then yield the RESULT
                // expression's value. Only the result tail re-enters this driver
                // (phase 0 requests it), so a deep comma chain's result spine
                // flattens. Byte-identical to the recursive arm: same aggregate
                // guard, same stmt order, then `lowerExpr(result)`.
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    if (seqExprAggregateGuardFails(node2)) {
                        work.pop_back(); result = InvalidMirInst; break;
                    }
                    bool stmtFailed = false;
                    for (HirNodeId stmt : hir.seqExprStmts(node2)) {
                        if (!lowerStmt(stmt)) { stmtFailed = true; break; }
                    }
                    if (stmtFailed) { work.pop_back(); result = InvalidMirInst; break; }
                    f.phase = 1;
                    // `lowerStmt` did not push to THIS work-stack, so `f` is
                    // still live; the result-tail request is the last action.
                    HirNodeId const resultN = hir.seqExprResult(node2);
                    request(resultN, false);  // lower result tail — may invalidate `f`
                } else {
                    // `result` already holds the tail expression's value.
                    work.pop_back();
                }
                break;
            case ValueFrame::Kind::Call:
                // `f(a, b, …)`: build the callee FIRST (phase 0→1, matching the
                // recursive arm's `callee = lowerExpr(kids[0])` which sequences
                // before the args), run the pre-loop setup (phase 1: callee-
                // variadic resolution + struct-return classification + sret Alloca
                // — the ONLY MIR before the args, so it MUST emit here), then pump
                // the arguments (a struct arg inline, a scalar arg through the
                // work-stack at phase 2). When all args are consumed, `finishCall`.
                // The per-arg pump + the immediate per-arg collect reproduce the
                // recursive loop's emission order EXACTLY (the helpers are the one
                // source of truth). The ctx lives at the stable index `f.aux`.
                if (f.phase == 0) {
                    f.phase = 1;
                    HirNodeId const calleeN = hir.children(f.node)[0];
                    request(calleeN, false);   // build callee — may invalidate `f`
                } else if (f.phase == 1) {
                    std::uint32_t const ctxIdx = f.aux;
                    MirInstId const callee = result;
                    if (!callee.valid()) {
                        work.pop_back(); callCtxs.pop_back();
                        result = InvalidMirInst; break;
                    }
                    f.phase = 2;
                    if (!callSetup(callee, callCtxs[ctxIdx])) {  // emits sret Alloca
                        work.pop_back(); callCtxs.pop_back();
                        result = InvalidMirInst; break;
                    }
                    int const p = pumpCallArgs(ctxIdx);  // may push (invalidates `f`)
                    if (p == 1) break;                   // entered a scalar — wait
                    if (p < 0) {
                        work.pop_back(); callCtxs.pop_back();
                        result = InvalidMirInst; break;
                    }
                    result = finishCall(callCtxs[ctxIdx]);  // all args consumed
                    work.pop_back(); callCtxs.pop_back();
                } else {
                    // A scalar arg just completed (`result` holds its value).
                    // Collect it (push + advance the running ABI cursor), then
                    // pump the next arg or finish.
                    std::uint32_t const ctxIdx = f.aux;
                    MirInstId const argVal = result;
                    if (!argVal.valid()) {
                        work.pop_back(); callCtxs.pop_back();
                        result = InvalidMirInst; break;
                    }
                    finishScalarCallArg(callCtxs[ctxIdx], argVal);
                    int const p = pumpCallArgs(ctxIdx);  // may push (invalidates `f`)
                    if (p == 1) break;                   // entered next scalar — wait
                    if (p < 0) {
                        work.pop_back(); callCtxs.pop_back();
                        result = InvalidMirInst; break;
                    }
                    result = finishCall(callCtxs[ctxIdx]);  // all args consumed
                    work.pop_back(); callCtxs.pop_back();
                }
                break;
            case ValueFrame::Kind::IntrinsicCall: {
                // `__intrinsic(a, b, …)`: all-scalar args, no callee child, no
                // struct ABI. phase 0 requests the FIRST arg (if any); phase 1
                // collects the just-lowered arg, advances, and requests the next —
                // staying in phase 1 until all args are consumed, then emits the
                // IntrinsicCall. Byte-identical to the recursive arm (lower arg →
                // push → … → emit). A scalar arg that is itself a call grows
                // `callCtxs`, so the ctx is re-addressed by the stable index `aux`.
                std::uint32_t const ctxIdx = f.aux;
                auto emitIntrinsic = [&] {
                    result = mir.addInst(MirOpcode::IntrinsicCall,
                                         callCtxs[ctxIdx].operands,
                                         callCtxs[ctxIdx].resultTy,
                                         callCtxs[ctxIdx].intrinsicId);
                    work.pop_back(); callCtxs.pop_back();
                };
                if (f.phase == 1) {
                    // Collect the arg that just completed, then advance.
                    MirInstId const argVal = result;
                    if (!argVal.valid()) {
                        work.pop_back(); callCtxs.pop_back();
                        result = InvalidMirInst; break;
                    }
                    callCtxs[ctxIdx].operands.push_back(argVal);
                    ++callCtxs[ctxIdx].argIdx;
                }
                if (callCtxs[ctxIdx].argIdx
                    >= hir.children(callCtxs[ctxIdx].node).size()) {
                    emitIntrinsic();   // all args consumed (or zero-arg)
                    break;
                }
                f.phase = 1;
                HirNodeId const argN =
                    hir.children(callCtxs[ctxIdx].node)[callCtxs[ctxIdx].argIdx];
                request(argN, false);   // lower the next arg — may invalidate `f`
                break;
            }
            }
        }
        return result;
    }

    // The public VALUE-lowering entry: lower an HIR expression to the MirInstId
    // that produces its rvalue. A thin wrapper over the shared {Value,Address}
    // driver (root request = VALUE).
    [[nodiscard]] MirInstId lowerExpr(HirNodeId node) {
        return runExprDriver(node, /*rootWantAddr=*/false);
    }

    // The public ADDRESS-lowering entry: resolve the pointer value an lvalue
    // names (a `Store` target, an `AddressOf` result, a base for member/index).
    // A thin wrapper over the shared {Value,Address} driver (root request =
    // ADDRESS). Distinct from `lowerExpr` which yields the lvalue's RVALUE
    // (`Load(ptr)`); see `lowerLvalueAddressNode` for the per-arm semantics.
    [[nodiscard]] MirInstId lowerLvalueAddress(HirNodeId node) {
        return runExprDriver(node, /*rootWantAddr=*/true);
    }

    // Error-recovery helper: every forward-`createBlock`'d block in a
    // control-flow lowering MUST be filled+terminated before the function
    // closes, or `MirBuilder::finish()` aborts. When an inner lowering
    // fails mid-CF, the parent has already created exit/join/update blocks
    // it can no longer reach. This helper begins each such block (idempotent
    // if it's already been opened+sealed) and writes `Unreachable`. Skip on
    // invalid id (block was never created for this path, e.g. else-less If).
    void sealCreatedAsUnreachable(MirBlockId b) {
        if (!b.valid()) return;
        // Only open if the block is still in the Created state (i.e. the
        // error path never reached its `beginBlock`); if a path already
        // begun + sealed it, this is a no-op.
        if (mir.isBlockUnopened(b)) {
            mir.beginBlock(b);
            mir.addUnreachable();
        }
    }

    // Materialize an `Alloca` slot for `sym` of declared type `ty` and
    // register it in `addressableLocal`. The slot's MIR type is `ptr<ty>`
    // (interned on demand). Aborts via diagnostic on a duplicate registration
    // (HIR verifier disallows redeclaration; a duplicate here is an internal
    // bug). Returns the alloca's MirInstId, or `InvalidMirInst` on error.
    [[nodiscard]] MirInstId allocaForLocal(SymbolId sym, TypeId ty,
                                           HirNodeId anchor) {
        // Enforce the documented EITHER/OR invariant at the bind site: a
        // symbol must not already live in `symbolToValue` (SSA) when we
        // give it a storage slot, nor be double-allocated. The HIR verifier
        // owns the no-redeclaration rule; this is the load-bearing local
        // assertion that catches any future invariant-break loud. `anchor`
        // is the HIR node responsible for the binding (a VarDecl or a
        // function-param VarDecl) so failure diagnostics can carry a span.
        if (addressableLocal.contains(sym.v) || symbolToValue.contains(sym.v)) {
            unsupported(anchor, std::format(
                "duplicate slot/SSA binding for symbol {} (internal bug — HIR "
                "verifier should have rejected the redeclaration, or the param "
                "slot-promotion pre-pass over-classified)", sym.v));
            return InvalidMirInst;
        }
        TypeId const ptrTy = interner.pointer(ty);
        // VLA C4b (D-CSUBSET-VLA): a VLA-TYPEDEF OBJECT (`typedef int R[n]; R a;`) has
        // NO own captured runtime size — its size was FROZEN once at R's TypeDecl (C99
        // §6.7.7p2). Route it to the copy-down path, which sources every size from R's
        // decl-frozen slots (never re-lowering `n`). Checked BEFORE the direct-VLA arm:
        // the object's own size capture was skipped at CST→HIR, so `vlaSizeMap[a.v]` is
        // absent and `vlaAllocaForLocal`/`computeVlaByteSize(a)` would fail loud on the
        // missing side-table.
        if (typedefVlaOriginMap != nullptr) {
            if (auto it = typedefVlaOriginMap->find(sym.v);
                it != typedefVlaOriginMap->end())
                return vlaAllocaFromTypedef(sym, SymbolId{it->second}, ty, ptrTy,
                                            anchor);
        }
        // VLA C1a (D-CSUBSET-VLA): a VARIABLE-LENGTH array local reserves a RUNTIME-
        // sized slot — the Alloca carries a size OPERAND, not a compile-time payload.
        // This arm runs BEFORE the isMemoryResidentType byte-size path below (which
        // nullopts + fails loud on a VLA — its layout has no static size). The size
        // expr is evaluated HERE, at the DECL point (§6.7.6.2p2: once when the
        // declaration is reached) — a VLA is emitted at its VarDecl site, never the
        // entry-block hoist (the collectLocalDecls hoist SKIPS un-sizeable arrays; the
        // load-bearing dependency that keeps `int a[f()]` evaluating in program
        // order). C1a fails loud at MIR→LIR; C1b builds the dynamic-alloca codegen.
        // VLA C3: `||typeContainsVla` routes a FIXED-outer multi-dim VLA (`int a[5][n]`
        // — top type is a fixed Array, NOT isVlaArray) to the SAME runtime path (else
        // it falls to the fixed aggregateByteSize path and fails loud on the VLA
        // element's un-sizeable layout).
        if (interner.isVlaArray(ty) || interner.typeContainsVla(ty)) {
            return vlaAllocaForLocal(sym, ty, ptrTy, anchor);
        }
        // FC7 (D-FC7-MEMBER-ACCESS): a struct/union/ARRAY local reserves its
        // FULL layout size in the frame, not one scalar slot. Encode the byte
        // size in the Alloca payload (the channel ML6 anticipated; the LIR
        // callconv sums per-alloca slots from it — payload 0 = the scalar
        // "one slot" sentinel). An aggregate whose size is unavailable
        // (no aggregateLayout / un-sizeable type) fails LOUD — never a
        // silently under-sized slot, which would corrupt the neighbour.
        // ARRAY locals are sized here too (D-MIR-STORAGE-ARRAY-INDEX-GEP): this
        // cycle makes them runnable (index + brace-init), so a bare `int a[4]`
        // must reserve 16 bytes, not the 8-byte scalar sentinel.
        std::uint32_t allocaPayload = 0;
        // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt(N>64)` local reserves its full
        // ceil(N/64)*8-byte multi-limb layout — the memory-resident sizing path (a
        // scalar 8-byte slot would under-allocate + alias the neighbour).
        if (isMemoryResidentType(interner, ty)) {
            auto const sz = aggregateByteSize(ty);
            if (!sz.has_value()) {
                unsupported(anchor, "aggregate/array local requires a sizeable "
                                    "layout (target 'aggregateLayout' params "
                                    "and a complete type)");
                return InvalidMirInst;
            }
            if (*sz > std::numeric_limits<std::uint32_t>::max()) {
                unsupported(anchor, "aggregate/array local size exceeds the "
                                    "32-bit frame-slot payload encoding");
                return InvalidMirInst;
            }
            allocaPayload = static_cast<std::uint32_t>(*sz);
        }
        // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: the local's EFFECTIVE alignment =
        // max(natural alignment of `ty`, explicit `alignas`). It rides the
        // Alloca's secondary payload (the primary is the aggregate byte size);
        // MIR→LIR aggregates the per-function maximum and feeds it to the frame
        // layout so the slot lands on the required boundary. The natural part
        // comes from the type-layout engine (any type — scalar or aggregate);
        // the explicit part from the declaration-keyed `alignmentMap` (already
        // validated by the semantic phase). 0 when neither is derivable (no
        // aggregateLayout config): a safe "no info" sentinel — the frame layout
        // then leaves the base at its existing alignment (no over-align to honor).
        std::uint32_t effectiveAlign = 0;
        if (config.aggregateLayoutLoaded) {
            if (auto const lay = computeLayout(ty, interner, config.aggregateLayout,
                                               config.dataModel);
                lay.has_value())
                effectiveAlign = lay->align.bytes();
        }
        if (alignmentMap != nullptr && anchor.valid())
            if (auto const* p = alignmentMap->tryGet(anchor);
                p != nullptr && p->alignmentBytes > effectiveAlign)
                effectiveAlign = p->alignmentBytes;
        MirInstId const a =
            mir.addInst(MirOpcode::Alloca, {}, ptrTy, allocaPayload,
                        MirInstFlags::None, /*payload2=*/effectiveAlign);
        addressableLocal[sym.v] = a;
        return a;
    }

    // VLA (D-CSUBSET-VLA): widen ONE VLA dimension bound to an i64 count, applying the
    // codegen belts (a semantic-tier S_VlaSizeNotInteger should already have fired; these
    // are defense-in-depth). A FLOAT bound would silently FPToSI-truncate; a wide
    // `_BitInt(N>64)` bound's multi-limb→i64 narrow is a later cycle; a non-integer with
    // no widening → fail loud. Shared by the direct-VLA alloca and the ptr-to-VLA stride
    // store so NEITHER can drop a belt (IMPORTANT-2).
    [[nodiscard]] MirInstId widenVlaDim(HirNodeId dimNode, HirNodeId anchor) {
        TypeId const i64ty = i64Ty();
        MirInstId const v = lowerExpr(dimNode);
        if (!v.valid()) return InvalidMirInst;
        TypeId const dimTy = hir.typeId(dimNode);
        // A FLOAT bound would silently FPToSI-TRUNCATE through mapCast — fail loud.
        if (dimTy.valid() && isFloatingKind(interner.kind(dimTy))) {
            unsupported(anchor, "variable-length array size expression has floating "
                                "type — must be integer (C 6.7.6.2p1; a semantic-"
                                "tier constraint that should already have fired)");
            return InvalidMirInst;
        }
        // A WIDE `_BitInt(N>64)` bound is a legal integer VLA size, but its
        // multi-limb → i64 narrow is a later cycle — fail loud CLEANLY, never the
        // fatal `bitIntContainerKind`-on-wide path.
        if (dimTy.valid() && interner.kind(dimTy) == TypeKind::BitInt
            && interner.bitIntWidth(dimTy) > 64) {
            unsupported(anchor, "a wide _BitInt(N>64) variable-length array bound "
                                "is not yet lowered (deferred — D-CSUBSET-VLA)");
            return InvalidMirInst;
        }
        // Project the SOURCE kind: enum→underlying, narrow `_BitInt`→native
        // container, else its own kind; then WIDEN to i64 (signed→SExt,
        // unsigned→ZExt — both C-consistent for a byte count).
        TypeKind const fromK =
            dimTy.valid() ? resolveScalarIntKind(dimTy) : TypeKind::I32;
        if (fromK == TypeKind::I64) return v;
        MirOpcode const ext = mapCast(fromK, TypeKind::I64);
        if (ext == MirOpcode::Invalid) {
            unsupported(anchor, "variable-length array size expression has a "
                                "non-integer type with no widening to a 64-bit "
                                "byte count");
            return InvalidMirInst;
        }
        std::array<MirInstId, 1> eo{v};
        return mir.addInst(ext, eo, i64ty);
    }

    // VLA (D-CSUBSET-VLA): the total runtime BYTE size of a nested array type + its
    // innermost base element type. Shared by the direct-VLA `Alloca` sizing
    // (`vlaAllocaForLocal`, `arrayTy` = the whole object) and the ptr-to-VLA row-stride
    // store (`storePtrToVlaStride`, `arrayTy` = the POINTEE row). Walks the array spine
    // into per-LEVEL shape types (level 0 = `arrayTy`), widens each captured dimension
    // bound (via `widenVlaDim`), and forms the cumulative product BOTTOM-UP: after level
    // L the running product == `sizeof` of the shape type at level L (dims L..depth-1 ×
    // baseElem). When `freezeLevelSlots`, each RUNTIME-sized level's byte size is frozen
    // ONCE into a hidden fixed 8-byte U64 `(sym, levelType)` slot (CRITICAL-1) — the
    // whole object is `sizeof a`; an intermediate VLA row is BOTH its index stride AND
    // `sizeof a[0]`. A FULLY-FIXED intermediate level (`int a[n][5]`'s `int[5]` row) is
    // NOT slotted (its stride is a compile-time `elementStride`). `freezeLevelSlots=false`
    // freezes no level (the ptr caller freezes only the single pointee slot). The captured
    // dim count MUST equal the array-level count — a mismatch (e.g. a VLA whose element
    // comes from an array typedef) is deferred (fail loud). nullopt on any belt fail-loud
    // (already reported). Program-order: the dim size-exprs (`int a[f()]`) lower HERE, at
    // the caller's decl site — never the entry hoist.
    struct VlaByteSize { MirInstId totalBytes{}; TypeId baseElemTy{}; };
    [[nodiscard]] std::optional<VlaByteSize>
    computeVlaByteSize(SymbolId sym, TypeId arrayTy, HirNodeId anchor,
                       bool freezeLevelSlots) {
        TypeId const i64ty = i64Ty();
        // The per-dimension bound exprs were captured by CST→HIR (un-skipping EVERY
        // array suffix) keyed by SymbolId, in outer→inner order. Absent ⇒ an internal
        // side-table desync — fail loud, never a 0-sized / fixed slot.
        std::vector<HirNodeId> const* dims = nullptr;
        if (vlaSizeMap != nullptr)
            if (auto it = vlaSizeMap->find(sym.v); it != vlaSizeMap->end())
                dims = &it->second;
        if (dims == nullptr || dims->empty()) {
            unsupported(anchor, "variable-length array local has no captured runtime "
                                "size expression (internal side-table desync)");
            return std::nullopt;
        }
        // Walk the nested array type into per-LEVEL shape types (level 0 = `arrayTy`, the
        // whole object; each next = its element via ops[0]) down to the non-array
        // base. A level is EITHER a VLA (runtime dim) or a fixed Array (compile-time
        // dim) — both are kind Array; the base terminates the walk.
        std::vector<TypeId> levelTypes;
        TypeId walk = arrayTy;
        for (int guard = 0; guard < 4096 && walk.valid()
                            && interner.kind(walk) == TypeKind::Array; ++guard) {
            levelTypes.push_back(walk);
            auto const ops = interner.operands(walk);
            if (ops.empty()) {
                unsupported(anchor, "variable-length array type has an array level with "
                                    "no element (interner invariant violated)");
                return std::nullopt;
            }
            walk = ops[0];
        }
        TypeId const baseElemTy = walk;   // the innermost non-array element
        std::size_t const depth = levelTypes.size();
        // The captured dim count MUST equal the array-level count — one bound per
        // declarator suffix. A mismatch means the type carries array levels that are
        // NOT declarator suffixes on this object — realistically a VLA whose element
        // comes from an array typedef (`typedef int R[5]; R a[n];` → type `int[n][5]`,
        // one `[n]` suffix but two levels). C3 captures only declarator suffixes, so
        // this shape is deferred (C4) — fail loud with a real diagnostic, never guess.
        if (dims->size() != depth) {
            unsupported(anchor, std::format(
                "a variable-length array of this shape is not yet supported: its type "
                "has {} array level(s) but only {} declarator dimension bound(s) were "
                "captured (e.g. a VLA whose element comes from an array typedef, "
                "`typedef int R[5]; R a[n];`) — deferred (D-CSUBSET-VLA)",
                depth, dims->size()));
            return std::nullopt;
        }
        // Lower + widen each dimension bound to an i64 count, in OUTER→INNER (source)
        // order so any side effects (`int a[(k+=3)][(k+=5)]`) evaluate ONCE, in
        // program order (C 6.7.6.2p2). The per-dim belts apply to EVERY dimension.
        std::vector<MirInstId> counts;
        counts.reserve(depth);
        for (HirNodeId const dimNode : *dims) {
            MirInstId const c = widenVlaDim(dimNode, anchor);
            if (!c.valid()) return std::nullopt;
            counts.push_back(c);
        }
        // baseElemSize = the innermost element's byte size (a COMPILE-TIME constant).
        // nullopt / 0 → fail loud (incomplete / empty-aggregate element), never a
        // `Mul(count, 0)` that makes every index alias offset 0.
        std::optional<std::uint64_t> const baseStride =
            baseElemTy.valid() ? elementStride(baseElemTy) : std::nullopt;
        if (!baseStride.has_value() || *baseStride == 0) {
            unsupported(anchor, "variable-length array base element type has no "
                                "computable non-zero size (incomplete element or no "
                                "aggregateLayout)");
            return std::nullopt;
        }
        // Cumulative product BOTTOM-UP (see the doc comment). Each runtime-sized level is
        // frozen into a per-object (sym, levelType) slot ONLY when `freezeLevelSlots`.
        MirInstId acc = ci64(static_cast<std::int64_t>(*baseStride));
        for (std::size_t L = depth; L-- > 0;) {
            std::array<MirInstId, 2> mo{counts[L], acc};
            acc = mir.addInst(MirOpcode::Mul, mo, i64ty);
            if (!acc.valid()) return std::nullopt;
            if (freezeLevelSlots
                && (interner.isVlaArray(levelTypes[L])
                    || interner.typeContainsVla(levelTypes[L]))) {
                TypeId const u64PtrTy =
                    interner.pointer(interner.primitive(TypeKind::U64));
                MirInstId const slot =
                    mir.addInst(MirOpcode::Alloca, {}, u64PtrTy, /*payload=*/8,
                                MirInstFlags::None, /*payload2=*/8);
                if (!slot.valid()) return std::nullopt;
                std::array<MirInstId, 2> stOps{acc, slot};
                mir.addInst(MirOpcode::Store, stOps, InvalidType);
                vlaStrideSlot[vlaSlotKey(sym.v, levelTypes[L].v)] = slot;
            }
        }
        if (!acc.valid()) return std::nullopt;
        return VlaByteSize{acc, baseElemTy};
    }

    // VLA C4a-local (D-CSUBSET-VLA): freeze a LOCAL pointer-to-VLA's runtime ROW stride
    // at its decl point. `int (*p)[n]` — the subscript `p[i]` steps by the pointee's
    // runtime byte size (`n*sizeof(int)`), recovered by `scaleIndexToBytes` from the
    // `(p, pointeeTy)` slot exactly as a declared VLA row. CRITICAL-2: this MUST run at
    // the VarDecl decl site (program order) — the pointer's own 8-byte alloca is HOISTED
    // to entry, where `n` is not yet stored; reading `n` there would freeze a garbage
    // stride. `freezeLevelSlots=false` so ONLY the single top pointee slot is frozen
    // here — a multi-level pointee `int(*p)[n][m]`'s inner subscript then MISSES and
    // fails loud cleanly at `scaleIndexToBytes` (never a partial silent miscompute,
    // MINOR-1). `pointeeTy == operands(p.type)[0] == hir.typeId(Index(p,i))`, so the key
    // matches the subscript lookup verbatim.
    [[nodiscard]] bool storePtrToVlaStride(SymbolId sym, TypeId pointeeTy,
                                           HirNodeId anchor) {
        auto const sz = computeVlaByteSize(sym, pointeeTy, anchor,
                                           /*freezeLevelSlots=*/false);
        if (!sz.has_value()) return false;   // a belt fail-loud already reported
        TypeId const u64PtrTy =
            interner.pointer(interner.primitive(TypeKind::U64));
        MirInstId const slot =
            mir.addInst(MirOpcode::Alloca, {}, u64PtrTy, /*payload=*/8,
                        MirInstFlags::None, /*payload2=*/8);
        if (!slot.valid()) return false;
        std::array<MirInstId, 2> stOps{sz->totalBytes, slot};
        mir.addInst(MirOpcode::Store, stOps, InvalidType);
        vlaStrideSlot[vlaSlotKey(sym.v, pointeeTy.v)] = slot;
        return true;
    }

    // VLA C1a/C3 (D-CSUBSET-VLA): materialize a (possibly MULTI-DIMENSIONAL) variable-
    // length array local's RUNTIME-sized `Alloca`. The caller (`allocaForLocal`) ran
    // the duplicate-binding guard; this runs for `isVlaArray(ty) || typeContainsVla(ty)`.
    // Shape: operand[0] = the total runtime BYTE size (via the shared
    // `computeVlaByteSize`), payload = 0 (the "runtime-sized" sentinel, DISTINCT from a
    // fixed alloca's non-zero byte payload), payload2 = the effective alignment. Fails
    // loud downstream at MIR→LIR (C1a) / builds the dynamic `sub sp,<size>` (C1b).
    // `freezeLevelSlots=true`: every runtime-sized LEVEL's byte size is frozen into a
    // per-object `(sym, levelType)` slot (the whole object is `sizeof a`; each VLA row is
    // its index stride) — CRITICAL-1.
    [[nodiscard]] MirInstId vlaAllocaForLocal(SymbolId sym, TypeId ty, TypeId ptrTy,
                                              HirNodeId anchor) {
        // VLA C4a-local (D-CSUBSET-VLA): the stride math is shared with the ptr-to-VLA
        // decl-site store (`storePtrToVlaStride`) via `computeVlaByteSize`, so neither
        // can silently drop a belt. `freezeLevelSlots=true`: as a DIRECT VLA object,
        // freeze every runtime-sized level's `(sym, levelType)` size slot (CRITICAL-1 —
        // the whole object is `sizeof a`; each VLA row is its index stride).
        auto const sz = computeVlaByteSize(sym, ty, anchor, /*freezeLevelSlots=*/true);
        if (!sz.has_value()) return InvalidMirInst;
        MirInstId const bytes = sz->totalBytes;   // = sizeof level 0 = whole-object total
        TypeId const baseElemTy = sz->baseElemTy;
        // Effective alignment from the BASE element (the VLA levels have no static
        // layout) + any `alignas` override on the decl. 0 = "no info". Mirrors the
        // fixed path's payload2 channel; the PRIMARY payload stays 0 (runtime-sized).
        std::uint32_t effectiveAlign = 0;
        if (config.aggregateLayoutLoaded && baseElemTy.valid())
            if (auto const lay = computeLayout(baseElemTy, interner,
                                               config.aggregateLayout,
                                               config.dataModel);
                lay.has_value())
                effectiveAlign = lay->align.bytes();
        if (alignmentMap != nullptr && anchor.valid())
            if (auto const* p = alignmentMap->tryGet(anchor);
                p != nullptr && p->alignmentBytes > effectiveAlign)
                effectiveAlign = p->alignmentBytes;
        std::array<MirInstId, 1> aops{bytes};
        MirInstId const a =
            mir.addInst(MirOpcode::Alloca, aops, ptrTy, /*payload=*/0,
                        MirInstFlags::None, /*payload2=*/effectiveAlign);
        addressableLocal[sym.v] = a;
        return a;
    }

    // VLA C4b (D-CSUBSET-VLA): materialize a VLA-TYPEDEF object's (`typedef int R[n]; R
    // a;`) runtime `Alloca` by COPYING the typedef origin R's decl-frozen per-level size
    // slots DOWN into the object's OWN `(a, levelType)` slots — NOT by re-lowering `n`
    // (C99 §6.7.7p2: the size was frozen ONCE, when R was reached; `n` may have changed
    // since — the freeze-once invariant). `a`'s type is byte-identical to R's (the
    // semantic `declTy == headTy` gate), so the interned level-type peel yields the SAME
    // TypeIds R froze under → the copied keys EXACTLY match what `a[i]` /
    // `sizeof a`(/`sizeof a[0]`) later Load (I3, no depth arithmetic). Only the levels R
    // ACTUALLY froze are copied (a FIXED intermediate level — `R[n][5]`'s `int[5]` — was
    // never slotted; its stride is compile-time). Level 0 (the whole object) is a VLA by
    // construction (a VLA-typedef object is a VLA at the top), so it is always frozen,
    // and its value doubles as `a`'s runtime alloca byte size. Emitted at `a`'s VarDecl
    // site in program order, AFTER R's TypeDecl freeze — a VLA local is NOT entry-hoisted
    // (`computeLayout` nullopts on the -2 level → the hoist skips it), the load-bearing
    // ordering that keeps this copy-down reading R's already-Stored (live) frozen slots.
    [[nodiscard]] MirInstId vlaAllocaFromTypedef(SymbolId a, SymbolId origin, TypeId ty,
                                                 TypeId ptrTy, HirNodeId anchor) {
        TypeId const i64ty    = i64Ty();
        TypeId const u64PtrTy =
            interner.pointer(interner.primitive(TypeKind::U64));
        // Peel `ty`'s array spine into per-LEVEL shape types (level 0 = whole object,
        // each next via ops[0]) down to the non-array base — the SAME walk
        // `computeVlaByteSize` used when it froze R's slots (so the level TypeIds, hence
        // the slot keys, are identical).
        std::vector<TypeId> levelTypes;
        TypeId walk = ty;
        for (int guard = 0; guard < 4096 && walk.valid()
                            && interner.kind(walk) == TypeKind::Array; ++guard) {
            levelTypes.push_back(walk);
            auto const ops = interner.operands(walk);
            if (ops.empty()) {
                unsupported(anchor, "variable-length array typedef object has an array "
                                    "level with no element (interner invariant "
                                    "violated)");
                return InvalidMirInst;
            }
            walk = ops[0];
        }
        if (levelTypes.empty()) {
            unsupported(anchor, "variable-length array typedef object has a non-array "
                                "top type (internal side-table desync)");
            return InvalidMirInst;
        }
        // Copy each level R froze down into a's own slot, keyed IDENTICALLY so `a[i]` /
        // `sizeof a` Load them (I3 — copy ONLY the HITS; a fixed intermediate level R
        // never slotted is skipped, matching `computeVlaByteSize`'s freeze predicate).
        // Capture level 0's copied value as a's whole-object runtime byte size.
        MirInstId total = InvalidMirInst;
        for (std::size_t L = 0; L < levelTypes.size(); ++L) {
            auto const it =
                vlaStrideSlot.find(vlaSlotKey(origin.v, levelTypes[L].v));
            if (it == vlaStrideSlot.end()) continue;   // R did not freeze this level
            std::array<MirInstId, 1> ld{it->second};
            MirInstId const val = mir.addInst(MirOpcode::Load, ld, i64ty);
            if (!val.valid()) return InvalidMirInst;
            MirInstId const slot =
                mir.addInst(MirOpcode::Alloca, {}, u64PtrTy, /*payload=*/8,
                            MirInstFlags::None, /*payload2=*/8);
            if (!slot.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> st{val, slot};
            mir.addInst(MirOpcode::Store, st, InvalidType);
            vlaStrideSlot[vlaSlotKey(a.v, levelTypes[L].v)] = slot;
            if (L == 0) total = val;   // level 0's frozen value == whole-object total
        }
        if (!total.valid()) {
            // Level 0 (the whole object) is a VLA by construction, so R MUST have frozen
            // it — absent ⇒ an internal side-table desync (R's TypeDecl freeze did not
            // run, or keyed differently). Fail loud, never a 0-sized / stale alloca.
            unsupported(anchor, "variable-length array typedef object's origin froze no "
                                "whole-object size slot (internal side-table desync)");
            return InvalidMirInst;
        }
        // Effective alignment from the BASE element (`walk` = the innermost non-array
        // element) + any `alignas` override — mirror `vlaAllocaForLocal` (the VLA levels
        // have no static layout; the PRIMARY payload stays 0 = runtime-sized).
        std::uint32_t effectiveAlign = 0;
        if (config.aggregateLayoutLoaded && walk.valid())
            if (auto const lay = computeLayout(walk, interner, config.aggregateLayout,
                                               config.dataModel);
                lay.has_value())
                effectiveAlign = lay->align.bytes();
        if (alignmentMap != nullptr && anchor.valid())
            if (auto const* p = alignmentMap->tryGet(anchor);
                p != nullptr && p->alignmentBytes > effectiveAlign)
                effectiveAlign = p->alignmentBytes;
        std::array<MirInstId, 1> aops{total};
        MirInstId const av =
            mir.addInst(MirOpcode::Alloca, aops, ptrTy, /*payload=*/0,
                        MirInstFlags::None, /*payload2=*/effectiveAlign);
        addressableLocal[a.v] = av;
        return av;
    }

    // FC7 (D-FC7-MEMBER-ACCESS): the cached layout of aggregate `aggTy`, or
    // nullptr — the fail-loud signal the caller turns into a positioned
    // diagnostic — when the target declared no `aggregateLayout` OR the type
    // is incomplete / un-sizeable. Memoized in `layoutCache_`.
    [[nodiscard]] StructLayout const* cachedLayout(TypeId aggTy) {
        if (!config.aggregateLayoutLoaded) return nullptr;
        if (auto it = layoutCache_.find(aggTy.v); it != layoutCache_.end())
            return &it->second;
        auto layout = computeLayout(aggTy, interner, config.aggregateLayout,
                                    config.dataModel);
        if (!layout) return nullptr;
        return &layoutCache_.emplace(aggTy.v, std::move(*layout)).first->second;
    }

    // The byte offset of field `fieldIdx` within `aggTy`, or nullopt (caller
    // fail-louds) when the layout is unavailable OR `fieldIdx` is out of
    // range (a malformed HIR / field-count desync — never an OOB read).
    [[nodiscard]] std::optional<std::uint64_t>
    fieldByteOffset(TypeId aggTy, std::uint32_t fieldIdx) {
        StructLayout const* layout = cachedLayout(aggTy);
        if (layout == nullptr) return std::nullopt;
        if (fieldIdx >= layout->fieldOffsets.size()) return std::nullopt;
        return layout->fieldOffsets[fieldIdx];
    }

    // The total byte size of `aggTy`, or nullopt (caller fail-louds) — the
    // Alloca payload for a struct/union local (D-FC7 frame sizing).
    [[nodiscard]] std::optional<std::uint64_t> aggregateByteSize(TypeId aggTy) {
        StructLayout const* layout = cachedLayout(aggTy);
        if (layout == nullptr) return std::nullopt;
        return layout->size;
    }

    // The byte STRIDE of an array/pointer element of type `elemTy` — the
    // multiplier that turns an element index into a byte offset (Option A,
    // D-MIR-STORAGE-ARRAY-INDEX-GEP). `computeLayout` sizes ANY type: a scalar
    // (int→4, a pointer→8) via the dataModel, an aggregate (incl. tail padding
    // = the C array stride) via the layout params. nullopt (caller fail-louds)
    // when the element type is incomplete / the target declared no layout.
    [[nodiscard]] std::optional<std::uint64_t> elementStride(TypeId elemTy) {
        StructLayout const* layout = cachedLayout(elemTy);
        if (layout == nullptr) return std::nullopt;
        return layout->size;
    }

    // FC8 D-CSUBSET-BITFIELD: the bit placement of a `MemberAccess` node IFF it
    // names a bit-field, else nullptr. The aggregate's cached layout carries one
    // `BitFieldPlacement` per field (empty unless the struct/union has a
    // bit-field); a placement with `unitBytes != 0` is a bit-field. The returned
    // pointer is stable (it lives in the memoized `layoutCache_`).
    [[nodiscard]] BitFieldPlacement const* bitfieldPlacementOf(HirNodeId node) {
        if (hir.kind(node) != HirKind::MemberAccess) return nullptr;
        auto kids = hir.children(node);
        if (kids.size() != 1) return nullptr;
        TypeId const aggTy = hir.typeId(kids[0]);
        if (!aggTy.valid()) return nullptr;
        StructLayout const* layout = cachedLayout(aggTy);
        if (layout == nullptr) return nullptr;
        std::uint32_t const fieldIdx = hir.payload(node);
        if (fieldIdx >= layout->bitFields.size()) return nullptr;
        BitFieldPlacement const& p = layout->bitFields[fieldIdx];
        return p.unitBytes != 0 ? &p : nullptr;
    }

    // True iff `k` is a SIGNED integer kind (so a bit-field extract must
    // sign-extend). U*/Char/Bool extract zero-extended (char bit-field signedness
    // is implementation-defined — we choose unsigned, self-consistently).
    [[nodiscard]] static bool bitfieldIsSigned(TypeKind k) noexcept {
        switch (k) {
            case TypeKind::I8: case TypeKind::I16: case TypeKind::I32:
            case TypeKind::I64: case TypeKind::I128: return true;
            default:                                 return false;
        }
    }

    // FC8 D-CSUBSET-ENUM-BITFIELD: an enum-typed bit-field's allocation unit IS
    // its underlying integer (C 6.7.2.1 + the enum-behaves-as-underlying rule,
    // D-CSUBSET-ENUM-INT-CONVERSION). Resolve Enum→underlying so the unit's
    // load/store width, shift/mask constants, signedness, and op result types
    // all run at the real integer type; a non-enum type passes through
    // unchanged. Kept local (mirrors detail::type_rules::enumUnderlyingOrSelf)
    // to avoid a MIR→semantic-layer header dependency.
    [[nodiscard]] TypeId enumReprType(TypeId t) {
        if (!t.valid() || interner.kind(t) != TypeKind::Enum) return t;
        auto const sc = interner.scalars(t);
        return sc.empty() ? t : interner.primitive(static_cast<TypeKind>(sc[0]));
    }

    // D-CSUBSET-BITINT-BITFIELD: a `_BitInt(N<=64)` bit-field's allocation unit runs
    // at its native container (I8/U8..I64/U64 by width+signedness), the same
    // Enum→underlying trick (the reprKind precedent). Resolving here means
    // bitfieldIsSigned / the shift-mask constants / mapCast all see a plain primitive,
    // never BitInt. Wide (N>64) BitInt bit-fields are rejected at the semantic tier
    // (the >64 allocation-unit cap) and never reach codegen.
    [[nodiscard]] TypeId bitIntReprType(TypeId t) {
        if (!t.valid() || interner.kind(t) != TypeKind::BitInt) return t;
        return interner.primitive(interner.bitIntContainerKind(t));
    }

    // FC8 D-CSUBSET-BITFIELD: extract a bit-field value from a loaded allocation
    // unit. Unsigned: `(unit >> bitOffset) & ((1<<W)-1)`. Signed: sign-extend via
    // `(unit << (B - bitOffset - W)) >>arith (B - W)` (B = unit bits). All ops are
    // computed at the field's (unit) type — reuses the existing MIR shift/and ops.
    [[nodiscard]] MirInstId
    emitBitfieldExtract(MirInstId unitVal, BitFieldPlacement const& p, TypeId fieldTy) {
        fieldTy = bitIntReprType(enumReprType(fieldTy));   // enum/BitInt → container
        std::uint32_t const B0 = p.unitBytes * 8;
        // c73 (D-CSUBSET-32BIT-ALU-FORMS): a SUB-INT (u8/u16/i8/i16/char) allocation
        // UNIT makes the shift/mask/and ops sub-int-width, which walls at the
        // target's sub-native ALU gate (sqlite's pervasive `u8 x:1` flag structs).
        // Compute the extraction at the PROMOTED int width (a bit-field read
        // promotes to int anyway, C 6.3.1.1), then Trunc the result back to the
        // field type for the caller's node-type contract. A NATIVE unit (>=32-bit:
        // u32/i32/u64/… incl. the wide `u64:40` case) keeps computeTy==fieldTy →
        // the ORIGINAL path (byte-identical, zero regression). The promote-cast's
        // SExt-vs-ZExt is irrelevant here: the field sits in the low B0 bits and
        // the shift/mask sequence re-derives its value + sign at width B.
        bool const promote = B0 < 32;
        TypeId const computeTy = promote ? interner.primitive(TypeKind::I32) : fieldTy;
        std::uint32_t const B = promote ? 32u : B0;
        if (promote) {
            std::array<MirInstId, 1> pa{unitVal};
            unitVal = mir.addInst(mapCast(interner.kind(fieldTy), TypeKind::I32),
                                  pa, computeTy);
        }
        // Shift/mask constants MUST match the COMPUTE type (never a stray I32 into a
        // native u64:40 unit — a mixed-width op the verifier misses; see
        // constIntOfType). Unit widths >64 are rejected at semantic, so mask fits u64.
        auto shiftBy = [&](MirInstId v, MirOpcode op, std::uint32_t amt) -> MirInstId {
            if (amt == 0) return v;
            MirInstId const sh = constIntOfType(static_cast<std::int64_t>(amt), computeTy);
            std::array<MirInstId, 2> ops{v, sh};
            return mir.addInst(op, ops, computeTy);
        };
        MirInstId result;
        if (bitfieldIsSigned(interner.kind(fieldTy))) {
            MirInstId v = shiftBy(unitVal, MirOpcode::Shl, B - p.bitOffset - p.bitWidth);
            result = shiftBy(v, MirOpcode::AShr, B - p.bitWidth);
        } else {
            MirInstId v = shiftBy(unitVal, MirOpcode::LShr, p.bitOffset);
            if (p.bitWidth < B) {
                std::uint64_t const mask =
                    p.bitWidth >= 64 ? ~0ull : ((1ull << p.bitWidth) - 1);
                MirInstId const m = constIntOfType(static_cast<std::int64_t>(mask), computeTy);
                std::array<MirInstId, 2> ops{v, m};
                v = mir.addInst(MirOpcode::And, ops, computeTy);
            }
            result = v;
        }
        if (promote) {   // Trunc the i32 result back to the field type (caller contract)
            std::array<MirInstId, 1> ta{result};
            result = mir.addInst(mapCast(TypeKind::I32, interner.kind(fieldTy)),
                                 ta, fieldTy);
        }
        return result;
    }

    // FC8 D-CSUBSET-BITFIELD: read-modify-write a bit-field at unit address
    // `unitPtr` with `rhsVal`: `unit = (unit & ~(mask<<off)) | ((rhs & mask)<<off)`
    // then store back. The RMW preserves every OTHER bit in the unit (incl. a
    // neighbour field packed into the same unit). Computed at the field/unit type.
    [[nodiscard]] bool
    emitBitfieldInsert(MirInstId unitPtr, MirInstId rhsVal,
                       BitFieldPlacement const& p, TypeId fieldTy,
                       MirInstFlags vf = MirInstFlags::None) {
        // c21 (D-CSUBSET-VOLATILE-QUALIFIER): when the bit-field member is
        // `volatile`, BOTH halves of the read-modify-write (the unit Load below
        // and the unit Store at the end) carry the flag — the whole RMW is one
        // volatile access of the unit, never elided/reordered.
        fieldTy = bitIntReprType(enumReprType(fieldTy));   // enum/BitInt → container
        // c73 (D-CSUBSET-32BIT-ALU-FORMS): mirror emitBitfieldExtract — a SUB-INT
        // unit computes the read-modify-write at the promoted int width, then
        // Truncs the merged unit back to the field type for the Store. A native
        // (>=32-bit) unit keeps computeTy==fieldTy → the original path (unchanged).
        std::uint32_t const B0 = p.unitBytes * 8;
        bool const promote = B0 < 32;
        TypeId const computeTy = promote ? interner.primitive(TypeKind::I32) : fieldTy;
        std::uint64_t const mask =
            p.bitWidth >= 64 ? ~0ull : ((1ull << p.bitWidth) - 1);
        std::uint64_t const fieldMask = mask << p.bitOffset;
        std::array<MirInstId, 1> lo{unitPtr};
        MirInstId unit = mir.addInst(MirOpcode::Load, lo, fieldTy,
                                     /*payload=*/0, vf);
        if (!unit.valid()) return false;
        if (promote) {
            std::array<MirInstId, 1> ua{unit};
            unit = mir.addInst(mapCast(interner.kind(fieldTy), TypeKind::I32),
                               ua, computeTy);
            // Widen the rhs to the compute type so `rhs & mask` is not a mixed-
            // width op. rhsVal is FIELD-TYPED by the caller contract (the
            // assignment RHS is HIR-coerced to the field type; the aggregate-init
            // caller coerces the value to the field type before this call). Only
            // its low `bitWidth` bits survive the mask.
            std::array<MirInstId, 1> ra{rhsVal};
            rhsVal = mir.addInst(mapCast(interner.kind(fieldTy), TypeKind::I32),
                                 ra, computeTy);
        }
        MirInstId const clrK = constIntOfType(static_cast<std::int64_t>(~fieldMask), computeTy);
        std::array<MirInstId, 2> ca{unit, clrK};
        MirInstId const cleared = mir.addInst(MirOpcode::And, ca, computeTy);
        MirInstId const mK = constIntOfType(static_cast<std::int64_t>(mask), computeTy);
        std::array<MirInstId, 2> ma{rhsVal, mK};
        MirInstId masked = mir.addInst(MirOpcode::And, ma, computeTy);
        if (p.bitOffset != 0) {
            MirInstId const sh = constIntOfType(static_cast<std::int64_t>(p.bitOffset), computeTy);
            std::array<MirInstId, 2> sa{masked, sh};
            masked = mir.addInst(MirOpcode::Shl, sa, computeTy);
        }
        std::array<MirInstId, 2> oa{cleared, masked};
        MirInstId merged = mir.addInst(MirOpcode::Or, oa, computeTy);
        if (promote) {   // Trunc the merged unit back to the field type for the Store
            std::array<MirInstId, 1> ta{merged};
            merged = mir.addInst(mapCast(TypeKind::I32, interner.kind(fieldTy)),
                                 ta, fieldTy);
        }
        std::array<MirInstId, 2> st{merged, unitPtr};
        mir.addInst(MirOpcode::Store, st, InvalidType, /*payload=*/0, vf);
        return true;
    }

    // ── FC17.9(b) C23 <stdbit.h> (D-FULLC-STDBIT): the 14 stdc_* op composition ──
    //
    // The bit-width (in bits) of an integer/bool TypeKind — the operand-width probe
    // the stdc_* arms key on. 0 for a non-fixed-width kind (a fail-loud sentinel).
    [[nodiscard]] static int intBitWidth(TypeKind k) noexcept {
        switch (k) {
            case TypeKind::Bool: case TypeKind::I8:  case TypeKind::U8:
            case TypeKind::Char: case TypeKind::Byte: return 8;
            case TypeKind::I16:  case TypeKind::U16:  return 16;
            case TypeKind::I32:  case TypeKind::U32:  return 32;
            case TypeKind::I64:  case TypeKind::U64:  return 64;
            default: return 0;
        }
    }

    // Compose one C23 `stdc_*` bit operation (N3096 §7.18) from the 3 hardware
    // bit-count primitives (MIR Popcount/Clz/Ctz) + the universal ALU verbs.
    //   • WIDTH-CORRECT: the operand's EXACT width W∈{8,16,32,64} is read from its
    //     coerced HIR param-core type `xTy` (guaranteed by the pre-BuiltinCall arg
    //     coercion). All compute is at the promotion width P∈{32,64} (the
    //     primitives' native width — Clz/Ctz are DEFINED at 0 = P), then the value
    //     is cast to `t` (the builtin's C23 result type). A sub-word W is handled by
    //     ARITHMETIC (subtract the P−W zero-ext pad; mask the W-bit complement),
    //     never a per-width instruction.
    //   • SINGLE-EVAL: `x` is lowered ONCE by the caller into operands[0]; every
    //     sub-expression here reuses that ONE SSA value.
    //   • BRANCHLESS: the 8 ops with a C `?:` select via `mask = 0 − (uP)cond` (all
    //     -ones iff cond, else 0) → `(a & mask) | (b & ~mask)`. NO CFG diamond, NO
    //     `if (arch/format==…)`: pure generic MIR the optimizer + every target
    //     lowering already understand.
    //   • SHIFT-UB SAFE (audit I3): every `Shl(1, amt)` has amt∈[0,W−1] on EVERY
    //     branch (bit_floor amt = bitWidth − (x≠0); bit_ceil amt = bitWidth(x−1) &
    //     (W−1), with the ==W overflow arm selecting 0). Never an unguarded shift.
    // `xTy` is the operand's coerced HIR type (its builtin param core U8/U16/U32/
    // U64 — read by the caller via hir.typeId(argNode)); the width W is taken from
    // it, so the composition never queries the MirBuilder for a value's type.
    [[nodiscard]] MirInstId emitStdbitOp(BuiltinLowering op, MirInstId x0,
                                         TypeId xTy, HirNodeId node, TypeId t) {
        TypeKind const xKind = interner.kind(xTy);
        int const W = intBitWidth(xKind);
        if (W != 8 && W != 16 && W != 32 && W != 64) {
            unsupported(node,
                "a stdc_* bit builtin operand must be an 8/16/32/64-bit integer "
                "(its builtin param core) — the width is read to compose the "
                "N3096 §7.18 formula");
            return InvalidMirInst;
        }
        int const P = (W <= 32) ? 32 : 64;
        TypeKind const pKind = (P == 32) ? TypeKind::U32 : TypeKind::U64;
        TypeId   const pTy   = interner.primitive(pKind);
        // vP = (uP)x — zero-extend to the promotion width (a no-op when W==P).
        MirInstId vP = x0;
        if (xKind != pKind) {
            std::array<MirInstId, 1> za{x0};
            vP = mir.addInst(mapCast(xKind, pKind), za, pTy);
        }

        // ── shared branchless composition helpers (all pTy-typed) ──
        auto konst = [&](std::int64_t v) { return constIntOfType(v, pTy); };
        auto bin = [&](MirOpcode o, MirInstId a, MirInstId b) {
            std::array<MirInstId, 2> ops{a, b};
            return mir.addInst(o, ops, pTy);
        };
        auto un = [&](MirOpcode o, MirInstId a) {
            std::array<MirInstId, 1> ops{a};
            return mir.addInst(o, ops, pTy);
        };
        // an ICmp result is i1 (Bool), NEVER pTy — a pTy-typed compare would
        // mis-type the ZExt in `sel` (it would see a pTy source, not a Bool).
        auto cmp = [&](MirOpcode o, MirInstId a, MirInstId b) {
            std::array<MirInstId, 2> ops{a, b};
            return mir.addInst(o, ops, interner.primitive(TypeKind::Bool));
        };
        // the 3 primitives at width P (result pTy — the count ≤P fits; mir_to_lir
        // reads the OPERAND type, here pTy, to pick the 32- vs 64-bit realization).
        auto pop = [&](MirInstId v) { return un(MirOpcode::Popcount, v); };
        auto clz = [&](MirInstId v) { return un(MirOpcode::Clz, v); };
        auto ctz = [&](MirInstId v) { return un(MirOpcode::Ctz, v); };
        // (uP)(uW)~x — the W-bit complement: flip all P bits, keep the low W.
        std::uint64_t const allOnesW = (W >= 64) ? ~0ull : ((1ull << W) - 1);
        auto complementW = [&]() {
            return bin(MirOpcode::And, un(MirOpcode::Not, vP),
                       konst(static_cast<std::int64_t>(allOnesW)));
        };
        // branchless select: cond ? a : b (a,b already-computed pTy SSA values →
        // single-eval). mask = 0 − (uP)cond (all-ones iff cond, else 0).
        auto sel = [&](MirInstId condBool, MirInstId a, MirInstId b) {
            MirInstId const condP   = un(MirOpcode::ZExt, condBool);
            MirInstId const mask    = bin(MirOpcode::Sub, konst(0), condP);
            MirInstId const notMask = un(MirOpcode::Not, mask);
            return bin(MirOpcode::Or, bin(MirOpcode::And, a, mask),
                                      bin(MirOpcode::And, b, notMask));
        };
        // cast the pTy compute value to the builtin result type t (a Trunc for a
        // U64 compute → U32 count / a narrow bit_floor result; else a no-op).
        auto toResult = [&](MirInstId v) -> MirInstId {
            TypeKind const tk = interner.kind(t);
            if (tk == pKind) return v;
            std::array<MirInstId, 1> ca{v};
            return mir.addInst(mapCast(pKind, tk), ca, t);
        };

        MirInstId const ZERO = konst(0);
        MirInstId const ONE  = konst(1);
        MirInstId const WK   = konst(W);       // W
        MirInstId const PmWK = konst(P - W);   // P − W (the zero-ext pad width)

        switch (op) {
            case BuiltinLowering::StdcCountOnes:      // pc
                return toResult(pop(vP));
            case BuiltinLowering::StdcCountZeros:     // W − pc (W real bits only)
                return toResult(bin(MirOpcode::Sub, WK, pop(vP)));
            case BuiltinLowering::StdcLeadingZeros:   // clz(x) − (P − W)
                return toResult(bin(MirOpcode::Sub, clz(vP), PmWK));
            case BuiltinLowering::StdcLeadingOnes:    // clz((uP)(uW)~x) − (P − W)
                return toResult(bin(MirOpcode::Sub, clz(complementW()), PmWK));
            case BuiltinLowering::StdcTrailingZeros: {  // x==0 ? W : ctz(x)
                MirInstId const isZero = cmp(MirOpcode::ICmpEq, vP, ZERO);
                return toResult(sel(isZero, WK, ctz(vP)));
            }
            case BuiltinLowering::StdcTrailingOnes: {   // x==(uW)~0 ? W : ctz((uP)(uW)~x)
                MirInstId const isAllOnes =
                    cmp(MirOpcode::ICmpEq, vP, konst(static_cast<std::int64_t>(allOnesW)));
                return toResult(sel(isAllOnes, WK, ctz(complementW())));
            }
            case BuiltinLowering::StdcFirstLeadingZero: {  // lo==W ? 0 : lo+1 (lo=leading_ones)
                MirInstId const lo  = bin(MirOpcode::Sub, clz(complementW()), PmWK);
                MirInstId const isW = cmp(MirOpcode::ICmpEq, lo, WK);
                return toResult(sel(isW, ZERO, bin(MirOpcode::Add, lo, ONE)));
            }
            case BuiltinLowering::StdcFirstLeadingOne: {   // x==0 ? 0 : leading_zeros+1
                MirInstId const lz     = bin(MirOpcode::Sub, clz(vP), PmWK);
                MirInstId const isZero = cmp(MirOpcode::ICmpEq, vP, ZERO);
                return toResult(sel(isZero, ZERO, bin(MirOpcode::Add, lz, ONE)));
            }
            case BuiltinLowering::StdcFirstTrailingZero: { // to==W ? 0 : to+1 (to=trailing_ones)
                MirInstId const isAllOnes =
                    cmp(MirOpcode::ICmpEq, vP, konst(static_cast<std::int64_t>(allOnesW)));
                MirInstId const to  = sel(isAllOnes, WK, ctz(complementW()));
                MirInstId const isW = cmp(MirOpcode::ICmpEq, to, WK);
                return toResult(sel(isW, ZERO, bin(MirOpcode::Add, to, ONE)));
            }
            case BuiltinLowering::StdcFirstTrailingOne: {  // x==0 ? 0 : trailing_zeros+1
                MirInstId const isZero = cmp(MirOpcode::ICmpEq, vP, ZERO);
                return toResult(sel(isZero, ZERO, bin(MirOpcode::Add, ctz(vP), ONE)));
            }
            case BuiltinLowering::StdcHasSingleBit:   // popcount(x)==1 (result IS Bool == t)
                return cmp(MirOpcode::ICmpEq, pop(vP), ONE);
            case BuiltinLowering::StdcBitWidth:       // P − clz(x)  (clz(0)=P → 0; no guard)
                return toResult(bin(MirOpcode::Sub, konst(P), clz(vP)));
            case BuiltinLowering::StdcBitFloor: {      // x==0 ? 0 : 1 << (bit_width(x)−1)
                MirInstId const bw      = bin(MirOpcode::Sub, konst(P), clz(vP));
                MirInstId const neZero  = cmp(MirOpcode::ICmpNe, vP, ZERO);
                MirInstId const neZeroP = un(MirOpcode::ZExt, neZero);
                // amt = bit_width − (x≠0) ∈ [0, W−1] on EVERY branch (x==0 → 0−0=0).
                MirInstId const amt     = bin(MirOpcode::Sub, bw, neZeroP);
                MirInstId const shifted = bin(MirOpcode::Shl, ONE, amt);
                return toResult(sel(neZero, shifted, ZERO));
            }
            case BuiltinLowering::StdcBitCeil: {       // x<=1 ? 1 : bw(x−1)==W ? 0 : 1<<bw(x−1)
                MirInstId const isLe1   = cmp(MirOpcode::ICmpUle, vP, ONE);
                MirInstId const xm1     = bin(MirOpcode::Sub, vP, ONE);
                MirInstId const bwm1    = bin(MirOpcode::Sub, konst(P), clz(xm1));
                MirInstId const isOvf   = cmp(MirOpcode::ICmpEq, bwm1, WK);
                // amt = bw(x−1) & (W−1) ∈ [0, W−1] (bw(x−1)==W → 0, but the overflow
                // arm discards the shift result anyway) — never an unguarded Shl.
                MirInstId const amt     = bin(MirOpcode::And, bwm1, konst(W - 1));
                MirInstId const shifted = bin(MirOpcode::Shl, ONE, amt);
                MirInstId const inner   = sel(isOvf, ZERO, shifted);
                return toResult(sel(isLe1, ONE, inner));
            }
            default:
                break;
        }
        unsupported(node, "internal: emitStdbitOp reached with a non-stdbit lowering");
        return InvalidMirInst;
    }

    // Resolve the ADDRESS of an lvalue expression for ONE node, given that its
    // child lvalue/sub-expressions are resolved by RE-ENTERING the public
    // `lowerLvalueAddress` / `lowerExpr` drivers. Distinct from `lowerExpr`
    // which produces the RVALUE of the lvalue (`Load(ptr)`). Supported lvalue
    // shapes:
    //   - `Ref(sym)` where sym is an addressable local → the local's alloca.
    //   - `Deref(ptr)` → the lowered pointer (no double-load).
    //   - `MemberAccess(base, .field)` → `GEP(addressOf(base), const-field)`.
    //   - `Index(base, idxExpr)` → `GEP(addressOf(base), idxValue)`.
    // Returns `InvalidMirInst` on failure.
    //
    // Plan 24 Stage 4-address (D-PARSE-DEEP-NEST-RECURSION-MEMORY): the public
    // `lowerLvalueAddress` is now the {Value,Address} work-stack DRIVER (shared
    // with `lowerExpr`). The deep base-chain arms (MemberAccess base, Index
    // storage-base — the `a.b.c.d` / `a[i][j][k]` lvalue axes) are flattened
    // onto that work-stack so a deeply-nested lvalue chain carries FLAT O(1)
    // host-stack cost per level. This per-NODE handler is the byte-identical
    // emission body for EVERY OTHER lvalue arm (Ref/global, Deref, Call-sret,
    // the CFG/slot arms ConstructAggregate/Ternary/SeqExpr/VaArg); the driver
    // routes those here unchanged (their own re-entries flatten). The two
    // flattened arms here call the SAME `combineMemberAddr`/`combineIndexAddr`
    // epilogues the frames do (one source of truth) and are unreachable through
    // the driver — kept as the recursive fallback.

    // D-LK4-RODATA-PRODUCER-STRING: materialize a STRING LITERAL into a
    // synthetic rodata MirGlobal and return a `GlobalAddr` (typed `ptrTy`) to its
    // first byte. The SINGLE producer, shared by the array→pointer DECAY Cast arm
    // (value position) and the lvalue-address `HirKind::Literal` arm (`"abc"[i]`
    // — F5 agg_string_index). A non-string literal / SymbolId-space exhaustion
    // fails loud (no silent miscompile).
    //
    // FC17.5 F2 (D-CSUBSET-FUNC-PREDEFINED-IDENTIFIER): occurrences are
    // MEMOIZED by (array TypeId, byte content) — identical literals share ONE
    // rodata global (C 6.4.5p7 permits it for all strings; C99 6.4.2.2
    // REQUIRES it for `__func__`, whose two folded reads must decay to EQUAL
    // pointers). Each USE still gets its own GlobalAddr instruction; only the
    // backing global dedups.
    // The memo-aware minting CORE, shared by BOTH string-global producers —
    // `materializeStringLiteralGlobal` (function-body literals / decay / index)
    // AND `tryClassifyAsSymbolAddr`'s Cast-of-string-Literal constant-initializer
    // arm (`static const char *p = "s";` / `= __func__;`). ONE producer core is
    // what makes the C99 6.4.2.2 `__func__` identity hold ACROSS positions: a
    // body read and a static-initializer reference of the same function's
    // `__func__` must decay to EQUAL pointers (the code-audit's MEDIUM-1 — two
    // independent minters broke it). Precondition: the literal's pool entry IS a
    // string (callers check). Invalid on SymbolId exhaustion — each caller keeps
    // its own failure posture (loud vs classify-fallback).
    [[nodiscard]] SymbolId internStringLiteralGlobal(HirNodeId litNode) {
        std::uint32_t const litIdx0 = hir.payload(litNode);
        HirLiteralValue const& src = literals.at(litIdx0);
        auto memoKey = std::pair{hir.typeId(litNode).v,
                                 std::get<std::string>(src.value)};
        if (auto it = stringGlobalMemo_.find(memoKey);
            it != stringGlobalMemo_.end()) {
            return it->second;
        }
        SymbolId const sym = mintSyntheticGlobalSymbol();
        if (!sym.valid()) return sym;
        std::uint32_t const mirLitIdx = mir.literalPoolAdd(toMirLiteral(src));
        // D-CSUBSET-MUTABLE-CHAR-ARRAY-RODATA: the synthetic string-literal-pool
        // bytes are IMMUTABLE read-only data (a string literal is const however
        // it is used). Mint the global CONST so the asm section selection routes
        // it to `.rodata` — `isConst` is the discriminator that lets a NAMED
        // mutable `char arr[N]="str"` go to writable `.data` while this SYNTHETIC
        // pool global stays read-only.
        (void)mir.addGlobal(hir.typeId(litNode), sym, mirLitIdx, {},
                            SymbolBinding::Global, SymbolVisibility::Default,
                            /*isConst=*/true, MirThreadStorage::Shared);
        stringGlobalMemo_.emplace(std::move(memoKey), sym);
        return sym;
    }

    [[nodiscard]] MirInstId materializeStringLiteralGlobal(HirNodeId litNode,
                                                           TypeId ptrTy) {
        std::uint32_t const litIdx0 = hir.payload(litNode);
        HirLiteralValue const& src = literals.at(litIdx0);
        if (!std::holds_alternative<std::string>(src.value)) {
            unsupported(litNode,
                "string-literal materialization: the Literal pool entry is not "
                "a string arm (non-string array literals anchored "
                "D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY-LITERAL-DECAY)");
            return InvalidMirInst;
        }
        SymbolId const sym = internStringLiteralGlobal(litNode);
        if (!sym.valid()) {
            unsupported(litNode,
                "string-literal promotion failed: synthetic SymbolId space "
                "exhausted (UINT32_MAX wraparound). Anchor: "
                "D-LK4-RODATA-PRODUCER-STRING space-exhaustion pin.");
            return InvalidMirInst;
        }
        return mir.addGlobalAddr(sym, ptrTy);
    }

    [[nodiscard]] MirInstId lowerLvalueAddressNode(HirNodeId node) {
        HirKind const k = hir.kind(node);
        // C23 _BitInt(N>64) (D-CSUBSET-BITINT-C2-WIDE, Model A): a wide-`_BitInt`-typed
        // arithmetic/bitwise/shift/convert result is an AGGREGATE rvalue — the FIRST
        // one in C produced by arithmetic. It has no SSA value, so it is realized BY
        // ADDRESS: materialize the multi-limb result into a fresh slot and return the
        // slot address. `request` flips a wide-BitInt VALUE read here too, so a wide
        // operand of a compare/`!`/wide→scalar-cast is delivered as an address to the
        // combine* value arm. (A wide-BitInt lvalue Ref/Deref/Member/Index falls
        // through to the ordinary lvalue arms below.)
        if (isWideBitInt(interner, hir.typeId(node))) {
            if (k == HirKind::BinaryOp) return materializeWideBinaryOp(node);
            if (k == HirKind::UnaryOp)  return materializeWideUnaryOp(node);
            if (k == HirKind::Cast)     return materializeWideCast(node);
            if (k == HirKind::Literal)  return materializeWideLiteral(node);
        }
        // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value struct/union-
        // returning CALL is an aggregate rvalue materialized into a result slot;
        // its lvalue address IS that slot. `lowerExpr(Call)` does the
        // materialization and yields the slot address, so every aggregate
        // consumer (`a = f()`, `g(f())`, `f().x`, `return f();`) reaches the
        // result by address (the call is emitted exactly once — see the
        // call-once pin). A scalar call is not an lvalue → falls through.
        // D-CSUBSET-BITINT-C2-WIDE: a wide-`_BitInt`-returning call is likewise an
        // aggregate rvalue — its result slot is set up by `callSetup`/`finishCall`
        // (the wide `isByValueClass` return classification), reached by address here.
        if (k == HirKind::Call) {
            TypeId const ct = hir.typeId(node);
            if (ct.valid()
                && (interner.kind(ct) == TypeKind::Struct
                    || interner.kind(ct) == TypeKind::Union
                    || isWideBitInt(interner, ct)))
                return lowerExpr(node);
        }
        if (k == HirKind::Ref) {
            std::uint32_t const sym = hir.payload(node);
            if (auto it = addressableLocal.find(sym);
                it != addressableLocal.end()) {
                return it->second;
            }
            if (globalSymbols.contains(sym)) {
                // Lvalue of a global: the GlobalAddr is the pointer-to-
                // storage. Result type is pointer(declaredType).
                TypeId const declared = hir.typeId(node);
                if (!declared.valid()) {
                    unsupported(node, std::format(
                        "global Ref to symbol {} has no type", sym));
                    return InvalidMirInst;
                }
                return mir.addGlobalAddr(SymbolId{sym},
                                         interner.pointer(declared));
            }
            if (functionSymbols.contains(sym)) {
                // FC4 c1: `&fn` — the address of a FUNCTION (C 6.5.3.2).
                // The function's code address is its GlobalAddr, typed
                // Ptr<FnSig> (what an `int (*fp)(int) = &helper;` init
                // stores/compares). Mirrors the rvalue Ref arm's
                // function case; calls THROUGH the pointer lower via
                // the indirect call-reg path (FC4 c2).
                TypeId const declared = hir.typeId(node);
                if (!declared.valid()) {
                    unsupported(node, std::format(
                        "function Ref to symbol {} has no type", sym));
                    return InvalidMirInst;
                }
                return mir.addGlobalAddr(SymbolId{sym},
                                         interner.pointer(declared));
            }
            unsupported(node, std::format(
                "symbol {} has no storage slot (non-addressable param or "
                "unbound) — required by lvalue use", sym));
            return InvalidMirInst;
        }
        if (k == HirKind::Deref) {
            auto kids = hir.children(node);
            if (kids.size() != 1) {
                unsupported(node, "malformed Deref as lvalue");
                return InvalidMirInst;
            }
            return lowerExpr(kids[0]);
        }
        if (k == HirKind::MemberAccess) {
            auto kids = hir.children(node);
            if (kids.size() != 1) {
                unsupported(node, "malformed MemberAccess (expect 1 child)");
                return InvalidMirInst;
            }
            MirInstId const basePtr = lowerLvalueAddress(kids[0]);
            if (!basePtr.valid()) return InvalidMirInst;
            return combineMemberAddr(node, basePtr);
        }
        if (k == HirKind::Index) {
            auto kids = hir.children(node);
            if (kids.size() != 2) {
                unsupported(node, "malformed Index (expect 2 children)");
                return InvalidMirInst;
            }
            // Pointer-base vs array/struct-base distinction:
            //   - pointer base (e.g. `int* p; p[i]`): the base's RVALUE is
            //     the pointer; GEP takes `[ptr, idx]`. We must NOT ask for
            //     the lvalue address — the pointer may be a pure-SSA Arg.
            //   - array/struct base (e.g. `int a[N]; a[i]`): we need the
            //     lvalue ADDRESS so GEP can index into the storage with
            //     `[basePtr, 0, idx]`.
            TypeId const baseTy = hir.typeId(kids[0]);
            TypeKind const baseKind = baseTy.valid()
                ? interner.kind(baseTy) : TypeKind::Void;
            TypeId const elemTy = hir.typeId(node);
            if (!elemTy.valid()) {
                unsupported(node, "Index with invalid element type");
                return InvalidMirInst;
            }
            TypeId const resTy = interner.pointer(elemTy);
            MirInstId const idx = lowerExpr(kids[1]);
            if (!idx.valid()) return InvalidMirInst;
            // Option A (D-MIR-STORAGE-ARRAY-INDEX-GEP, user §B 2026-06-17):
            // pre-scale the element index to a BYTE offset so the Gep index is
            // uniformly bytes (matching the struct-field const-disp form). Both
            // the pointer and storage arms funnel through scaleIndexToBytes —
            // the ONE site element scaling is applied (also fixes the latent
            // non-`char` `p[i]` scale-1 miscompile).
            TypeId const indexTy = hir.typeId(kids[1]);
            MirInstId const byteIdx =
                scaleIndexToBytes(idx, elemTy, node, indexTy);
            if (!byteIdx.valid()) return InvalidMirInst;
            if (baseKind == TypeKind::Ptr) {
                MirInstId const basePtr = lowerExpr(kids[0]);
                if (!basePtr.valid()) return InvalidMirInst;
                return combineIndexAddr(node, byteIdx, basePtr);
            }
            // Storage (array/struct) base: the lvalue address IS the base; the
            // vestigial leading `0` of the old 3-op form (stepping through the
            // pointer-to-array, contributing 0 bytes) is dropped — base+byteIdx
            // is exact. Both arms now emit the same 2-op byte-offset Gep; the
            // 3-op form is no longer produced (its LIR arm stays defensively
            // fail-loud).
            MirInstId const basePtr = lowerLvalueAddress(kids[0]);
            if (!basePtr.valid()) return InvalidMirInst;
            return combineIndexAddr(node, byteIdx, basePtr);
        }
        if (k == HirKind::Literal) {
            // F5 (agg_string_index): a STRING LITERAL in lvalue position — the
            // base of `"abc"[i]`. The array decays to its rodata address (C
            // 6.3.2.1): materialize the rodata global (the SAME producer the
            // value-position array→pointer decay uses) and return the address of
            // its first byte, typed as the array-pointer (matching a named global
            // array's lvalue address). The Index storage arm above then does
            // byte-offset arithmetic into it via combineIndexAddr. A non-string
            // literal is not an lvalue → materializeStringLiteralGlobal fails loud.
            return materializeStringLiteralGlobal(
                node, interner.pointer(hir.typeId(node)));
        }
        if (k == HirKind::ConstructAggregate) {
            // D-CSUBSET-BITFIELD-RVALUE-RUNTIME (the GENERAL aggregate-rvalue
            // carrier): a compound literal `(struct S){…}` / `(int[]){…}` IS an
            // lvalue (C11 6.5.2.5p4) — its storage is a unique unnamed object.
            // The compiler's aggregate model is memory-based (no LIR aggregate-
            // width value: every by-value consumer — by-value arg, by-value
            // return, assign-from-rvalue, member-of-literal — takes the
            // aggregate's ADDRESS through this very function), so the literal's
            // lvalue address is a synthesized SLOT we materialize here and
            // initialize IN PLACE, exactly as a named local's `= {…}` init does.
            // This covers struct/union/array AND bit-field/non-bit-field by
            // construction: the struct/union arm routes through
            // lowerAggregateInitIntoSlot, which already dispatches a bit-field
            // type to the per-allocation-unit packer
            // (lowerBitfieldAggregateInitIntoSlot — the cycle-4 packing
            // chokepoint), so a runtime bit-field literal packs correctly here.
            // Keys ONLY on HirKind + the type's kind (Struct/Union/Array) and
            // reuses the config-driven layout — no target/arch/format identity.
            TypeId const at = hir.typeId(node);
            if (!at.valid()) {
                unsupported(node, "ConstructAggregate rvalue has no type "
                                   "(HIR verifier should have flagged)");
                return InvalidMirInst;
            }
            TypeKind const ak = interner.kind(at);
            if (ak == TypeKind::Struct || ak == TypeKind::Union
                || ak == TypeKind::Array) {
                MirInstId const slot = freshAggregateTemp(at);
                if (!slot.valid()) {
                    unsupported(node, "aggregate compound-literal rvalue requires "
                                       "a sizeable layout (un-sizeable type or the "
                                       "target declared no aggregateLayout)");
                    return InvalidMirInst;
                }
                bool const ok = (ak == TypeKind::Array)
                    ? lowerArrayInitIntoSlot(node, slot, at)
                    : lowerAggregateInitIntoSlot(node, slot, at);
                if (!ok) return InvalidMirInst;   // fail-loud already reported
                return slot;
            }
            // A non-aggregate ConstructAggregate is a front-end invariant
            // violation (the brace-init target-type check rejects scalar
            // targets) — fall through to the fail-loud.
        }
        if (k == HirKind::Ternary) {
            // D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR: an AGGREGATE-typed
            // ternary `cond ? A : B` (A/B aggregate rvalues — compound
            // literals, named lvalues, calls, or nested ternaries) used BY
            // VALUE. Unlike the compound-literal arm above (one slot inited in
            // place), the VALUE is control-flow-dependent — so we materialize
            // each arm into ONE COMMON slot under a CFG diamond and return the
            // slot. There is NO Phi of the aggregate: the compiler's aggregate
            // model is memory-based (no LIR aggregate-width SSA value), so the
            // "merge" is a shared memory location, not an SSA Phi (a scalar
            // ternary in lowerExpr uses a Phi; an aggregate one cannot). Keys
            // ONLY on HirKind + the type's kind (Struct/Union/Array, or a wide
            // `_BitInt` — D-CSUBSET-BITINT-C2-WIDE, also memory-resident) + the
            // config-driven layout — no target/arch/format identity.
            TypeId const at = hir.typeId(node);
            if (isMemoryResidentType(interner, at)) {
                auto kids = hir.children(node);
                if (kids.size() != 3) {
                    unsupported(node, "malformed Ternary (expect 3 children)");
                    return InvalidMirInst;
                }
                // The common result slot is allocated ONCE, BEFORE the branch,
                // so both arms write the same storage and the join reads it.
                MirInstId const slot = freshAggregateTemp(at);
                if (!slot.valid()) {
                    unsupported(node, "aggregate-valued ternary requires a "
                                       "sizeable layout (un-sizeable type or the "
                                       "target declared no aggregateLayout)");
                    return InvalidMirInst;
                }
                MirInstId const cond = lowerExpr(kids[0]);
                if (!cond.valid()) return InvalidMirInst;
                // Mirror the scalar-ternary diamond's StructCfMarker usage
                // (IfThen/IfElse/IfJoin) so the verifier's count-pairing
                // invariant holds (markers pair by count, not edge polarity).
                MirBlockId const thenBB =
                    mir.createBlock(StructCfMarker::IfThen);
                MirBlockId const elseBB =
                    mir.createBlock(StructCfMarker::IfElse);
                MirBlockId const joinBB =
                    mir.createBlock(StructCfMarker::IfJoin);
                mir.addCondBr(cond, thenBB, elseBB);

                mir.beginBlock(thenBB);
                if (!materializeAggregateArmIntoSlot(kids[1], slot, at)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(elseBB);
                    sealCreatedAsUnreachable(joinBB);
                    return InvalidMirInst;   // fail-loud already reported
                }
                mir.addBr(joinBB);

                mir.beginBlock(elseBB);
                if (!materializeAggregateArmIntoSlot(kids[2], slot, at)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(joinBB);
                    return InvalidMirInst;   // fail-loud already reported
                }
                mir.addBr(joinBB);

                // The slot IS the result — NO Phi, no SSA aggregate value. The
                // by-value caller consumes the addr in the (now open) joinBB.
                mir.beginBlock(joinBB);
                return slot;
            }
            // A scalar-typed Ternary is not an lvalue → fall through to the
            // fail-loud (a scalar ternary lowers as an rvalue via lowerExpr).
        }
        if (k == HirKind::SeqExpr) {
            // D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR (comma operator): the
            // VALUE of a comma/SeqExpr is its result expression's value; when
            // that result is an aggregate used by value, its lvalue address is
            // the result expression's lvalue address. Run the side-effect
            // statements (exactly as the lowerExpr SeqExpr arm does), then
            // recurse — the aggregate result reaches the compound-literal /
            // ternary / named-lvalue arms. (A scalar SeqExpr is handled in
            // lowerExpr; here we only need the aggregate-by-address path, but
            // the recursion's own kind dispatch + fail-loud guards any
            // non-lvalue result.)
            for (HirNodeId stmt : hir.seqExprStmts(node)) {
                if (!lowerStmt(stmt)) return InvalidMirInst;
            }
            return lowerLvalueAddress(hir.seqExprResult(node));
        }
        if (k == HirKind::VaArg) {
            // FC12a-struct (D-FC12A-VARIADIC-CALLEE): an aggregate-typed
            // `va_arg(ap, struct S)` is realized BY ADDRESS — the eightbyte pieces
            // are gathered from the register-save-area (or the struct is read by
            // value from the overflow area) into a fresh temp whose ADDRESS is the
            // result. A SCALAR va_arg never reaches here (it lowers as an rvalue in
            // lowerExpr); reaching here with a non-aggregate type is an internal
            // invariant violation.
            TypeId const at = hir.typeId(node);
            TypeKind const ak = at.valid() ? interner.kind(at) : TypeKind::Void;
            if (ak == TypeKind::Struct || ak == TypeKind::Union) {
                return lowerVaArgAggregate(node);
            }
            // A NATIVE scalar va_arg (a <=64-bit type, incl. `_BitInt(N<=64)`) is an
            // rvalue lowered via lowerExpr — never by address — so reaching here means
            // the type is a wide `_BitInt(N>64)`: memory-resident, so it IS requested by
            // address, but its multi-limb by-address gather is a later cycle. Fail LOUD
            // either way (a genuine misroute of a native scalar, or the deferred wide
            // path) — never a silent low-limb-only read.
            unsupported(node,
                "internal: a va_arg reached lowerLvalueAddress that is neither a struct/"
                "union nor a native scalar — a native scalar va_arg is an rvalue lowered "
                "via lowerExpr, while a wide `_BitInt(N>64)` va_arg is memory-resident but "
                "its multi-limb by-address gather is a later cycle "
                "(D-FC12A-VARIADIC-CALLEE / D-CSUBSET-BITINT-C2-WIDE)");
            return InvalidMirInst;
        }
        // Any OTHER lvalue kind is still unsupported — fail loud (the
        // aggregate-valued Ternary/SeqExpr carriers above are
        // D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR; the compound-literal carrier
        // is the ConstructAggregate arm — D-CSUBSET-BITFIELD-RVALUE-RUNTIME).
        unsupported(node, std::format(
            "lvalue kind ordinal {} not supported by this lowering "
            "(only Ref-to-addressable-local, Deref, MemberAccess, Index, "
            "compound-literal ConstructAggregate, aggregate Ternary/SeqExpr)",
            static_cast<unsigned>(k)));
        return InvalidMirInst;
    }

    // D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR: materialize an aggregate ternary
    // ARM (`armNode`, of the ternary's type `aggTy`) INTO the common result
    // `slot`. A compound-literal arm is initialized in place (optimization: no
    // intermediate slot + copy — the same per-field/per-unit init a named
    // local's `= {…}` uses, which already routes a bit-field type through the
    // per-allocation-unit packer, so a bit-field literal arm packs correctly).
    // Any other aggregate arm (named lvalue, call, NESTED ternary, comma) is
    // resolved to an ADDRESS via lowerLvalueAddress (recursion handles the
    // nested cases) and COPIED into the slot — Struct/Union field/byte-wise,
    // Array byte-wise (consistent with lowerArrayInitIntoSlot's element copy).
    // Returns false (fail-loud already reported) on any failure.
    [[nodiscard]] bool materializeAggregateArmIntoSlot(HirNodeId armNode,
                                                       MirInstId slot,
                                                       TypeId aggTy) {
        TypeKind const ak = interner.kind(aggTy);
        if (hir.kind(armNode) == HirKind::ConstructAggregate) {
            return (ak == TypeKind::Array)
                ? lowerArrayInitIntoSlot(armNode, slot, aggTy)
                : lowerAggregateInitIntoSlot(armNode, slot, aggTy);
        }
        MirInstId const srcPtr = lowerLvalueAddress(armNode);
        if (!srcPtr.valid()) return false;
        if (ak == TypeKind::Array) {
            auto const sz = aggregateByteSize(aggTy);
            if (!sz.has_value()) {
                unsupported(armNode, "aggregate-valued ternary array arm "
                                      "requires a sizeable layout");
                return false;
            }
            return lowerByteWiseCopy(srcPtr, slot, *sz);
        }
        return lowerAggregateCopy(armNode, srcPtr, slot, aggTy);
    }

    // Emit a 32-bit integer constant for use as a GEP index operand or
    // similar inline scalar. The MIR has no built-in "integer constant
    // index" facility — every index in a GEP is just another MirInstId,
    // and these are usually `Const` instructions sourced from the MIR
    // literal pool. Returns `InvalidMirInst` if interning fails.
    [[nodiscard]] MirInstId constInt(std::int64_t v) {
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = TypeKind::I32;
        TypeId const i32 = interner.primitive(TypeKind::I32);
        return mir.addConst(std::move(lit), i32);
    }

    // An integer constant whose width matches `ty`'s integer kind. The
    // stride-scaling `Mul` (scaleIndexToBytes) must have same-width operands:
    // `constInt` is I32-only, so a 64-bit index (`long`/pointer subscript)
    // would otherwise form a mixed-width `Mul(idxI64, constI32)` the verifier
    // does not catch. `ty` must be an integer type (an array subscript is
    // integer per the front-end); falls back to I32 if absent.
    [[nodiscard]] MirInstId constIntOfType(std::int64_t v, TypeId ty) {
        // An enum-typed operand's constant IS its underlying integer value
        // (D-CSUBSET-ENUM-INT-CONVERSION) — resolve Enum→underlying so the
        // const is a real primitive (interner.primitive(Enum) is not valid).
        // This is the single chokepoint that also widths the enum-bit-field
        // init zero-store (D-CSUBSET-ENUM-BITFIELD).
        ty = enumReprType(ty);
        // D-CSUBSET-BITINT: a `_BitInt(N)`-typed constant IS its container integer
        // value — `interner.primitive(BitInt)` has no width scalar (the enum twin),
        // so resolve BitInt → its native container kind (I8/I16/I32/I64) before
        // materializing. Reached by the mask/shift-amount consts of an N≥17 wrap.
        if (ty.valid() && interner.kind(ty) == TypeKind::BitInt)
            ty = interner.primitive(interner.bitIntContainerKind(ty));
        TypeKind const k = ty.valid() ? interner.kind(ty) : TypeKind::I32;
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = k;
        return mir.addConst(std::move(lit), interner.primitive(k));
    }

    // VLA C3 (D-CSUBSET-VLA, MINOR-10): the ROOT VLA object SymbolId of a subscript
    // chain — peel the Index base `children[0]` down to the terminal `Ref`. `a[i]` →
    // `a`; `a[i][j]` → `a`. Returns an INVALID SymbolId if the base is anything OTHER
    // than a single VLA root symbol (a row via a ternary/cast/deref) so the caller
    // FAILS LOUD rather than form a bogus stride.
    [[nodiscard]] SymbolId vlaIndexRootSymbol(HirNodeId indexNode) const {
        HirNodeId cur = indexNode;
        for (int guard = 0; guard < 4096 && cur.valid(); ++guard) {
            HirKind const k = hir.kind(cur);
            if (k == HirKind::Index) {
                auto kids = hir.children(cur);
                if (kids.empty()) return SymbolId{};
                cur = kids[0];   // peel to the base
                continue;
            }
            if (k == HirKind::Ref) return SymbolId{hir.payload(cur)};
            return SymbolId{};   // not a single VLA root object
        }
        return SymbolId{};
    }

    // Pre-multiply an element index by its element STRIDE so the resulting
    // Gep index operand is a BYTE offset (Option A — agnostic MIR scaling,
    // D-MIR-STORAGE-ARRAY-INDEX-GEP, user §B 2026-06-17; the LIR `lea` keeps
    // scale=1). THIS is the single site both Index arms (pointer + storage)
    // funnel through, so element scaling is applied by construction at every
    // index (§A.5) — it also fixes the latent non-`char` `p[i]` scale-1
    // miscompile. `stride == 1` (a 1-byte element: char/byte/1-byte struct)
    // returns `idx` unchanged — no `imul`, and char indexing stays byte-
    // identical (no regression). `indexTy` widths the stride constant + the
    // Mul. InvalidMirInst (fail-loud already reported) on an un-sizeable
    // element type.
    [[nodiscard]] MirInstId scaleIndexToBytes(MirInstId idx, TypeId elemTy,
                                              HirNodeId node, TypeId indexTy) {
        // VLA C3 (CRITICAL-1): a VLA-ROW element (the subscript RESULT type contains a
        // VLA — `int a[n][m]`'s `a[i]` yields the `int[m]` row, `int a[5][n]`'s `a[i]`
        // yields the `int[n]` row) has a RUNTIME stride = the per-object byte size
        // frozen at the VLA's decl. Load the `(root-sym, elemTy)` slot rather than a
        // compile-time `elementStride` (which nullopts on a VLA element). `elemTy ==
        // hir.typeId(node)` by construction (both Index arms pass the Index node's
        // type), so the key matches EXACTLY what `vlaAllocaForLocal` stored — no depth
        // arithmetic, no off-by-one. A FIXED row (`int a[n][5]`'s `int[5]`) is NOT a
        // VLA container → falls through to the compile-time path below.
        if (interner.typeContainsVla(elemTy)) {
            SymbolId const root = vlaIndexRootSymbol(node);
            if (!root.valid()) {
                unsupported(node, "variable-length array subscript base is not a single "
                                  "VLA object (e.g. a row via a ternary/cast) — its "
                                  "runtime row stride cannot be recovered");
                return InvalidMirInst;
            }
            auto const it = vlaStrideSlot.find(vlaSlotKey(root.v, elemTy.v));
            if (it == vlaStrideSlot.end()) {
                // A DECLARED VLA object's decl materializes its stride slots and
                // dominates every use, so a missing slot here means the subscript base
                // is NOT such an object — realistically a pointer-to-VLA
                // (`int (*p)[m]; p[i]`), whose runtime row stride C3 does not yet track.
                // Fail loud with a real diagnostic (deferred, C4), never a guessed stride.
                unsupported(node, "a pointer-to-variable-length-array subscript (or a "
                                  "similar VLA row not reached as a declared VLA object) "
                                  "is not yet supported — its runtime row stride is not "
                                  "tracked (deferred, D-CSUBSET-VLA)");
                return InvalidMirInst;
            }
            TypeId const i64ty = i64Ty();
            std::array<MirInstId, 1> ld{it->second};
            MirInstId const stride = mir.addInst(MirOpcode::Load, ld, i64ty);
            if (!stride.valid()) return InvalidMirInst;
            // Widen the index to i64 so the byte-offset Mul matches the i64 runtime
            // stride (a truncation to a narrower `indexTy` would drop the high bits of
            // a large row size). The subscript was already integer-promoted in HIR.
            MirInstId idx64 = idx;
            if (!(indexTy.valid() && interner.kind(indexTy) == TypeKind::I64)) {
                TypeKind const fromK =
                    indexTy.valid() ? resolveScalarIntKind(indexTy) : TypeKind::I32;
                if (fromK != TypeKind::I64) {
                    MirOpcode const ext = mapCast(fromK, TypeKind::I64);
                    if (ext == MirOpcode::Invalid) {
                        unsupported(node, "variable-length array subscript index has a "
                                          "non-integer type with no widening to i64");
                        return InvalidMirInst;
                    }
                    std::array<MirInstId, 1> eo{idx};
                    idx64 = mir.addInst(ext, eo, i64ty);
                    if (!idx64.valid()) return InvalidMirInst;
                }
            }
            std::array<MirInstId, 2> ops{idx64, stride};
            return mir.addInst(MirOpcode::Mul, ops, i64ty);
        }
        std::optional<std::uint64_t> const stride = elementStride(elemTy);
        if (!stride.has_value()) {
            unsupported(node, "array/pointer index element type has no "
                              "computable size (incomplete type or the target "
                              "declared no aggregateLayout)");
            return InvalidMirInst;
        }
        if (*stride == 0) {
            // A zero-size element (e.g. an empty aggregate `struct E {}`) would
            // make every index alias byte offset 0 — fail loud, never a silent
            // `Mul(idx, 0)` miscompile.
            unsupported(node, "array/pointer index element type has zero size "
                              "(empty aggregate) — cannot form a byte offset");
            return InvalidMirInst;
        }
        if (*stride == 1) return idx;  // 1-byte element: index already IS bytes
        MirInstId const strideK =
            constIntOfType(static_cast<std::int64_t>(*stride), indexTy);
        if (!strideK.valid()) return InvalidMirInst;
        std::array<MirInstId, 2> ops{idx, strideK};
        return mir.addInst(MirOpcode::Mul, ops, indexTy);
    }

    // (Removed — D-CSUBSET-BITFIELD-RVALUE-RUNTIME) The former
    // `lowerConstructAggregate` (a `Const(zero)` seed + an `InsertValue` chain)
    // and its recursive `zeroLiteralOf` seed-builder are gone. The aggregate
    // model is MEMORY-based: there is no LIR aggregate-width SSA value, and a
    // bit-field packs only via a unit read-modify-write (impossible on an SSA
    // aggregate). Every aggregate ConstructAggregate is now lowered into a SLOT
    // by ADDRESS — a LOCAL `= {…}` init via lowerAggregateInitIntoSlot/
    // lowerArrayInitIntoSlot, a GLOBAL `= {…}` init via evaluateConstant + the
    // static-data byte encoder (emitGlobals_), and a by-value RVALUE (a compound
    // literal in arg/return/assign/member-of-literal position) via the
    // lowerLvalueAddress compound-literal arm. The lowerExpr ConstructAggregate
    // dispatch arm now fails loud against any attempt to lower an aggregate as a
    // bare SSA rvalue (anti-resurrection invariant). `evaluateConstant`/
    // `toMirLiteral` survive — globals still fold through them.

    // FC8 D-CSUBSET-BITFIELD: true iff `ty` is a struct/union WITH a bit-field
    // (non-empty scalars under the structType encoding). Aggregate INITIALIZATION
    // of such a type is bit-field-AWARE (D-CSUBSET-BITFIELD-INIT): a local slot
    // init packs per allocation unit (lowerBitfieldAggregateInitIntoSlot), a
    // const global folds to raw per-field values that the static-data byte
    // encoder packs, and a by-value RVALUE literal (a compound literal with
    // runtime operands) materializes a slot and packs the SAME way via the
    // lowerLvalueAddress compound-literal arm (D-CSUBSET-BITFIELD-RVALUE-RUNTIME).
    [[nodiscard]] bool hasBitfieldMember(TypeId ty) const {
        if (!ty.valid()) return false;
        TypeKind const k = interner.kind(ty);
        return (k == TypeKind::Struct || k == TypeKind::Union)
               && !interner.scalars(ty).empty();
    }

    // FC8 D-CSUBSET-BITFIELD-INIT: lower a struct/union aggregate initializer
    // whose type HAS bit-fields into the slot at `allocaPtr`. A bit-field shares
    // its allocation UNIT with co-resident neighbours, so a plain full-width
    // Store would clobber them — instead each unit is packed: zeroed ONCE (so
    // un-covered bits + omitted fields are zero-filled, matching C aggregate
    // init), then each bit-field's value is OR-ed in via the SAME read-modify-
    // write `emitBitfieldInsert` the field-by-field assignment path uses (the
    // single packing chokepoint — write+read stay self-consistent on every
    // target). Ordinary fields among the bit-fields lower normally (Gep+Store,
    // or nested aggregate recurse). `lowerBraceInit` has zero-filled unspecified
    // fields, so `kids` is positional + complete; a zero-width bit-field marker
    // (`bitFields[i].unitBytes == 0` AND not an ordinary field) carries no value
    // child, so it is skipped by the parallel walk. Source/target-agnostic: keys
    // only on the config-driven layout's `bitFields[]` + `fieldOffsets[]`.
    [[nodiscard]] bool lowerBitfieldAggregateInitIntoSlot(HirNodeId aggNode,
                                                          MirInstId allocaPtr,
                                                          TypeId aggTy,
                                                          MirInstFlags vf = MirInstFlags::None) {
        // c27: `vf` Volatile when the destination aggregate is `volatile` — every
        // unit-zero, ordinary-field Store, and bit-field RMW carries the flag.
        StructLayout const* layout = cachedLayout(aggTy);
        if (layout == nullptr) {
            unsupported(aggNode, "bit-field aggregate initializer: layout "
                                 "unavailable (un-sizeable type or the target "
                                 "declared no aggregateLayout)");
            return false;
        }
        auto kids = hir.children(aggNode);
        // `lowerBraceInit` emits ONE positional child per struct field (omitted
        // slots already zero-filled), so `kids[f]` ↔ field `f` directly — INCLUDING
        // a zero-width bit-field (`unsigned : 0;`), which is a layout-only packing
        // break carrying a synthetic zero child. A zero-width marker has no storage
        // (`bitFields[f].unitBytes == 0` AND `fieldBitWidth(f)` present), so it is
        // skipped: it touches no unit and its child is an unwritten zero.
        std::size_t const fieldCount = layout->fieldOffsets.size() < kids.size()
                                           ? layout->fieldOffsets.size()
                                           : kids.size();
        // PASS 1 — zero every bit-field allocation unit BEFORE any field value is
        // written. A non-bit-field member can share an allocation unit's byte
        // range with a bit-field (C/GNU packing: `struct { char x; int a:3; }`
        // puts `x` at byte 0 and `a`'s int unit at bytes [0,4)), so zeroing a
        // unit lazily in declaration order would clobber an ordinary field
        // already stored into the same bytes — AND diverge from the static-data
        // encoder, which pre-zeroes the whole buffer once (`asm.cpp`). Hoisting
        // all unit-zeroes ahead of all value writes makes the result
        // order-independent: ordinary Stores and bit-field read-modify-writes
        // then all land on already-zeroed bytes (the RMW preserves the zero in
        // the bits it does not set, ordinary fields occupy disjoint bytes). Dedup
        // on (offset, unitBytes) so a unit is zeroed once, yet mixed-width
        // bit-fields sharing an offset (`char a:3; int b:4;` → both at offset 0,
        // units 1 and 4 bytes) each get their full unit covered.
        std::unordered_set<std::uint64_t> zeroedUnits;
        for (std::size_t f = 0; f < fieldCount; ++f) {
            bool const isBitfield =
                f < layout->bitFields.size() && layout->bitFields[f].unitBytes != 0;
            if (!isBitfield) continue;
            auto const          off       = layout->fieldOffsets[f];
            std::uint32_t const unitBytes = layout->bitFields[f].unitBytes;
            // off is a byte offset within a composite (« 4 GiB) — pack
            // (off, unitBytes) into one key so a wider unit at the same offset is
            // not skipped by a narrower one's earlier zero.
            if (!zeroedUnits.insert((off << 32) | unitBytes).second) continue;
            TypeId const    fieldTy = hir.typeId(kids[f]);
            MirInstId const offK    = constInt(static_cast<std::int64_t>(off));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> gepOps{allocaPtr, offK};
            MirInstId const          unitPtr =
                mir.addInst(MirOpcode::Gep, gepOps, interner.pointer(fieldTy));
            if (!unitPtr.valid()) return false;
            MirInstId const zero = constIntOfType(0, fieldTy);
            if (!zero.valid()) return false;
            std::array<MirInstId, 2> zst{zero, unitPtr};
            mir.addInst(MirOpcode::Store, zst, InvalidType, /*payload=*/0, vf);
        }
        // PASS 2 — write every field. Bit-fields read-modify-write their (now
        // zeroed) unit; ordinary fields store/recurse; zero-width markers carry
        // no storage. All zeroing is done, so no write can clobber another.
        for (std::size_t f = 0; f < fieldCount; ++f) {
            bool const isBitfield =
                f < layout->bitFields.size() && layout->bitFields[f].unitBytes != 0;
            bool const isZeroWidthMarker =
                !isBitfield && interner.fieldBitWidth(aggTy, f).has_value();
            if (isZeroWidthMarker) continue;   // no storage; child is a zero-fill
            HirNodeId const child = kids[f];
            auto const off = layout->fieldOffsets[f];
            MirInstId const offK = constInt(static_cast<std::int64_t>(off));
            if (!offK.valid()) return false;
            TypeId const fieldTy = hir.typeId(child);
            if (!isBitfield) {
                // Ordinary field: Gep to its byte offset, then store/recurse
                // exactly as the non-bit-field path does (a struct/union/array
                // field recurses; a scalar field is a plain Store).
                std::array<MirInstId, 2> gepOps{allocaPtr, offK};
                MirInstId const fieldPtr = mir.addInst(
                    MirOpcode::Gep, gepOps, interner.pointer(fieldTy));
                if (!fieldPtr.valid()) return false;
                TypeKind const fk = interner.kind(fieldTy);
                // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` field is memory-resident
                // (like a struct/union field) — it copies from the init value's
                // ADDRESS, never a scalar Store of a (pointer-width) rvalue.
                if (isByValueClass(interner, fieldTy)) {
                    if (fk != TypeKind::BitInt
                        && hir.kind(child) == HirKind::ConstructAggregate) {
                        if (!lowerAggregateInitIntoSlot(child, fieldPtr, fieldTy, vf))
                            return false;
                    } else {
                        MirInstId const srcPtr = lowerLvalueAddress(child);
                        if (!srcPtr.valid()) return false;
                        if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy, vf))
                            return false;
                    }
                    continue;
                }
                if (fk == TypeKind::Array) {
                    if (hir.kind(child) == HirKind::ConstructAggregate) {
                        if (!lowerArrayInitIntoSlot(child, fieldPtr, fieldTy, vf))
                            return false;
                    } else {
                        MirInstId const srcPtr = lowerLvalueAddress(child);
                        if (!srcPtr.valid()) return false;
                        if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy, vf))
                            return false;
                    }
                    continue;
                }
                MirInstId const v = lowerExpr(child);
                if (!v.valid()) return false;
                std::array<MirInstId, 2> stOps{v, fieldPtr};
                // c27: dest-aggregate volatility OR this field's own type volatility.
                mir.addInst(MirOpcode::Store, stOps, InvalidType, /*payload=*/0,
                            vf | volatileFlagForType(fieldTy));
                continue;
            }
            // Bit-field: Gep to the UNIT (already zeroed in pass 1), then RMW the
            // value in. The Gep width follows the field type = the unit width,
            // matching emitBitfieldInsert's load/store width.
            std::array<MirInstId, 2> gepOps{allocaPtr, offK};
            MirInstId const unitPtr = mir.addInst(
                MirOpcode::Gep, gepOps, interner.pointer(fieldTy));
            if (!unitPtr.valid()) return false;
            MirInstId v = lowerExpr(child);
            if (!v.valid()) return false;
            // c73: coerce the init value to the field type (like an assignment RHS)
            // so emitBitfieldInsert's RMW sees a field-typed rhs — its sub-int
            // promote path widens FROM the field type. Same-kind → no cast.
            {
                TypeKind const ck = interner.kind(hir.typeId(child));
                if (ck != interner.kind(fieldTy)) {
                    MirOpcode const cc = mapCast(ck, interner.kind(fieldTy));
                    if (cc != MirOpcode::Invalid) {
                        std::array<MirInstId, 1> va{v};
                        v = mir.addInst(cc, va, fieldTy);
                    }
                }
            }
            if (!emitBitfieldInsert(unitPtr, v, layout->bitFields[f], fieldTy, vf))
                return false;
        }
        return true;
    }

    // FC7 (D-FC7-MEMBER-ACCESS): lower a struct/union ConstructAggregate
    // initializer ELEMENT-WISE into the slot at `allocaPtr` — one
    // `Store(lowerExpr(child_i), Gep(allocaPtr, Const(fieldByteOffset_i)))`
    // per field. This avoids materializing an aggregate SSA value (there is
    // no LIR aggregate-width Store / non-zero InsertValue). `lowerBraceInit`
    // has already zero-filled unspecified fields, so the child list is
    // positional + complete; a UNION carries one child written at offset 0.
    // Returns false (fail-loud already reported) on any failure.
    [[nodiscard]] bool lowerAggregateInitIntoSlot(HirNodeId aggNode,
                                                  MirInstId allocaPtr,
                                                  TypeId aggTy,
                                                  MirInstFlags vf = MirInstFlags::None) {
        // c27 (D-CSUBSET-VOLATILE-POINTEE): `vf` is Volatile when the DESTINATION
        // aggregate is `volatile`-qualified (`volatile struct S s = {…}` — C
        // 6.7.3p5: every member write of a volatile object is a volatile access).
        // The VarDecl site passes the object's volatility; structural sub-recursion
        // propagates it to nested fields/elements. A non-volatile init keeps None.
        // FC8 D-CSUBSET-BITFIELD-INIT: a struct/union initializer whose type has
        // bit-fields packs per allocation unit — a plain field-wise Store would
        // write each bit-field full-width into the shared unit, clobbering its
        // co-resident neighbours. Routed to the unit-aware initializer.
        if (hasBitfieldMember(aggTy)) {
            return lowerBitfieldAggregateInitIntoSlot(aggNode, allocaPtr, aggTy, vf);
        }
        // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): brace-initializing an explicit-offset
        // (overlapping) struct would positionally Store each field, so a later field
        // silently clobbers an earlier one that shares its bytes — a wrong-but-runs
        // aggregate. An FFI overlap type (ULARGE_INTEGER) is only ever member-assigned
        // (overlap-immune, indexed offsets), never brace-inited; refuse LOUD rather
        // than miscompile. (Materially strip volatile via the interner's own view.)
        if (interner.hasExplicitOffsets(aggTy)) {
            unsupported(aggNode, "brace-initialization of an overlapping "
                                 "explicit-offset struct is unsupported — its members "
                                 "share bytes; assign the members individually");
            return false;
        }
        auto kids = hir.children(aggNode);
        for (std::size_t i = 0; i < kids.size(); ++i) {
            auto const off =
                fieldByteOffset(aggTy, static_cast<std::uint32_t>(i));
            if (!off.has_value()) {
                unsupported(aggNode, "aggregate initializer field-offset "
                                      "resolution failed (un-sizeable layout "
                                      "or field-count mismatch)");
                return false;
            }
            MirInstId const offK = constInt(static_cast<std::int64_t>(*off));
            if (!offK.valid()) return false;
            TypeId const fieldTy = hir.typeId(kids[i]);
            std::array<MirInstId, 2> gepOps{allocaPtr, offK};
            MirInstId const fieldPtr = mir.addInst(
                MirOpcode::Gep, gepOps, interner.pointer(fieldTy));
            if (!fieldPtr.valid()) return false;

            // FC7 D-FC7-NESTED-STRUCT-FIELD: a struct/union-TYPED field
            // whose initializer is itself a brace-init lowers to a nested
            // ConstructAggregate; recurse ELEMENT-WISE into the field's
            // sub-slot at `fieldPtr` (there is no aggregate-width Store, so
            // the scalar path below cannot realize it). An aggregate field
            // initialized from an aggregate VALUE (not a brace-init) is a
            // field-into-field copy — that needs the byte/field-wise
            // aggregate copy, fail loud (D-FC7-AGGREGATE-COPY-MEMCPY).
            TypeKind const fk = interner.kind(fieldTy);
            // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` field copies from the init
            // value's ADDRESS (memory-resident), never a scalar Store below.
            if (isByValueClass(interner, fieldTy)) {
                if (fk != TypeKind::BitInt
                    && hir.kind(kids[i]) == HirKind::ConstructAggregate) {
                    if (!lowerAggregateInitIntoSlot(kids[i], fieldPtr, fieldTy, vf))
                        return false;
                    continue;
                }
                // FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): an aggregate field
                // initialized from an aggregate VALUE (`{ existingStruct, … }`,
                // not a nested brace) is a copy from that value's lvalue
                // address into the field's sub-slot.
                MirInstId const srcPtr = lowerLvalueAddress(kids[i]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy, vf))
                    return false;
                continue;
            }

            // D-MIR-ARRAY-FIELD-AGGREGATE-INIT: an ARRAY-typed field. Array
            // element offsets are `j·stride` (not struct fieldOffsets), so it
            // routes to lowerArrayInitIntoSlot — a brace-init recurses element-
            // wise; an array VALUE copies byte-wise (D-FC7-AGGREGATE-COPY).
            if (fk == TypeKind::Array) {
                if (hir.kind(kids[i]) == HirKind::ConstructAggregate) {
                    if (!lowerArrayInitIntoSlot(kids[i], fieldPtr, fieldTy, vf))
                        return false;
                    continue;
                }
                MirInstId const srcPtr = lowerLvalueAddress(kids[i]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy, vf))
                    return false;
                continue;
            }

            MirInstId const v = lowerExpr(kids[i]);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> stOps{v, fieldPtr};
            // c27: flag if the dest aggregate is volatile (`vf`) OR this FIELD's own
            // type is volatile (`struct { volatile int m; }` — `m`'s init is a
            // volatile write even when the container is plain).
            mir.addInst(MirOpcode::Store, stOps, InvalidType, /*payload=*/0,
                        vf | volatileFlagForType(fieldTy));
        }
        return true;
    }

    // D-MIR-ARRAY-FIELD-AGGREGATE-INIT: lower an ARRAY ConstructAggregate
    // initializer ELEMENT-WISE into the slot at `basePtr` — one Store (or
    // nested recurse) per element at the CONSTANT byte offset `j·stride` (init
    // indices are compile-time, so the offset is a const-disp Gep, already
    // lowered). Mutually recursive with lowerAggregateInitIntoSlot (a struct/
    // union element) and itself (a multi-dimensional array element).
    // `lowerBraceInit` has zero-filled unspecified elements, so the child list
    // is positional + complete. Returns false (fail-loud already reported) on
    // any failure. Wired at both array-init sites — the array field recurse-
    // guard above AND the VarDecl array-local arm — so every array brace-init
    // funnels through this one helper (§A.5 by-construction coverage).
    [[nodiscard]] bool lowerArrayInitIntoSlot(HirNodeId arrNode,
                                              MirInstId basePtr,
                                              TypeId /*arrTy*/,
                                              MirInstFlags vf = MirInstFlags::None) {
        // c27: `vf` Volatile when the destination array is `volatile`-qualified
        // (`volatile int va[] = {…}`); propagated to nested element inits.
        auto kids = hir.children(arrNode);
        for (std::size_t j = 0; j < kids.size(); ++j) {
            TypeId const elemTy = hir.typeId(kids[j]);
            std::optional<std::uint64_t> const stride = elementStride(elemTy);
            if (!stride.has_value()) {
                unsupported(arrNode, "array initializer element type has no "
                                      "computable size");
                return false;
            }
            std::uint64_t const off = j * *stride;
            MirInstId const offK = constInt(static_cast<std::int64_t>(off));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> gepOps{basePtr, offK};
            MirInstId const elemPtr = mir.addInst(
                MirOpcode::Gep, gepOps, interner.pointer(elemTy));
            if (!elemPtr.valid()) return false;

            TypeKind const ek = interner.kind(elemTy);
            // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` array element copies from the
            // init value's ADDRESS (memory-resident), never a scalar Store below.
            if (isByValueClass(interner, elemTy)) {
                if (ek != TypeKind::BitInt
                    && hir.kind(kids[j]) == HirKind::ConstructAggregate) {
                    if (!lowerAggregateInitIntoSlot(kids[j], elemPtr, elemTy, vf))
                        return false;
                    continue;
                }
                MirInstId const srcPtr = lowerLvalueAddress(kids[j]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(arrNode, srcPtr, elemPtr, elemTy, vf))
                    return false;
                continue;
            }
            if (ek == TypeKind::Array) {
                if (hir.kind(kids[j]) == HirKind::ConstructAggregate) {
                    if (!lowerArrayInitIntoSlot(kids[j], elemPtr, elemTy, vf))
                        return false;
                    continue;
                }
                MirInstId const srcPtr = lowerLvalueAddress(kids[j]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(arrNode, srcPtr, elemPtr, elemTy, vf))
                    return false;
                continue;
            }

            MirInstId const v = lowerExpr(kids[j]);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> stOps{v, elemPtr};
            // c27: flag the store if the dest array is volatile (`vf`) OR the
            // ELEMENT type is volatile (`volatile int va[]` distributes the
            // qualifier to the element type `VolatileQual(int)` — the array itself
            // is NOT top-level-qualified, so `vf` alone would miss it).
            mir.addInst(MirOpcode::Store, stOps, InvalidType, /*payload=*/0,
                        vf | volatileFlagForType(elemTy));
        }
        return true;
    }

    // FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): copy `size` bytes from `srcPtr` to
    // `dstPtr` BYTE-WISE, using the widest available integer chunk at each
    // step — I64 (8B), then one I32 (4B), then `Char` (1B) for the 0–3 byte
    // tail. Each chunk's memory access is now width-EXACT to its type
    // (D-LIR-INT-MEMORY-WIDTH-EXACT — memAccessWidthFlags: Char/I8/U8/Bool→8,
    // I16/U16→16, I32/U32→32, else→64), so a chunk touches EXACTLY its bytes
    // and never overruns the object (the I32 tail of a 12-byte struct now
    // reads/writes bytes 8–11, not 8–15 — a latent overrun this width-
    // exactness also fixed). `Char` stays the 1-byte tail vehicle (a width-16
    // chunk for a 2-byte tail is a possible future optimization, not a
    // correctness need). Each chunk loads (zero/full-extends) then stores
    // the low width, round-tripping its bytes exactly. Chunk accesses may be
    // unaligned — fine on x86_64 and AArch64 (normal mov/LDR/STR). The shared
    // offset Const feeds both the src and dst GEP (immutable SSA). Returns
    // false on any failure (diagnostic already emitted upstream).
    [[nodiscard]] bool lowerByteWiseCopy(MirInstId srcPtr, MirInstId dstPtr,
                                         std::uint64_t size,
                                         MirInstFlags vf = MirInstFlags::None) {
        // c21 (D-CSUBSET-VOLATILE-QUALIFIER): `vf` is Volatile only for a
        // WHOLE-VOLATILE-AGGREGATE user copy (`volatile struct S a = b; a = b;` —
        // every member access of a volatile aggregate is volatile, C 6.7.3), passed
        // by the AssignStmt/VarDecl-init callers. Every STRUCTURAL caller (brace-
        // init field copy, by-value arg/return, sret) keeps the default None.
        auto emit = [&](TypeKind chunkKind, std::uint64_t off) -> bool {
            TypeId const chunkTy = interner.primitive(chunkKind);
            TypeId const ptrTy   = interner.pointer(chunkTy);
            MirInstId const offK = constInt(static_cast<std::int64_t>(off));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> sg{srcPtr, offK};
            MirInstId const sp = mir.addInst(MirOpcode::Gep, sg, ptrTy);
            if (!sp.valid()) return false;
            std::array<MirInstId, 1> ld{sp};
            MirInstId const v = mir.addInst(MirOpcode::Load, ld, chunkTy,
                                            /*payload=*/0, vf);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> dg{dstPtr, offK};
            MirInstId const dp = mir.addInst(MirOpcode::Gep, dg, ptrTy);
            if (!dp.valid()) return false;
            std::array<MirInstId, 2> st{v, dp};
            mir.addInst(MirOpcode::Store, st, InvalidType, /*payload=*/0, vf);
            return true;
        };
        std::uint64_t off = 0;
        for (; off + 8 <= size; off += 8)
            if (!emit(TypeKind::I64, off)) return false;
        if (off + 4 <= size) {
            if (!emit(TypeKind::I32, off)) return false;
            off += 4;
        }
        for (; off < size; off += 1)
            if (!emit(TypeKind::Char, off)) return false;
        return true;
    }

    // FC7: copy an aggregate value from `srcPtr` to `dstPtr`.
    //   • flat scalar STRUCT (no aggregate field) → FIELD-WISE: one Load+Store
    //     per scalar field at its FC6 offset (width-exact, no padding copy —
    //     the D-FC7-MEMBER-ACCESS path; an aggregate-width Load would TRUNCATE
    //     a wider-than-register struct to its low bytes).
    //   • UNION, or a struct with ANY aggregate field (struct/union/ARRAY) →
    //     BYTE-WISE whole-object copy (D-FC7-AGGREGATE-COPY-MEMCPY): a union's
    //     variants overlap so a field-wise copy of one misses the others'
    //     bytes; an aggregate-typed field can't be realized by a single
    //     field Load. Byte-wise copies the entire object representation
    //     (incl. padding — C 6.2.6.1, harmless), covering every nested byte.
    // Returns false on any failure (diagnostic already emitted).
    [[nodiscard]] bool lowerAggregateCopy(HirNodeId atNode, MirInstId srcPtr,
                                          MirInstId dstPtr, TypeId aggTy,
                                          MirInstFlags vf = MirInstFlags::None) {
        // c21 (D-CSUBSET-VOLATILE-QUALIFIER): `vf` is Volatile only when the user
        // copy target/source is a WHOLE-VOLATILE aggregate (passed by the
        // AssignStmt/VarDecl-init sites); structural callers default None.
        StructLayout const* layout = cachedLayout(aggTy);
        if (layout == nullptr) {
            unsupported(atNode, "aggregate copy requires a sizeable layout "
                                "(target 'aggregateLayout' / complete type)");
            return false;
        }
        TypeKind const aggKind = interner.kind(aggTy);
        if (aggKind == TypeKind::Union)
            return lowerByteWiseCopy(srcPtr, dstPtr, layout->size, vf);
        // c62 (C 6.7.9p14, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL): a top-level
        // ARRAY value copy — `char x[7] = "hi";` copies the N-byte rodata global
        // (zero-padded at the producer) into the stack slot. `layout->size` is the
        // array's full byte extent (computeLayout sizes any type incl. arrays), so
        // a byte-wise copy moves exactly N bytes — the array twin of the Union arm.
        if (aggKind == TypeKind::Array)
            return lowerByteWiseCopy(srcPtr, dstPtr, layout->size, vf);
        // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt(N>64)` is a multi-limb MEMORY
        // object with no field list — its copy is a whole-object byte-wise copy of
        // ceil(N/64)*8 bytes (the twin of the Union/Array arm). A scalar (aggregate-
        // width) Load+Store would move only the low 8 bytes → a silent partial copy.
        if (isWideBitInt(interner, aggTy))
            return lowerByteWiseCopy(srcPtr, dstPtr, layout->size, vf);
        if (aggKind != TypeKind::Struct) {
            unsupported(atNode, "aggregate copy of a non-struct/union/array value "
                                "is not supported");
            return false;
        }
        // COPY the field types out: `operands(composite)` returns a SPAN into the
        // interner's composite field side-table (c24 nominal typing; previously
        // operandPool_), and `interner.pointer(...)` in the field-copy loop below
        // interns fresh `Ptr<…>` types — a mint bumps the interner generation and
        // can move backing storage, dangling a retained span (a host-STL-growth-
        // dependent heap-use-after-free; non-Windows release legs misread the
        // freed bytes as an invalid Load result-type → MirBuilder fatal). The
        // owning vector is immune. Twin of the FC7-C1c fix at the function-param
        // setup below.
        std::vector<TypeId> const fieldTypes = [&] {
            auto const s = interner.operands(aggTy);
            return std::vector<TypeId>(s.begin(), s.end());
        }();
        if (fieldTypes.size() != layout->fieldOffsets.size()) {
            unsupported(atNode, "struct copy: field-type count mismatches the "
                                "layout field-offset count (internal invariant)");
            return false;
        }
        // CRIT-B (D-CSUBSET-BITINT-C2-WIDE): a struct with ANY memory-resident field
        // (struct / union / array / a wide `_BitInt` member) copies BYTE-WISE. A wide-
        // BitInt field falls through the flat-scalar field-wise Load (16+ bytes read
        // as one 8-byte value) → a silent partial copy, so it must join the byte-wise
        // branch. `isMemoryResidentType` includes the wide-BitInt case by construction.
        bool anyAggregateField = false;
        for (TypeId ft : fieldTypes) {
            if (isMemoryResidentType(interner, ft)) {
                anyAggregateField = true;
                break;
            }
        }
        if (anyAggregateField)
            return lowerByteWiseCopy(srcPtr, dstPtr, layout->size, vf);

        // Flat scalar struct — field-wise, width-exact. c107: an explicit-offset
        // (overlapping) struct needs NO guard here — unlike brace-init/static-encode
        // (which write distinct per-field VALUES and would clobber), a by-value COPY
        // reads one coherent source and re-writes any overlapping byte identically,
        // so the redundant writes are byte-equal and the copy is correct.
        for (std::size_t i = 0; i < fieldTypes.size(); ++i) {
            MirInstId const offK = constInt(
                static_cast<std::int64_t>(layout->fieldOffsets[i]));
            if (!offK.valid()) return false;
            TypeId const fptrTy = interner.pointer(fieldTypes[i]);
            std::array<MirInstId, 2> srcGep{srcPtr, offK};
            MirInstId const srcField =
                mir.addInst(MirOpcode::Gep, srcGep, fptrTy);
            if (!srcField.valid()) return false;
            std::array<MirInstId, 1> loadOps{srcField};
            MirInstId const v =
                mir.addInst(MirOpcode::Load, loadOps, fieldTypes[i],
                            /*payload=*/0, vf);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> dstGep{dstPtr, offK};
            MirInstId const dstField =
                mir.addInst(MirOpcode::Gep, dstGep, fptrTy);
            if (!dstField.valid()) return false;
            std::array<MirInstId, 2> stOps{v, dstField};
            mir.addInst(MirOpcode::Store, stOps, InvalidType, /*payload=*/0, vf);
        }
        return true;
    }

    // ── FC7 by-value aggregate ABI synthesis (D-FC7-STRUCT-BY-VALUE-ARG-RETURN) ──
    // §B-locked at HIR→MIR: a struct/union passed BY VALUE is classified via the
    // `aggregate_abi` engine and synthesized into scalar register pieces (each an
    // ordinary I64/F64 MIR arg the UNCHANGED callconv places by class) or a
    // by-reference pointer; the callee reconstructs into the param's frame slot.
    // Both sides derive the SAME piece sequence from (type × CC), so the caller's
    // operand order matches the callee's Arg ordinals by construction. Struct
    // RETURNS by value stay fail-loud this phase (sret needs the multi-register
    // MIR Return — D-FC7-SYSV-STRUCT-RETURN-IN-REGS).

    // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG): the running physical-arg ordinal.
    // INDEPENDENT CCs (SysV/AAPCS64, `slotAligned=false`) count GPR and FPR
    // ordinals SEPARATELY (the `Arg` payload is the per-class register index);
    // a SLOT-ALIGNED CC (Win64) uses one FLAT shared slot. Monotonic per class,
    // so a struct's multiple pieces land in consecutive registers and no two
    // Args of the same class collide. (Threading per-class here also fixes the
    // latent mixed-class `D-ML7-2.10`: a scalar param now gets its per-class
    // index, not the param index.)
    struct ArgOrdinalCounter {
        bool          slotAligned = false;
        std::uint32_t flat = 0, gpr = 0, fpr = 0;
        // The FLAT call-operand POSITION (arg_payload.hpp) — the index of this
        // parameter's value in the CALL's actual-argument operand list
        // (operands[1 + position]). Advances once PER CALL OPERAND: once per
        // scalar Arg, once per aggregate register PIECE, and — crucially — once
        // for the two operand shapes that consume a call slot WITHOUT an Arg
        // (the x8-sret ReadIndirectResult, the straddled-aggregate
        // RecvByValueStackParam). Declaration/receive order == call-site
        // operand-expansion order, so `position` is the value the INLINER uses
        // to map a callee Arg → the caller's actual (the per-class ordinal is
        // ambiguous across GPR/FPR). INDEPENDENT of the register counters
        // above (which stay the lir_callconv key).
        std::uint32_t position = 0;
        [[nodiscard]] std::uint32_t next(AbiPieceClass cls) {
            if (slotAligned) return flat++;
            return (cls == AbiPieceClass::Fpr) ? fpr++ : gpr++;
        }
        [[nodiscard]] std::uint32_t nextPosition() { return position++; }
    };

    // ── Plan 24 (hir_to_mir Call residual) — shared Call-arm pieces ─────────
    // The accumulating per-call state for lowering a `Call` / `IntrinsicCall`.
    // The recursive `lowerExprNode` Call arm builds one of these on the stack;
    // the iterative `runExprDriver` Call frame keeps one in a `callCtxs` LIFO
    // vector keyed by a STABLE index (the cst_to_hir `CallCtx` idiom — a nested
    // call's argument grows that vector mid-pump, so a held reference would
    // dangle while the index survives). The four helpers below
    // (`callSetup` / `processOneCallArg` / `finishScalarCallArg` / `finishCall`)
    // are the SINGLE source of truth for the emission sequence, so the recursive
    // arm and the flattened frame are byte-identical by construction. Field
    // semantics mirror the recursive arm's locals 1:1 (see the originals).
    struct CallLowerCtx {
        HirNodeId                  node{};        // the Call / IntrinsicCall node
        TypeId                     resultTy{};    // hir.typeId(node) (the `t` local)
        bool                       isIntrinsic = false;
        std::uint32_t              intrinsicId = 0;
        std::vector<MirInstId>     operands;      // [callee, (sret,) args…] accumulating
        TypeId                     byvalCalleeSig{};
        bool                       calleeVariadic = false;
        std::optional<AbiPassing>  structRetAbi;
        MirInstId                  structRetSlot = InvalidMirInst;
        std::size_t                operandsBeforeArgs = 0;
        std::size_t                fnParamsSize = 0;
        std::size_t                fixedOperandCount = 0;
        bool                       fixedOperandCountStamped = false;
        std::uint32_t              runGpr = 0;
        std::uint32_t              runFpr = 0;
        std::size_t                argIdx = 0;    // index of the NEXT arg child
    };
    // The outcome of advancing one argument (`processOneCallArg`): a struct arg
    // emits inline and reports `StructDone`; a scalar arg is NOT lowered here —
    // it reports `ScalarPending` so the caller can lower it (recursively, or via
    // the work-stack) and then call `finishScalarCallArg` with the result.
    enum class CallArgStep : std::uint8_t { Done, ScalarPending, StructDone, Error };

    // Pre-arg-loop setup for a `Call` (mirrors the recursive arm's lines between
    // the callee `lowerExpr` and the arg loop). The callee MirInstId is already
    // lowered and passed in; this pushes it (+ a ByReference sret hidden pointer)
    // onto `ctx.operands`, resolves the callee-variadic flag, classifies a
    // by-value struct RETURN (allocating the result slot via `freshAggregateTemp`
    // — the ONLY MIR this emits, which is why it must run AFTER the callee and
    // BEFORE any argument, exactly as the recursive form), and seeds the running
    // ABI counters. Returns false on a fail-loud (diagnostic already emitted).
    // INTRINSIC calls never call this (no callee child, no struct ABI).
    [[nodiscard]] bool callSetup(MirInstId callee, CallLowerCtx& ctx) {
        HirNodeId const node = ctx.node;
        TypeId const t = ctx.resultTy;
        auto kids = hir.children(node);
        ctx.operands.reserve(kids.size());
        ctx.operands.push_back(callee);
        ctx.byvalCalleeSig = hir.typeId(kids[0]);
        if (ctx.byvalCalleeSig.valid()
            && interner.kind(ctx.byvalCalleeSig) == TypeKind::Ptr) {
            auto const w = interner.operands(ctx.byvalCalleeSig);
            if (!w.empty()) ctx.byvalCalleeSig = w[0];
        }
        ctx.calleeVariadic =
            ctx.byvalCalleeSig.valid()
            && interner.kind(ctx.byvalCalleeSig) == TypeKind::FnSig
            && interner.fnIsVariadic(ctx.byvalCalleeSig);
        // D-CSUBSET-BITINT-C2-WIDE: a wide-`_BitInt`-returning call is classified +
        // slot-materialized here exactly like a struct/union return (isByValueClass);
        // finishCall then routes it through emitStructReturningCall (2-GPR / sret).
        if (isByValueClass(interner, t)) {
            ctx.structRetAbi = byValueClassify(t);
            if (!ctx.structRetAbi.has_value()) {
                unsupported(node,
                    "returning a by-value struct/union is not supported "
                    "by this target's calling convention "
                    "(D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                return false;
            }
            ctx.structRetSlot = freshAggregateTemp(t);
            if (!ctx.structRetSlot.valid()) {
                unsupported(node,
                    "by-value struct/union return requires a sizeable "
                    "layout (complete type)");
                return false;
            }
            if (ctx.structRetAbi->kind == AbiPassing::Kind::ByReference)
                ctx.operands.push_back(ctx.structRetSlot);  // hidden sret pointer
        }
        ctx.operandsBeforeArgs =
            1 /*callee*/
          + ((ctx.structRetAbi.has_value()
              && ctx.structRetAbi->kind == AbiPassing::Kind::ByReference
              && !config.aggregateSretViaHiddenArg) ? 1u : 0u);
        ctx.fnParamsSize =
            (ctx.byvalCalleeSig.valid()
             && interner.kind(ctx.byvalCalleeSig) == TypeKind::FnSig)
                ? interner.fnParams(ctx.byvalCalleeSig).size()
                : 0;
        ctx.runGpr = 0;
        ctx.runFpr = 0;
        if (ctx.structRetAbi.has_value()
            && ctx.structRetAbi->kind == AbiPassing::Kind::ByReference
            && config.aggregateSretViaHiddenArg)
            ctx.runGpr = 1;
        ctx.argIdx = 1;   // arg children start after the callee
        return true;
    }

    // Advance ONE `Call` argument (`ctx.argIdx`), mirroring the recursive arm's
    // per-iteration loop body. A by-value struct/union arg is fully materialized
    // HERE (emitting Alloca/copy/Gep/Load or the ByValueStackArg carrier, exactly
    // as the recursive loop) and `ctx.argIdx` advanced → `StructDone`. A scalar
    // arg is NOT lowered here (the caller owns that, recursively or via the
    // work-stack); it reports `ScalarPending` with `ctx.argIdx` STILL pointed at
    // the in-flight arg so `finishScalarCallArg` derives the same `argTy`. When
    // all args are consumed → `Done`. A fail-loud (already emitted) → `Error`.
    [[nodiscard]] CallArgStep processOneCallArg(CallLowerCtx& ctx) {
        auto kids = hir.children(ctx.node);
        if (ctx.argIdx >= kids.size()) return CallArgStep::Done;
        std::size_t const i = ctx.argIdx;
        TypeId const argTy = hir.typeId(kids[i]);
        if (argTy.valid()) {
            // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` arg is passed BY VALUE exactly
            // like a struct/union (isByValueClass → classifyAggregate → 2-GPR / by-ref),
            // materialized here; it never becomes a ScalarPending register value.
            if (isByValueClass(interner, argTy)) {
                if (!emitByValueStructCallArg(ctx, kids[i], argTy))
                    return CallArgStep::Error;
                if (i == ctx.fnParamsSize) {
                    ctx.fixedOperandCount =
                        ctx.operands.size() - ctx.operandsBeforeArgs;
                    ctx.fixedOperandCountStamped = true;
                }
                ++ctx.argIdx;
                return CallArgStep::StructDone;   // caller pumps the next arg
            }
        }
        return CallArgStep::ScalarPending;   // caller lowers the scalar value
    }

    // The by-value struct/union ARG synthesis lifted VERBATIM from the recursive
    // arm's loop body (the ~200-line ABI block: classify, variadic carrier
    // selection per va_list strategy, the all-or-nothing register-exhaustion
    // split, the InRegisters piece push). Pushes onto `ctx.operands` and advances
    // `ctx.runGpr`/`ctx.runFpr`. Returns false on a fail-loud (already emitted).
    [[nodiscard]] bool emitByValueStructCallArg(CallLowerCtx& ctx, HirNodeId argNode,
                                                TypeId argTy) {
        std::vector<MirInstId>& operands = ctx.operands;
        auto const abi = byValueClassify(argTy);
        if (!abi.has_value()) {
            unsupported(argNode,
                "passing a struct/union BY VALUE is not "
                "supported by this target's calling "
                "convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
            return false;
        }
        if (abi->kind == AbiPassing::Kind::ByReference) {
            if (ctx.calleeVariadic) {
                if (!config.vaListLayout.has_value()) {
                    unsupported(argNode,
                        "passing a struct/union BY VALUE to a "
                        "variadic function requires the CC's "
                        "'vaListLayout' (overflow geometry) which "
                        "this target does not declare "
                        "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT)");
                    return false;
                }
                VaListLayout const& vlMem = *config.vaListLayout;
                if (vlMem.strategy == VaListStrategy::HomogeneousPointer) {
                    if (!appendByValueArg(operands, argNode, argTy, *abi))
                        return false;
                    ctx.runGpr += 1;
                } else if (vlMem.strategy == VaListStrategy::Aapcs64DualCursor) {
                    if (!appendByValueArg(operands, argNode, argTy, *abi))
                        return false;
                    ctx.runGpr += 1;
                } else if (vlMem.strategy == VaListStrategy::SysVRegisterSave) {
                    if (!appendByValueStackArg(operands, argNode, argTy))
                        return false;
                } else {
                    unsupported(argNode,
                        "passing a struct/union BY VALUE to a "
                        "variadic function under this va_list "
                        "strategy is not realized "
                        "(internal: unknown VaListStrategy)");
                    return false;
                }
            } else {
                if (!appendByValueArg(operands, argNode, argTy, *abi))
                    return false;
                ctx.runGpr += 1;   // the pointer operand is GPR-class
            }
        } else {
            // InRegisters: count the eightbyte pieces per class.
            std::uint32_t numGp = 0, numFp = 0;
            for (AbiPiece const& p : abi->pieces) {
                if (p.cls == AbiPieceClass::Fpr) ++numFp;
                else ++numGp;
            }
            if (numGp == 0 && numFp == 0) {
                unsupported(argNode,
                    "InRegisters struct/union arg has zero register "
                    "pieces (internal: classifier invariant "
                    "violated)");
                return false;
            }
            if (ctx.calleeVariadic && !config.vaListLayout.has_value()) {
                unsupported(argNode,
                    "passing a struct/union BY VALUE to a variadic "
                    "function requires the CC's 'vaListLayout' "
                    "(register-save geometry) which this target does "
                    "not declare "
                    "(D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT)");
                return false;
            }
            bool const routeToStack = !config.argSlotAligned
                && (ctx.runGpr + numGp > config.argGprCount
                    || ctx.runFpr + numFp > config.argFprCount);
            if (routeToStack) {
                std::uint8_t const exhaustClass =
                    config.aggregateStackExhaustsRegisters
                        ? (numFp > 0 ? kByValueStackArgExhaustFpr
                                     : kByValueStackArgExhaustGpr)
                        : kByValueStackArgExhaustNone;
                if (!appendByValueStackArg(operands, argNode, argTy, exhaustClass))
                    return false;
                if (config.aggregateStackExhaustsRegisters) {
                    if (ctx.runGpr + numGp > config.argGprCount)
                        ctx.runGpr = config.argGprCount;
                    if (ctx.runFpr + numFp > config.argFprCount)
                        ctx.runFpr = config.argFprCount;
                }
            } else {
                if (!appendByValueArg(operands, argNode, argTy, *abi))
                    return false;
                ctx.runGpr += numGp;
                ctx.runFpr += numFp;
            }
        }
        return true;
    }

    // Collect a just-lowered SCALAR `Call` argument (`argResult`), mirroring the
    // recursive arm's scalar tail after `lowerExpr(kids[i])`: push it, advance the
    // running per-class register cursor by its scalar class, stamp the fixed-
    // operand boundary if this was the last fixed param, and advance `ctx.argIdx`.
    void finishScalarCallArg(CallLowerCtx& ctx, MirInstId argResult) {
        auto kids = hir.children(ctx.node);
        std::size_t const i = ctx.argIdx;          // the in-flight arg
        TypeId const argTy = hir.typeId(kids[i]);
        ctx.operands.push_back(argResult);
        if (argTy.valid()) {
            if (scalarArgClass(argTy) == AbiPieceClass::Fpr) ++ctx.runFpr;
            else ++ctx.runGpr;
        } else {
            ++ctx.runGpr;   // typeless fallback: treat as GPR
        }
        if (i == ctx.fnParamsSize) {
            ctx.fixedOperandCount = ctx.operands.size() - ctx.operandsBeforeArgs;
            ctx.fixedOperandCountStamped = true;
        }
        ++ctx.argIdx;
    }

    // Emit the final MIR for a fully-accumulated `Call` (mirrors the recursive
    // arm's tail after the arg loop): stamp the variadic call payload, set the
    // indirect-result bit for an x8-sret CC, and emit either a plain `Call` or
    // (for a by-value struct return) the multi-piece struct-returning call. The
    // returned MirInstId is the call's value (or the result slot's address for a
    // struct return). InvalidMirInst on a fail-loud (already emitted).
    [[nodiscard]] MirInstId finishCall(CallLowerCtx& ctx) {
        HirNodeId const node = ctx.node;
        TypeId const t = ctx.resultTy;
        (void)ctx.fixedOperandCountStamped;
        std::uint32_t callPayload = 0;
        TypeId calleeTy = hir.typeId(hir.children(node)[0]);
        if (calleeTy.valid() && interner.kind(calleeTy) == TypeKind::Ptr) {
            auto const operands_ = interner.operands(calleeTy);
            if (operands_.empty()) {
                unsupported(node,
                    "Call callee has Ptr-typed FnSig with empty "
                    "operands — interner invariant violated");
                return InvalidMirInst;
            }
            calleeTy = operands_[0];
        }
        if (calleeTy.valid()
            && interner.kind(calleeTy) == TypeKind::FnSig
            && interner.fnIsVariadic(calleeTy)) {
            if (ctx.fixedOperandCount > ::dss::call_payload::kFixedOperandMask) {
                unsupported(node,
                    "Call payload: fixed-operand count exceeds 30-bit "
                    "encoding limit");
                return InvalidMirInst;
            }
            callPayload = ::dss::call_payload::encode(
                true, static_cast<std::uint32_t>(ctx.fixedOperandCount));
        }
        if (ctx.structRetAbi.has_value()
            && ctx.structRetAbi->kind == AbiPassing::Kind::ByReference
            && !config.aggregateSretViaHiddenArg)
            callPayload |= ::dss::call_payload::kIndirectResultBit;
        // FC17.9(c) (D-CSUBSET-SETJMP): OR the returns-twice flag onto the emitted Call
        // from the CST→HIR side-table (a direct `setjmp`/`_setjmp` callee) — the EXACT
        // mirror of how `volatileFlagFor` rides onto a Load/Store. The optimizer's
        // returns-twice-aware passes read this MIR flag (noreturn never reaches MIR).
        MirInstFlags const rtFlag = returnsTwiceFlagFor(node);
        if (ctx.structRetAbi.has_value())
            return emitStructReturningCall(node, ctx.operands, callPayload,
                                           *ctx.structRetAbi, ctx.structRetSlot, rtFlag);
        return mir.addInst(MirOpcode::Call, ctx.operands, t, callPayload,
                           /*flags=*/rtFlag);
    }

    // The register class of a SCALAR/pointer param (for the per-class arg
    // ordinal): FPR for a float type, GPR otherwise (ints, pointers, bool).
    [[nodiscard]] AbiPieceClass scalarArgClass(TypeId ty) const {
        TypeKind const k = interner.kind(ty);
        return (k == TypeKind::F16 || k == TypeKind::F32
                || k == TypeKind::F64 || k == TypeKind::F128)
            ? AbiPieceClass::Fpr : AbiPieceClass::Gpr;
    }

    // Classify `aggTy` for by-value passing under the active CC's strategy.
    // nullopt ⇒ the strategy is not implemented for this CC (the guard then
    // fails loud) OR the type is un-sizeable.
    [[nodiscard]] std::optional<AbiPassing> byValueClassify(TypeId aggTy) {
        if (!config.aggregateLayoutLoaded) return std::nullopt;
        return classifyAggregate(config.aggregateClassification,
                                 config.aggregateMaxRegBytes, aggTy, interner,
                                 config.aggregateLayout, config.dataModel);
    }

    // The MIR type of a register piece. GPR → I64 (the full 8-byte register
    // moves; the byte-exact valid-byte copy happens at the temp/slot boundary —
    // the register's unused high bytes are ABI-undefined). FPR → F32 for a 4-byte
    // piece (a float HFA element in an s-register), else F64 (a double / an 8-byte
    // SSE eightbyte) — the WIDTH matters for FP so the load/store emits the right
    // single/double form (a wrong F64 load of a float HFA would read 8 bytes and
    // clobber the next element). The piece's register CLASS follows from the type.
    [[nodiscard]] TypeId pieceType(AbiPiece const& p) {
        if (p.cls == AbiPieceClass::Fpr)
            return interner.primitive(p.widthBytes <= 4 ? TypeKind::F32
                                                        : TypeKind::F64);
        return interner.primitive(TypeKind::I64);
    }

    // An anonymous frame temp sized to hold aggregate `aggTy` (lir_callconv
    // rounds the slot to a 16-byte multiple, so a full-eightbyte load/store can
    // never over-run it). InvalidMirInst on an un-sizeable type.
    [[nodiscard]] MirInstId freshAggregateTemp(TypeId aggTy) {
        auto const sz = aggregateByteSize(aggTy);
        if (!sz.has_value()
            || *sz > std::numeric_limits<std::uint32_t>::max())
            return InvalidMirInst;
        return mir.addInst(MirOpcode::Alloca, {}, interner.pointer(aggTy),
                           static_cast<std::uint32_t>(*sz));
    }

    // Append a by-value struct/union ARG (`argNode` of type `aggTy`, classified
    // `abi`) to a Call's `operands`. InRegisters ⇒ copy the struct into an
    // eightbyte-rounded temp (so a full-eightbyte load can't over-read a
    // non-padded source) and load each eightbyte as an I64/F64 scalar arg.
    // ByReference ⇒ copy to a temp and pass its address (the callee owns the
    // copy → by-value semantics). Returns false (fail-loud already emitted).
    [[nodiscard]] bool appendByValueArg(std::vector<MirInstId>& operands,
                                        HirNodeId argNode, TypeId aggTy,
                                        AbiPassing const& abi) {
        MirInstId const srcAddr = lowerLvalueAddress(argNode);
        if (!srcAddr.valid()) return false;
        StructLayout const* layout = cachedLayout(aggTy);
        MirInstId const temp = freshAggregateTemp(aggTy);
        if (layout == nullptr || !temp.valid()) {
            unsupported(argNode, "by-value aggregate arg requires a sizeable "
                                 "layout (target 'aggregateLayout' / complete type)");
            return false;
        }
        // c27 (D-CSUBSET-VOLATILE-POINTEE): passing a `volatile`-qualified
        // aggregate BY VALUE reads the whole object (C 6.7.3p5: every member
        // access of a volatile aggregate is volatile). The source's ACCESSED TYPE
        // is `VolatileQual(...)` when the container is volatile (a `volatile struct`
        // local, or a deref of a `volatile struct *`). Flag the source-reading
        // Loads of the copy; the temp (a fresh private copy) is non-volatile, so
        // its piece Loads stay plain.
        MirInstFlags const srcVf =
            volatileFlagFor(argNode) | volatileFlagForType(hir.typeId(argNode));
        if (!lowerByteWiseCopy(srcAddr, temp, layout->size, srcVf)) return false;
        if (abi.kind == AbiPassing::Kind::ByReference) {
            operands.push_back(temp);   // a pointer to the callee-owned copy
            return true;
        }
        for (AbiPiece const& p : abi.pieces) {
            TypeId const pty = pieceType(p);
            MirInstId const offK =
                constInt(static_cast<std::int64_t>(p.byteOffset));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> g{temp, offK};
            MirInstId const gp =
                mir.addInst(MirOpcode::Gep, g, interner.pointer(pty));
            if (!gp.valid()) return false;
            std::array<MirInstId, 1> l{gp};
            MirInstId const v = mir.addInst(MirOpcode::Load, l, pty);
            if (!v.valid()) return false;
            operands.push_back(v);
        }
        return true;
    }

    // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): append a by-value
    // struct/union ARG that must be passed ENTIRELY in the outgoing overflow (stack)
    // area — the Option-C carrier. Used at the variadic boundary for the two shapes
    // SysV §3.2.3/§3.5.7 forces wholly to memory: a MEMORY-class (>16B) aggregate, AND
    // an InRegisters aggregate whose eightbyte pieces do not all fit in the remaining
    // arg registers (the register-exhaustion split). Copies the source into a callee-
    // owned eightbyte-rounded temp (so the callee gets by-value semantics + a full-
    // eightbyte read can't over-run a non-padded source), then pushes ONE Call operand:
    // a `ByValueStackArg` wrapping the temp address, carrying the aggregate byte size as
    // its payload. lir_callconv places it at the next overflow offset(s) (NOT a register
    // — runGpr/runFpr are NOT advanced by the caller). CC-NEUTRAL: no SysV-specific
    // assumption; the Win64/AAPCS64 struct-vararg cycles reuse this carrier. Returns
    // false (fail-loud already emitted).
    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the arg-register class the
    // CALLER's placement EXHAUSTS once this aggregate is stacked (matches the callee
    // cursor + va_start clamp so caller/callee agree). The byte size + exhaust class are
    // packed into the ByValueStackArg payload per the shared `kByValueStackArg*` encoding
    // (mir_opcode.hpp); mir_to_lir unpacks it onto the LIR `ByValueStackAgg` marker.
    [[nodiscard]] bool appendByValueStackArg(
            std::vector<MirInstId>& operands, HirNodeId argNode, TypeId aggTy,
            std::uint8_t exhaustClass = kByValueStackArgExhaustNone) {
        MirInstId const srcAddr = lowerLvalueAddress(argNode);
        if (!srcAddr.valid()) return false;
        StructLayout const* layout = cachedLayout(aggTy);
        MirInstId const temp = freshAggregateTemp(aggTy);
        if (layout == nullptr || !temp.valid()) {
            unsupported(argNode, "by-value stack aggregate arg requires a sizeable "
                                 "layout (target 'aggregateLayout' / complete type)");
            return false;
        }
        if (!lowerByteWiseCopy(srcAddr, temp, layout->size)) return false;
        if (layout->size > kByValueStackArgSizeMask) {
            unsupported(argNode, "by-value stack aggregate arg is too large to "
                                 "encode its byte size");
            return false;
        }
        std::uint32_t const payload =
            static_cast<std::uint32_t>(layout->size)
            | (static_cast<std::uint32_t>(exhaustClass)
               << kByValueStackArgExhaustShift);
        std::array<MirInstId, 1> carrierOps{temp};
        MirInstId const carrier =
            mir.addInst(MirOpcode::ByValueStackArg, carrierOps,
                        interner.pointer(aggTy), payload);
        if (!carrier.valid()) return false;
        operands.push_back(carrier);
        return true;
    }

    // Receive a by-value struct/union PARAM into its frame slot, consuming
    // physical-arg ordinals from the per-class `ArgOrdinalCounter` (kept in
    // lockstep with the caller's operand order). InRegisters ⇒ one Arg per
    // eightbyte, each stored
    // into the slot at its offset (the 16-rounded slot absorbs trailing
    // padding). ByReference ⇒ the incoming pointer to the caller's private copy
    // IS the param's storage (no re-copy). Returns false (fail-loud emitted).
    [[nodiscard]] bool receiveByValueParam(SymbolId sym, TypeId aggTy,
                                           HirNodeId anchor, AbiPassing const& abi,
                                           ArgOrdinalCounter& ctr) {
        if (abi.kind == AbiPassing::Kind::ByReference) {
            // The hidden pointer is GPR-class. If the GPR pool is already exhausted
            // (an INDEPENDENT-counter CC) the pointer itself rides the incoming stack
            // — a stacked SCALAR; account it in the byte cursor + residual guard so
            // va_start's overflow base skips it (D-FC12-...).
            if (!config.argSlotAligned && ctr.gpr >= config.argGprCount
                && !accountFixedStackScalar(anchor))
                return false;
            std::uint32_t const pos = ctr.nextPosition();  // one call operand
            MirInstId const ptr =
                mir.addArg(ctr.next(AbiPieceClass::Gpr), interner.pointer(aggTy),
                           pos);
            if (!ptr.valid()) return false;
            addressableLocal[sym.v] = ptr;   // the caller's copy is the param
            return true;
        }
        // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: ALL-OR-NOTHING straddle.
        // For an INDEPENDENT-counter CC, if the aggregate's eightbyte pieces do not all
        // fit the remaining arg registers it is received WHOLLY from the incoming stack
        // (never split) — the callee mirror of the caller's `ByValueStackArg` carrier.
        // Win64 (slotAligned) never straddles (one struct = one positional slot).
        std::uint32_t numGp = 0, numFp = 0;
        for (AbiPiece const& p : abi.pieces) {
            if (p.cls == AbiPieceClass::Fpr) ++numFp; else ++numGp;
        }
        bool const straddles = !config.argSlotAligned
            && (ctr.gpr + numGp > config.argGprCount
                || ctr.fpr + numFp > config.argFprCount);
        if (straddles) {
            // Residual guard (NO silent miscompile): this aggregate is sited via the
            // incoming-overflow BYTE cursor, but a stacked SCALAR is sited by
            // lir_callconv from its per-class ordinal `(ord - pool)*slot` — the two
            // agree ONLY when the aggregate is the FIRST/ONLY fixed param to overflow.
            // A prior stacked fixed param (scalar OR a first aggregate) desyncs them →
            // fail loud on the rare multi-overflow shapes (outside the deferral scope).
            if (currentFnSawFixedStackParam_) {
                unsupported(anchor,
                    "a by-value aggregate parameter that straddles the register/stack "
                    "boundary is only supported as the FIRST fixed parameter to "
                    "overflow onto the incoming stack; a preceding stacked fixed "
                    "parameter is not yet supported "
                    "(D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS)");
                return false;
            }
            StructLayout const* layout = cachedLayout(aggTy);
            MirInstId const slot = allocaForLocal(sym, aggTy, anchor);
            if (layout == nullptr || !slot.valid()) {
                unsupported(anchor, "by-value stacked aggregate parameter requires a "
                                    "sizeable layout (complete type / target "
                                    "'aggregateLayout')");
                return false;
            }
            // Symmetric with the caller's appendByValueStackArg size guard: a stacked
            // aggregate's rounded span accumulates into the uint32 byte cursor, so an
            // absurd (>1 GiB) layout would overflow it. Fail loud (the type system
            // bounds real aggregates far below this — defensive parity, not reachable).
            if (layout->size > kByValueStackArgSizeMask) {
                unsupported(anchor, "by-value stacked aggregate parameter is too large "
                                    "to encode its incoming-stack byte span");
                return false;
            }
            // The incoming ADDRESS of this whole aggregate = the overflow base + its
            // byte offset within the incoming overflow area (the byte cursor; 0 for
            // the first/only overflowed fixed param, guaranteed by the guard above).
            // Byte-copy it into the param's local slot (by-value semantics, the
            // by-reference reception precedent but reading from the stack).
            // The straddled aggregate rides ONE `ByValueStackArg` carrier
            // operand on the call side (no Arg here — RecvByValueStackParam) —
            // advance the flat position past that operand so later params stay
            // aligned to the operand list (arg_payload.hpp; this callee is
            // inline-refused, but the positions must stay consistent).
            (void)ctr.nextPosition();
            MirInstId const src =
                mir.addInst(MirOpcode::RecvByValueStackParam, {},
                            interner.pointer(aggTy),
                            /*payload=*/currentFnFixedStackBytes_);
            if (!src.valid()) return false;
            if (!lowerByteWiseCopy(src, slot, layout->size)) return false;
            addressableLocal[sym.v] = slot;
            // Advance the byte cursor by this aggregate's rounded incoming-stack span,
            // then EXHAUST-or-BACKFILL the overflowed per-class cursor: AAPCS64 sets
            // NGRN/NSRN ← pool (clamp, no backfill); SysV leaves the cursor (backfill).
            // Config-driven (gcc aarch64_layout_arg vs function_arg_advance_64), never
            // an arch identity branch.
            std::uint32_t const slot8 = stackSlotBytes();
            currentFnFixedStackBytes_ += roundUpToSlot(
                static_cast<std::uint32_t>(layout->size), slot8);
            currentFnSawStackedAggregate_ = true;
            currentFnSawFixedStackParam_  = true;
            if (config.aggregateStackExhaustsRegisters) {
                if (ctr.gpr + numGp > config.argGprCount)
                    ctr.gpr = config.argGprCount;
                if (ctr.fpr + numFp > config.argFprCount)
                    ctr.fpr = config.argFprCount;
            }
            return true;
        }
        // Register-resident: receive each eightbyte from its arg register into the
        // 16-rounded slot at its offset (trailing padding absorbed).
        MirInstId const slot = allocaForLocal(sym, aggTy, anchor);
        if (!slot.valid()) return false;
        for (AbiPiece const& p : abi.pieces) {
            TypeId const pty = pieceType(p);
            std::uint32_t const pos = ctr.nextPosition();  // one operand per piece
            MirInstId const a = mir.addArg(ctr.next(p.cls), pty, pos);
            if (!a.valid()) return false;
            MirInstId const offK =
                constInt(static_cast<std::int64_t>(p.byteOffset));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> g{slot, offK};
            MirInstId const gp =
                mir.addInst(MirOpcode::Gep, g, interner.pointer(pty));
            if (!gp.valid()) return false;
            std::array<MirInstId, 2> st{a, gp};
            mir.addInst(MirOpcode::Store, st);
        }
        return true;
    }

    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the incoming/outgoing
    // stack-arg slot quantum = the GPR / pointer width (8 on the shipped LP64/LLP64
    // targets). The same value lir_callconv uses for `outgoingSlotSize`; derived
    // agnostically from the data model so a future ILP32 ABI is a config change.
    [[nodiscard]] std::uint32_t stackSlotBytes() const {
        return static_cast<std::uint32_t>(
            scalarByteSize(TypeKind::Ptr, config.dataModel).value_or(8));
    }
    [[nodiscard]] static std::uint32_t roundUpToSlot(std::uint32_t bytes,
                                                     std::uint32_t slot) {
        return slot == 0 ? bytes : ((bytes + slot - 1) / slot) * slot;
    }

    // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: account ONE stacked fixed
    // SCALAR (or a ByReference aggregate's stacked hidden pointer) in the byte cursor
    // + residual guard. A stacked scalar AFTER a stacked aggregate desyncs the
    // per-class-ordinal siting from the byte cursor (the AAPCS64 clamp strands it) →
    // fail loud. Returns false (diagnostic emitted) on that residual. INDEPENDENT-
    // counter CCs only (the caller gates on !argSlotAligned).
    [[nodiscard]] bool accountFixedStackScalar(HirNodeId anchor) {
        if (currentFnSawStackedAggregate_) {
            unsupported(anchor,
                "a fixed scalar parameter that overflows onto the incoming stack AFTER "
                "a by-value aggregate parameter was placed there is not yet supported "
                "(D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS)");
            return false;
        }
        currentFnFixedStackBytes_    += stackSlotBytes();
        currentFnSawFixedStackParam_  = true;
        return true;
    }

    // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): emit a by-value struct/union-
    // returning Call into its result slot `slot` and yield `slot` (the aggregate
    // is accessed by address). `operands` already carries [callee, (sret slot,)
    // args...]. ByReference (sret) ⇒ the call is void and the callee writes
    // through the hidden pointer → `slot` already holds the result. InRegisters ⇒
    // the call's result IS piece 0 (lir_callconv captures it from return-register
    // ordinal 0 of its class) and each further eightbyte is a
    // `ReturnPiece(call, ordinal)`; every piece is stored into `slot` at its
    // offset. The per-class `ord` mirrors the callee's multi-Return assignment, so
    // caller and callee agree on which register each piece occupies.
    [[nodiscard]] MirInstId emitStructReturningCall(
            HirNodeId node, std::span<MirInstId const> operands,
            std::uint32_t callPayload, AbiPassing const& abi, MirInstId slot,
            MirInstFlags callFlags = MirInstFlags::None) {
        // `callFlags` carries the FC17.9(c) returns-twice bit (D-CSUBSET-SETJMP) for a
        // returns-twice callee that returns a struct BY VALUE. No returns-twice libc
        // function does (setjmp returns int) — but threading it here keeps the carrier
        // complete for EVERY Call shape rather than only the scalar/void path (no
        // silent-drop seam if a future returns-twice extern returns an aggregate).
        if (abi.kind == AbiPassing::Kind::ByReference) {
            MirInstId const call =
                mir.addInst(MirOpcode::Call, operands, InvalidType, callPayload,
                            /*flags=*/callFlags);
            if (!call.valid()) return InvalidMirInst;
            return slot;
        }
        if (abi.pieces.empty()) {
            unsupported(node,
                "in-register struct return classified with zero pieces "
                "(classifier invariant violated)");
            return InvalidMirInst;
        }
        TypeId const p0ty = pieceType(abi.pieces[0]);
        MirInstId const call =
            mir.addInst(MirOpcode::Call, operands, p0ty, callPayload,
                        /*flags=*/callFlags);
        if (!call.valid()) return InvalidMirInst;
        // Pass 1: capture every piece IMMEDIATELY after the call — the
        // lir_callconv caller look-ahead requires the `ReturnPiece` reads to be
        // contiguous with the call (no Gep/Store may intervene, else the pieces
        // would read return registers already clobbered). Piece 0 IS the call's
        // own result; pieces 1..N-1 are ReturnPiece reads.
        std::vector<MirInstId> pieceVals;
        pieceVals.reserve(abi.pieces.size());
        std::uint32_t gprRet = 0;
        std::uint32_t fprRet = 0;
        for (std::size_t k = 0; k < abi.pieces.size(); ++k) {
            AbiPiece const& p = abi.pieces[k];
            std::uint32_t const ord =
                (p.cls == AbiPieceClass::Fpr) ? fprRet++ : gprRet++;
            if (k == 0) { pieceVals.push_back(call); continue; }
            MirInstId const rp =
                mir.addReturnPiece(call, ord, pieceType(p));
            if (!rp.valid()) return InvalidMirInst;
            pieceVals.push_back(rp);
        }
        // Pass 2: store each captured piece into the result slot at its offset.
        for (std::size_t k = 0; k < abi.pieces.size(); ++k) {
            AbiPiece const& p = abi.pieces[k];
            TypeId const pty = pieceType(p);
            MirInstId const offK =
                constInt(static_cast<std::int64_t>(p.byteOffset));
            if (!offK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> g{slot, offK};
            MirInstId const gp =
                mir.addInst(MirOpcode::Gep, g, interner.pointer(pty));
            if (!gp.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> st{pieceVals[k], gp};
            mir.addInst(MirOpcode::Store, st);
        }
        return slot;
    }

    // FC7 C1c: lower `return <struct-expr>;` for a by-value struct/union-returning
    // function. The returned aggregate is an lvalue/temp; take its ADDRESS and
    // either copy it through the sret hidden pointer (ByReference) or load its
    // eightbyte pieces into a multi-operand Return (InRegisters). Mirror of the
    // caller's `emitStructReturningCall`.
    [[nodiscard]] bool lowerStructReturn(HirNodeId valNode) {
        auto const abi = byValueClassify(currentFnResult_);
        if (!abi.has_value()) {
            unsupported(valNode,
                "returning a by-value struct/union is not supported by this "
                "target's calling convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
            return false;
        }
        StructLayout const* layout = cachedLayout(currentFnResult_);
        if (layout == nullptr) {
            unsupported(valNode, "by-value struct/union return requires a sizeable "
                                 "layout (complete type)");
            return false;
        }
        MirInstId const srcAddr = lowerLvalueAddress(valNode);
        if (!srcAddr.valid()) return false;
        // c27 (D-CSUBSET-VOLATILE-POINTEE): returning a `volatile`-qualified
        // aggregate BY VALUE reads the whole returned object (C 6.7.3p5). The
        // returned value's ACCESSED TYPE is `VolatileQual(...)` when it is a
        // `volatile struct` lvalue (or a deref of a `volatile struct *`). Flag the
        // source-reading Loads (the sret-copy halves and the in-register piece
        // Loads); the sret destination is the CALLER's storage — keeping the
        // dest-store flag matches the conservative whole-aggregate-copy convention.
        MirInstFlags const srcVf =
            volatileFlagFor(valNode) | volatileFlagForType(hir.typeId(valNode));
        if (abi->kind == AbiPassing::Kind::ByReference) {
            if (!sretPtr_.valid()) {
                unsupported(valNode, "sret return reached without a hidden result "
                                     "pointer (lowerFunction setup invariant)");
                return false;
            }
            if (!lowerByteWiseCopy(srcAddr, sretPtr_, layout->size, srcVf)) return false;
            // SysV/Win64 return the result pointer in the integer return register
            // (rax/rcx). AAPCS64/Apple x8-sret returns VOID — the caller owns x8 and
            // the storage it points at; the callee must NOT also place it in x0.
            if (config.aggregateSretViaHiddenArg) mir.addReturn(sretPtr_);
            else                                  mir.addReturn();
            return true;
        }
        std::vector<MirInstId> pieces;
        pieces.reserve(abi->pieces.size());
        for (AbiPiece const& p : abi->pieces) {
            TypeId const pty = pieceType(p);
            MirInstId const offK =
                constInt(static_cast<std::int64_t>(p.byteOffset));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> g{srcAddr, offK};
            MirInstId const gp =
                mir.addInst(MirOpcode::Gep, g, interner.pointer(pty));
            if (!gp.valid()) return false;
            std::array<MirInstId, 1> l{gp};
            MirInstId const v = mir.addInst(MirOpcode::Load, l, pty, /*payload=*/0, srcVf);
            if (!v.valid()) return false;
            pieces.push_back(v);
        }
        mir.addReturnMulti(pieces);
        return true;
    }

    // Recursively collect into `out` the set of `SymbolId.v`s whose lvalue
    // address is needed somewhere under `node` — i.e. symbols that must be
    // slot-backed rather than pure-SSA. Drives entry-block slot-promotion
    // for params. Four shapes contribute:
    //   - `AddressOf(Ref(sym))` — explicit `&sym`.
    //   - `MemberAccess(Ref(sym), …)` — `sym.field` needs `sym`'s address
    //     so GEP can index into its storage. (Conversely `(*p).field`
    //     never needs the addressable form because Deref's lvalue is the
    //     pointer rvalue.)
    //   - `Index(Ref(sym), …)` when sym's type is NOT a pointer —
    //     indexing an array/aggregate local needs the array's address;
    //     indexing through a pointer (`p[i]`) does not.
    //   - `AssignStmt` whose target is `Ref(sym)` — the symbol is being
    //     mutated, so it must live in a storage slot rather than as a
    //     pure-SSA value.
    // Walks every child; HIR is already verified well-formed before this
    // pass, so all referenced types are valid.
    void collectAddressTakenSymbols(HirNodeId node,
                                    std::unordered_set<std::uint32_t>& out) {
        if (!node.valid()) return;
        HirKind const k = hir.kind(node);
        auto refSymOf = [&](HirNodeId child) -> std::optional<std::uint32_t> {
            if (hir.kind(child) != HirKind::Ref) return std::nullopt;
            return hir.payload(child);
        };
        if (k == HirKind::AddressOf) {
            auto kids = hir.children(node);
            if (kids.size() == 1) {
                if (auto s = refSymOf(kids[0]); s.has_value()) out.insert(*s);
            }
        } else if (k == HirKind::MemberAccess) {
            auto kids = hir.children(node);
            if (kids.size() == 1) {
                if (auto s = refSymOf(kids[0]); s.has_value()) out.insert(*s);
            }
        } else if (k == HirKind::Index) {
            auto kids = hir.children(node);
            if (kids.size() == 2) {
                if (auto s = refSymOf(kids[0]); s.has_value()) {
                    // Only register if the base is NOT a pointer (pointer
                    // indexing uses the rvalue, not the storage).
                    TypeId const baseTy = hir.typeId(kids[0]);
                    TypeKind const baseKind = baseTy.valid()
                        ? interner.kind(baseTy) : TypeKind::Void;
                    if (baseKind != TypeKind::Ptr) out.insert(*s);
                }
            }
        } else if (k == HirKind::AssignStmt) {
            HirNodeId const target = hir.assignTarget(node);
            if (auto s = refSymOf(target); s.has_value()) {
                out.insert(*s);
            }
        } else if (k == HirKind::VaArg || k == HirKind::VaStart
                   || k == HirKind::VaEnd) {
            // c63 (D-CSUBSET-VA-LIST-PARAM-SLOT): va_arg/va_start/va_end use the
            // va_list as an LVALUE — the list is read AND ADVANCED in place — so a
            // va_list PARAMETER must be slot-backed (a local va_list already is, via
            // its VarDecl). kids[0] is the apExpr; mark it when it is a bare Ref.
            // Drives the Win64/Apple `char*` va_list param onto the addressTaken
            // scalar arm by USAGE (its Ptr<I8> type is indistinguishable from a plain
            // char*); the SysV Array<__va_list_tag> param is intercepted earlier by a
            // dedicated reception arm, so this mark is moot for it.
            auto kids = hir.children(node);
            if (!kids.empty()) {
                if (auto s = refSymOf(kids[0]); s.has_value()) out.insert(*s);
            }
        }
        for (HirNodeId child : hir.children(node)) {
            collectAddressTakenSymbols(child, out);
        }
    }

    // c116b H1 (D-WIN64-SEH-FUNCLETS): collect the symbols referenced by any SEH
    // `__except` FILTER expression in the function body. The filter is extracted into
    // a separate funclet function that runs at fault time; it can only reach a parent
    // local through the parent's FRAME (RecoverParentFrameSlot). A parent PARAMETER is
    // otherwise a pure SSA `Arg` value (not in memory), which the funclet cannot
    // recover — so every filter-referenced symbol must be forced MEMORY-BACKED
    // (address-taken) in the parent, exactly like mem2reg is skipped for SEH bodies.
    // Body locals are already slot-backed via their VarDecl; this closes the PARAM
    // gap (sqlite's `sehExceptionFilter(pWal, …)` reads the `pWal` parameter). Keyed
    // on the SEH HIR node (a C-language construct), not arch/format.
    void collectSehFilterReferencedSymbols(
        HirNodeId node, std::unordered_set<std::uint32_t>& out) {
        if (!node.valid()) return;
        if (hir.kind(node) == HirKind::SehTryExcept) {
            HirNodeId const filterN = hir.sehTryFilter(node);
            collectRefSymbols(filterN, out);
        }
        for (HirNodeId child : hir.children(node)) {
            collectSehFilterReferencedSymbols(child, out);
        }
    }

    // Collect every `Ref`-node symbol reachable under `node` (any depth). Used by the
    // SEH-filter escape analysis to force filter-referenced params into memory.
    void collectRefSymbols(HirNodeId node,
                           std::unordered_set<std::uint32_t>& out) {
        if (!node.valid()) return;
        if (hir.kind(node) == HirKind::Ref) out.insert(hir.payload(node));
        for (HirNodeId child : hir.children(node)) {
            collectRefSymbols(child, out);
        }
    }

    // D-CSUBSET-COMPUTED-GOTO: collect every LABEL ORDINAL whose address is taken
    // via `&&label` (LabelAddressOf) anywhere in the function body, so an IndirectBr
    // can list all address-taken blocks as successors. A forward `&&end` (textually
    // after the `goto *p`) is still found — the whole body is scanned up front.
    void collectAddressTakenLabels(HirNodeId node,
                                   std::unordered_set<std::uint32_t>& out) {
        if (!node.valid()) return;
        if (hir.kind(node) == HirKind::LabelAddressOf) {
            out.insert(hir.labelAddressOrdinal(node));
        }
        for (HirNodeId child : hir.children(node)) {
            collectAddressTakenLabels(child, out);
        }
    }

    // ── VLA C5 (D-CSUBSET-VLA): block-scope stack teardown helpers ──────────
    // A dynamic VLA descends SP; C5 restores it on every NON-return exit of the
    // declaring scope. The decisions live HERE (HIR→MIR) because MIR is flat — a
    // Br/CondBr carries no scope tag, so "which VLA scopes does this edge exit" must
    // be computed while lexical-block structure is still visible.

    // Map each LabelStmt's ordinal → its HIR node (goto-teardown ancestry). Populated
    // at function entry, alongside the addressTakenLabels scan. Reset per function.
    void collectLabelNodes(HirNodeId node) {
        if (!node.valid()) return;
        if (hir.kind(node) == HirKind::LabelStmt)
            labelNodeByOrdinal_[hir.labelOrdinal(node)] = node;
        for (HirNodeId child : hir.children(node)) collectLabelNodes(child);
    }
    [[nodiscard]] HirNodeId resolveLabelNode(std::uint32_t ordinal) const {
        auto it = labelNodeByOrdinal_.find(ordinal);
        return it == labelNodeByOrdinal_.end() ? HirNodeId{} : it->second;
    }

    // Open a teardown watermark for a dynamic-VLA local: emit a StackSave (SP
    // captured just BEFORE the VLA's `sub sp`) and push a vlaScopeStack_ frame. The
    // caller (VarDecl lowering) invokes this IMMEDIATELY before `allocaForLocal`
    // emits the VLA alloca — the fixed stride-slot allocas emitted in between do NOT
    // move SP (they are FP-relative frame slots), so the save == SP pre-`sub sp`.
    // The scope node is EITHER a compound-statement Block (a body/nested-block VLA,
    // torn down at the block's exit) OR a ForStmt (a `for`-INIT VLA — declared once at
    // loop entry, persists across iterations, torn down at the loop's EXIT edges by
    // the For driver, NEVER on the back-edge). A VLA in a MULTI-declarator for-init
    // (`for (int a[n], b[m]; ...)`) is wrapped in a Block whose parent is the ForStmt;
    // that Block's own teardown would free the VLA at the END OF THE INIT (before the
    // body), so that narrow shape is deferred fail-loud (never a silent early-free).
    [[nodiscard]] bool emitStackSaveForVla(HirNodeId declNode) {
        HirNodeId const scopeNode = hir.parent(declNode);
        bool const scopeIsBlock =
            scopeNode.valid() && hir.kind(scopeNode) == HirKind::Block;
        bool const scopeIsForInit =
            scopeNode.valid() && hir.kind(scopeNode) == HirKind::ForStmt;
        if (!scopeIsBlock && !scopeIsForInit) {
            unsupported(declNode,
                "a variable-length array in this declaration position is not yet "
                "torn down at scope exit (D-CSUBSET-VLA)");
            return false;
        }
        // A multi-declarator for-init Block (parent == ForStmt, and it IS that for's
        // init clause): defer — its Block teardown would free the VLA before the body.
        if (scopeIsBlock) {
            HirNodeId const gp = hir.parent(scopeNode);
            if (gp.valid() && hir.kind(gp) == HirKind::ForStmt) {
                auto const initN = hir.forInit(gp);
                if (initN.has_value() && *initN == scopeNode) {
                    unsupported(declNode,
                        "a variable-length array in a MULTI-declarator `for`-init "
                        "clause (`for (int a[n], b[m]; ...)`) is not yet torn down at "
                        "loop exit — deferred (D-CSUBSET-VLA-FOR-INIT-MULTIDECL)");
                    return false;
                }
            }
        }
        std::uint32_t const scopeId = vlaScopeCounter_++;
        MirInstId const save =
            mir.addInst(MirOpcode::StackSave, {}, i64Ty(), scopeId);
        if (!save.valid()) return false;
        vlaScopeStack_.push_back({declNode, scopeNode, save, scopeId});
        return true;
    }

    // Emit a StackRestore to the entry watermark of vlaScopeStack_[frameIdx] (frees
    // that scope's VLA + every DEEPER one — the stack grows down, so a shallower
    // watermark reclaims all below it). scopeId payload pairs it to its StackSave.
    void emitVlaRestore(std::size_t frameIdx) {
        VlaScopeFrame const& fr = vlaScopeStack_[frameIdx];
        std::array<MirInstId, 1> ops{fr.saveBefore};
        mir.addInst(MirOpcode::StackRestore, ops, InvalidType, fr.scopeId);
    }

    // A lexical block whose lowering opened VLA scopes at depth >= `baseDepth` is
    // finishing. On a FALL-THROUGH exit (the open block NOT already sealed by a
    // break/continue/goto/return — audit fix #10), restore SP to the SHALLOWEST VLA
    // this block declared (index == baseDepth), then pop those frames. `emitRestore
    // = false` on the error-bail path (pop only; compilation is aborting). No-op —
    // emits NEITHER op — when the block declared no VLA (the byte-clean common case).
    void closeVlaBlockScope(std::size_t baseDepth, bool emitRestore) {
        if (vlaScopeStack_.size() <= baseDepth) return;   // no VLA opened here
        if (emitRestore && !mir.openBlockHasTerminator())
            emitVlaRestore(baseDepth);
        vlaScopeStack_.resize(baseDepth);
    }

    // Is label `L` inside `fr`'s VLA scope? Drives which scopes a `goto` exits.
    //  * A BLOCK-scope VLA: true iff fr.scopeNode is a lexical ancestor of L AND
    //    fr.declNode textually PRECEDES (in the block's child order) the child leading
    //    to L — the VLA's scope runs from its decl to the end of its block (C99 6.2.4).
    //  * A for-INIT VLA (scopeNode is the ForStmt): true iff the ForStmt is a lexical
    //    ancestor of L — a for-init object is in scope for the ENTIRE for statement
    //    (there is no position "before" the for-init inside the loop), so ANY label in
    //    the loop is enclosed; a goto to a label OUTSIDE the for exits the for-init.
    [[nodiscard]] bool vlaFrameEnclosesLabel(VlaScopeFrame const& fr,
                                             HirNodeId L) const {
        bool const scopeIsBlock = hir.kind(fr.scopeNode) == HirKind::Block;
        HirNodeId prev = L;
        for (HirNodeId cur = hir.parent(L); cur.valid(); cur = hir.parent(cur)) {
            if (cur == fr.scopeNode) {
                if (!scopeIsBlock) return true;   // for-init: whole-for scope
                auto kids = hir.children(cur);
                int pIdx = -1, dIdx = -1;
                for (std::size_t i = 0; i < kids.size(); ++i) {
                    if (kids[i] == prev)        pIdx = static_cast<int>(i);
                    if (kids[i] == fr.declNode) dIdx = static_cast<int>(i);
                }
                return dIdx >= 0 && pIdx >= 0 && dIdx < pIdx;
            }
            prev = cur;
        }
        return false;   // scopeNode not an ancestor of L → L is outside this scope
    }

    // Emit the SP teardown for a `goto`. Every open vlaScopeStack_ frame encloses the
    // goto textually; those NOT enclosing the target label L are EXITED. Scopes nest,
    // so the enclosing-L frames form a PREFIX and the exited ones a SUFFIX — restore
    // to the SHALLOWEST exited frame (frees all exited). The entry-side ban
    // (H_VlaJumpIntoScope) guarantees a legal goto never enters a VLA scope past its
    // decl, so the restore-target StackSave always dominates the goto (SSA-valid).
    // Nothing to do when the goto stays within every open scope (frees nothing).
    void emitGotoVlaTeardown(HirNodeId gotoNode) {
        if (vlaScopeStack_.empty()) return;
        HirNodeId const L = resolveLabelNode(hir.labelOrdinal(gotoNode));
        for (std::size_t k = 0; k < vlaScopeStack_.size(); ++k) {
            // A defensively-unresolved L (should not occur post-HIR-verify) frees ALL
            // open scopes — the safe over-approximation for an outward jump, never a
            // leak.
            if (!L.valid() || !vlaFrameEnclosesLabel(vlaScopeStack_[k], L)) {
                emitVlaRestore(k);
                return;
            }
        }
    }

    // c69 (D-MIR-ENTRY-BLOCK-ALLOCA-HOIST): collect every body-local `VarDecl`
    // node, so `lowerFunction` can pre-emit its storage `Alloca` into the ENTRY
    // block (which dominates every use) BEFORE lowering the body. Without this a
    // local declared in an ENTRY-UNREACHABLE block — the canonical case is a
    // declaration before the first `case` of a `switch`, which the c60 switch-
    // flatten places in a predecessor-less pre-case block — gets its `Alloca`
    // emitted into that dead block; the mandatory unreachable-prune
    // (D-MIR-UNREACHABLE-PRUNE-NORMALIZE) then drops the block while a reachable
    // case body still `Load`s the slot → the MirFunctionRebuilder rewrite-map
    // completeness abort (D-OPT2-REWRITE-MAP-COMPLETENESS). Walks the whole body
    // subtree (no nested functions in the C-subset → every VarDecl is a local of
    // THIS function); includes `for`-init declarations (a child of the `For`).
    void collectLocalDecls(HirNodeId node, std::vector<HirNodeId>& out) {
        if (!node.valid()) return;
        if (hir.kind(node) == HirKind::VarDecl) out.push_back(node);
        for (HirNodeId child : hir.children(node)) {
            collectLocalDecls(child, out);
        }
    }

    // FC3.5 sweep-c1 (chip task_20b1224d): lower one `for` header
    // clause (init or update). cst_to_hir's `lowerForClause` emits one
    // of exactly three shapes:
    //   * `VarDecl`    — a declaration init (`for (int i = 0; ...)`);
    //   * `AssignStmt` — an assignment, INCLUDING every compound
    //     assign (`+=`/`-=`/.../`<<=`/`>>=`) and postfix `++`/`--`,
    //     which `lowerCompoundAssign`/`lowerIncDecStmt` desugar to
    //     AssignStmt before MIR ever sees them;
    //   * a BARE expression evaluated for its side effects (the value
    //     is discarded — cst_to_hir deliberately does NOT wrap for-
    //     clause expressions in ExprStmt).
    // Routing the whole clause through `lowerExpr` was the bug:
    // `for (i = 9; i; i = i - 1)`'s update is an AssignStmt, and the
    // expression dispatch fails loud with "HIR expression kind ordinal
    // 19 [AssignStmt] not yet supported". Statement-shaped clauses go
    // through `lowerStmt`; anything else stays an expression (its
    // default arm still fail-louds on a genuinely unloweable kind).
    bool lowerForClauseNode(HirNodeId node) {
        switch (hir.kind(node)) {
            case HirKind::VarDecl:
            case HirKind::AssignStmt:
            // FC4 c1: a multi-declarator for-init (`for (int i = 0, j = n;
            // ...)`) lowers as a Block of VarDecls — statements, lowered
            // sequentially into the entry block like any block body.
            case HirKind::Block:
                return lowerStmt(node);
            default:
                // A bare for-init / for-update expression is a DISCARD position
                // too (its value is unused) — share the ExprStmt aggregate
                // chokepoint (lowerDiscardedExpr) so an aggregate ternary/comma
                // in a for-clause routes through the carrier, not lowerExpr's
                // anti-resurrection fail-loud (D-CSUBSET-AGGREGATE-VALUED-
                // CONTROL-EXPR).
                return lowerDiscardedExpr(node);
        }
    }

    // Lower an expression whose VALUE is DISCARDED — an `ExprStmt`, or a bare
    // for-init / for-update clause — emitted for its SIDE EFFECTS only. An
    // AGGREGATE-typed discarded expression (a compound literal
    // `(struct S){f(),g()};`, an aggregate ternary `(cond ? a : b);`, a comma/
    // SeqExpr `(f(), s);`, a struct-returning call `g();`, or a bare aggregate
    // lvalue) has NO SSA rvalue under the memory-based aggregate model — there is
    // no aggregate-width value to produce-and-drop. Route EVERY aggregate-typed
    // discard through ONE chokepoint — `lowerLvalueAddress` — so coverage is by
    // construction with NO per-kind AND NO per-POSITION miss (both the ExprStmt
    // site and the for-clause site funnel HERE): it resolves the value's ADDRESS
    // (materializing a slot + running operand/arm side effects for an rvalue
    // carrier; returning the existing storage for a named lvalue) and drops it.
    // This completes the by-value aggregate-rvalue carriers across BOTH discard
    // positions — the compound-literal slot (D-CSUBSET-BITFIELD-RVALUE-RUNTIME)
    // AND the aggregate Ternary / comma-SeqExpr control-expr carrier
    // (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR) — and keeps `lowerExpr`'s per-kind
    // anti-resurrection fail-louds (ConstructAggregate / Ternary / SeqExpr)
    // reachable ONLY by genuine internal misrouting (unreachable from user code).
    // A scalar discard lowers as an ordinary rvalue via `lowerExpr`.
    [[nodiscard]] bool lowerDiscardedExpr(HirNodeId expr) {
        if (TypeId const et = hir.typeId(expr); et.valid()) {
            // A memory-resident result (Struct/Union/Array AND a wide `_BitInt(N>64)`,
            // D-CSUBSET-BITINT-C2-WIDE) has no bare-SSA value — route it BY ADDRESS, the
            // same by-construction path aggregates use. `isMemoryResidentType` folds the
            // wide-BitInt case in (previously a discarded wide expr relied on lowerExpr's
            // request-flip — harmless, but off the shared funnel).
            if (isMemoryResidentType(interner, et)) {
                return lowerLvalueAddress(expr).valid();
            }
        }
        return lowerExpr(expr).valid();
    }

    // ── FC12a-core variadic CALLEE lowering (D-FC12A-VARIADIC-CALLEE) ────────────
    //
    // SysV AMD64 §3.5.7. `va_list` is `__va_list_tag[1]`; its lvalue ADDRESS is the
    // tag base. All four field offsets + the reg-save geometry + the reg-vs-overflow
    // thresholds come from `config.vaListLayout` (CONFIG, agnostic) — this code
    // never branches on cc.name/arch/format. The two FRAME addresses (reg-save-area,
    // incoming-overflow-area) are emitted as the leaf opcodes VaRegSaveAreaAddr /
    // VaOverflowArgAreaAddr that the LIR callconv pass fills in once it owns the
    // frame layout (the ReadIndirectResult precedent).

    // The `__va_list_tag*` base address of a va_list lvalue `apChild`. `ap` is
    // `__va_list_tag[1]`, so its lvalue address IS the address of its first (only)
    // tag element — the base every field-Gep indexes from.
    [[nodiscard]] MirInstId vaTagBase(HirNodeId apChild) {
        return lowerLvalueAddress(apChild);
    }

    // `&tag.<field>` as a pointer to the field — a base+disp Gep (the FC7 member-
    // address pattern). `fieldPtrType` is the pointer-to-field type for the Store/Load.
    [[nodiscard]] MirInstId vaFieldPtr(MirInstId tagBase,
                                       VaListLayout::Field const& f,
                                       TypeId fieldPtrType) {
        MirInstId const off = constInt(static_cast<std::int64_t>(f.byteOffset));
        if (!off.valid()) return InvalidMirInst;
        std::array<MirInstId, 2> ops{tagBase, off};
        return mir.addInst(MirOpcode::Gep, ops, fieldPtrType);
    }

    // `va_start(ap, last)` → 4 field Stores into the tag. gp_offset = fixedGpr *
    // gpSlotBytes; fp_offset = gpOffsetLimit + fixedFpr * fpSlotBytes (the SysV
    // cursor PAST the fixed args, into the SSE block of the save area);
    // reg_save_area = &save (VaRegSaveAreaAddr); overflow_arg_area = &incoming-stack
    // (VaOverflowArgAreaAddr). Returns the last Store's id (a valid inst the discard
    // chokepoint accepts — va_start is a void expression). Fail loud if the CC
    // declared no vaListLayout.
    [[nodiscard]] MirInstId lowerVaStart(HirNodeId node) {
        if (!config.vaListLayout.has_value()) {
            unsupported(node, "variadic callee (va_start) is unsupported for this "
                              "target's calling convention — it declares no "
                              "'vaListLayout' (D-FC12A-VARIADIC-CALLEE)");
            return InvalidMirInst;
        }
        VaListLayout const& vl = *config.vaListLayout;
        auto kids = hir.children(node);
        if (kids.size() != 1) {
            unsupported(node, "malformed VaStart (expect 1 child)");
            return InvalidMirInst;
        }
        // FC12b: strategy-dispatch with a fail-loud default so no site half-migrates
        // and silently runs the SysV path on a Win64 (or unknown) layout.
        switch (vl.strategy) {
        case VaListStrategy::SysVRegisterSave: {
            // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: a variadic callee
            // whose FIXED params overflow the register-save area has named param(s) on
            // the incoming STACK; SysV §3.5.7 requires overflow_arg_area to point PAST
            // them (baked into the VaOverflowArgAreaAddr payload). `currentFnFixedStack
            // Bytes_` is the EXACT named-stack byte count, accumulated in param order
            // as the reception loop ran — a stacked scalar contributes one slot
            // (gpSlotBytes), a stacked by-value AGGREGATE (it straddled the reg/stack
            // boundary, received all-or-nothing) contributes its rounded span. This
            // REPLACES the old `(gprOver+fprOver)*gpSlotBytes` slot-count, which
            // silently UNDERCOUNTED a stacked aggregate (the all-or-nothing cursor
            // backfills or clamps — never EXCEEDS the pool — so the slot delta missed
            // the aggregate's bytes entirely → the deferral's fail-loud guard).
            std::uint32_t const fixedStackBytes = currentFnFixedStackBytes_;
            MirInstId const tagBase = vaTagBase(kids[0]);
            if (!tagBase.valid()) return InvalidMirInst;

            TypeId const u32      = interner.primitive(TypeKind::U32);
            TypeId const u32Ptr   = interner.pointer(u32);
            TypeId const voidPtr  = interner.pointer(interner.primitive(TypeKind::Void));
            TypeId const voidPtrPtr = interner.pointer(voidPtr);

            // gp_offset = fixedGpr * gpSlotBytes.
            std::uint32_t const gpInit = currentFnFixedGpr_ * vl.gpSlotBytes;
            // fp_offset = gpOffsetLimit + fixedFpr * fpSlotBytes (SSE block follows GPR).
            std::uint32_t const fpInit = vl.gpOffsetLimit + currentFnFixedFpr_ * vl.fpSlotBytes;

            MirInstId const gpField = vaFieldPtr(tagBase, vl.gpOffsetField, u32Ptr);
            if (!gpField.valid()) return InvalidMirInst;
            {
                std::array<MirInstId, 2> st{constIntOfType(gpInit, u32), gpField};
                mir.addInst(MirOpcode::Store, st);
            }
            MirInstId const fpField = vaFieldPtr(tagBase, vl.fpOffsetField, u32Ptr);
            if (!fpField.valid()) return InvalidMirInst;
            {
                std::array<MirInstId, 2> st{constIntOfType(fpInit, u32), fpField};
                mir.addInst(MirOpcode::Store, st);
            }
            // reg_save_area = &save.
            MirInstId const rsaField = vaFieldPtr(tagBase, vl.regSaveAreaField, voidPtrPtr);
            if (!rsaField.valid()) return InvalidMirInst;
            MirInstId const rsaVal =
                mir.addInst(MirOpcode::VaRegSaveAreaAddr, {}, voidPtr);
            {
                std::array<MirInstId, 2> st{rsaVal, rsaField};
                mir.addInst(MirOpcode::Store, st);
            }
            // overflow_arg_area = &incoming-stack-args, displaced PAST any named
            // params that overflowed onto the incoming stack (payload = fixedStackBytes,
            // 0 for the common case). LIR materialization adds it to the overflow base.
            MirInstId const ovfField = vaFieldPtr(tagBase, vl.overflowArgAreaField, voidPtrPtr);
            if (!ovfField.valid()) return InvalidMirInst;
            MirInstId const ovfVal =
                mir.addInst(MirOpcode::VaOverflowArgAreaAddr, {}, voidPtr,
                            /*payload=*/fixedStackBytes);
            std::array<MirInstId, 2> st{ovfVal, ovfField};
            return mir.addInst(MirOpcode::Store, st);   // last Store — a valid void inst
        }
        case VaListStrategy::HomogeneousPointer: {
            // Win64 / Apple arm64: `va_list` is a `char*` (a single pointer, NOT a tag
            // struct), so its lvalue ADDRESS is a `char**` slot we store the start
            // pointer into. `va_arg` linearly bumps that pointer.
            //   * Win64 (variadicUsesOverflowBase=false): start = &home[namedArgCount]
            //     (VaHomeArgAreaAddr, payload = the slot-aligned named-arg count) — the
            //     first vararg in the CONTIGUOUS home+overflow area.
            //   * Apple arm64 (variadicUsesOverflowBase=true): start = the OVERFLOW base
            //     (VaOverflowArgAreaAddr) — Apple has NO home area; every vararg is
            //     stacked (the CALLER forces it, variadicArgsAlwaysStack), so the first
            //     vararg sits at the overflow base. NO payload (the named args are NOT
            //     in a home block — they stay in their registers, read via SSA).
            MirInstId const apPtr = lowerLvalueAddress(kids[0]);
            if (!apPtr.valid()) return InvalidMirInst;
            TypeId const voidPtr = interner.pointer(interner.primitive(TypeKind::Void));
            MirInstId const first =
                vl.variadicUsesOverflowBase
                    ? mir.addInst(MirOpcode::VaOverflowArgAreaAddr, {}, voidPtr)
                    : mir.addInst(MirOpcode::VaHomeArgAreaAddr, {}, voidPtr,
                                  /*payload=*/currentFnFixedFlat_);
            std::array<MirInstId, 2> st{first, apPtr};
            return mir.addInst(MirOpcode::Store, st);
        }
        case VaListStrategy::Aapcs64DualCursor: {
            // AAPCS64 §B.4: initialize the 5-field `__va_list`. `ap` is the struct
            // itself, so its lvalue ADDRESS is the struct base every field-Gep indexes
            // from. The GR/VR save area is a single contiguous callee-local zone the
            // prologue spilled x0..x7 then v0..v7 into (VaRegSaveAreaAddr = its base):
            //   __gr_top  = rsa + gpSaveCount*gpSlotBytes            (past the GR block)
            //   __vr_top  = rsa + gpSaveCount*gpSlotBytes
            //                   + fpSaveCount*fpSlotBytes            (past the VR block)
            //   __stack   = VaOverflowArgAreaAddr                    (incoming stack args)
            //   __gr_offs = -(gpSaveCount - fixedGpr)*gpSlotBytes    (NEGATIVE; →0)
            //   __vr_offs = -(fpSaveCount - fixedFpr)*fpSlotBytes    (NEGATIVE; →0)
            // The offs are NEGATIVE i32: va_arg adds them to <gr|vr>_top (which points
            // PAST the block) to address the right slot, and counts up toward 0.
            //
            // FC12-deferral④ (D-FC12C-AAPCS64-VARIADIC-OVERFLOW-FIXED-STACK-ARGS):
            // mirrors the SysV arm. A variadic callee whose FIXED params overflow the
            // save area (fixedGpr > gpSaveCount / fixedFpr > fpSaveCount) has named
            // param(s) on the incoming stack: __stack must skip them (baked into the
            // VaOverflowArgAreaAddr payload below) AND the per-class cursor (__gr_offs /
            // __vr_offs) is CLAMPED to 0 (no register slots remain for varargs of that
            // class) — the old `-(saveCount - fixedCount)` would underflow unsigned.
            // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: the EXACT named-
            // stack byte count, accumulated in param order as the reception loop ran
            // (a stacked scalar = one 8B slot; a stacked by-value AGGREGATE/HFA that
            // straddled = its rounded span). __stack skips them. REPLACES the old
            // `(gprOver+fprOver)*gpSlotBytes` slot-count that undercounted a stacked
            // aggregate (under the AAPCS64 EXHAUST rule the class cursor clamps to
            // EXACTLY the pool on the straddle, so the slot delta is 0 — it missed the
            // aggregate's bytes). The __gr_offs/__vr_offs clamp below reads
            // currentFnFixedGpr_/Fpr_ directly (the reception loop set them with the
            // exhaust/backfill policy), so the cursors are already correct.
            std::uint32_t const fixedStackBytes = currentFnFixedStackBytes_;
            MirInstId const base = vaTagBase(kids[0]);   // the `__va_list` struct base
            if (!base.valid()) return InvalidMirInst;

            TypeId const i32      = interner.primitive(TypeKind::I32);
            TypeId const i32Ptr   = interner.pointer(i32);
            TypeId const voidPtr  = interner.pointer(interner.primitive(TypeKind::Void));
            TypeId const voidPtrPtr = interner.pointer(voidPtr);

            std::uint32_t const grBlockBytes = vl.gpSaveCount * vl.gpSlotBytes;
            std::uint32_t const vrBlockBytes = vl.fpSaveCount * vl.fpSlotBytes;

            // rsa base (the spilled register-save-area, materialized by lir_callconv).
            MirInstId const rsa =
                mir.addInst(MirOpcode::VaRegSaveAreaAddr, {}, voidPtr);

            // __gr_top = rsa + grBlockBytes.
            MirInstId const grTopField = vaFieldPtr(base, vl.grTopField, voidPtrPtr);
            if (!grTopField.valid()) return InvalidMirInst;
            {
                MirInstId const k = constInt(static_cast<std::int64_t>(grBlockBytes));
                if (!k.valid()) return InvalidMirInst;
                std::array<MirInstId, 2> gepOps{rsa, k};
                MirInstId const grTop = mir.addInst(MirOpcode::Gep, gepOps, voidPtr);
                std::array<MirInstId, 2> st{grTop, grTopField};
                mir.addInst(MirOpcode::Store, st);
            }
            // __vr_top = rsa + grBlockBytes + vrBlockBytes.
            MirInstId const vrTopField = vaFieldPtr(base, vl.vrTopField, voidPtrPtr);
            if (!vrTopField.valid()) return InvalidMirInst;
            {
                MirInstId const k =
                    constInt(static_cast<std::int64_t>(grBlockBytes + vrBlockBytes));
                if (!k.valid()) return InvalidMirInst;
                std::array<MirInstId, 2> gepOps{rsa, k};
                MirInstId const vrTop = mir.addInst(MirOpcode::Gep, gepOps, voidPtr);
                std::array<MirInstId, 2> st{vrTop, vrTopField};
                mir.addInst(MirOpcode::Store, st);
            }
            // __stack = &incoming-stack-args (the overflow base), displaced PAST any
            // named params that overflowed onto the incoming stack (payload =
            // fixedStackBytes, 0 for the common case). LIR adds it to the overflow base.
            MirInstId const stackField = vaFieldPtr(base, vl.stackField, voidPtrPtr);
            if (!stackField.valid()) return InvalidMirInst;
            {
                MirInstId const ovf =
                    mir.addInst(MirOpcode::VaOverflowArgAreaAddr, {}, voidPtr,
                                /*payload=*/fixedStackBytes);
                std::array<MirInstId, 2> st{ovf, stackField};
                mir.addInst(MirOpcode::Store, st);
            }
            // __gr_offs = -(gpSaveCount - fixedGpr) * gpSlotBytes (NEGATIVE i32),
            // CLAMPED to 0 when the fixed GPR params have consumed the whole GR block
            // (fixedGpr >= gpSaveCount): no GR slots remain for varargs, and the naive
            // subtraction would underflow (unsigned). va_arg's reg arm tests
            // ICmpSlt(offs, 0) → 0 routes every GPR vararg straight to __stack. (The
            // fixed-stack-arg displacement is carried by the VaOverflowArgAreaAddr
            // payload above; the FOLD 1 fail-loud rejects the aggregate-straddle case.)
            MirInstId const grOffsField = vaFieldPtr(base, vl.grOffsField, i32Ptr);
            if (!grOffsField.valid()) return InvalidMirInst;
            {
                std::int64_t const grOffs =
                    (currentFnFixedGpr_ >= vl.gpSaveCount)
                        ? 0
                        : -static_cast<std::int64_t>(
                              (vl.gpSaveCount - currentFnFixedGpr_) * vl.gpSlotBytes);
                std::array<MirInstId, 2> st{constIntOfType(grOffs, i32), grOffsField};
                mir.addInst(MirOpcode::Store, st);
            }
            // __vr_offs = -(fpSaveCount - fixedFpr) * fpSlotBytes (NEGATIVE i32),
            // CLAMPED to 0 when the fixed FP params have consumed the whole VR block
            // (fixedFpr >= fpSaveCount) — same rationale as __gr_offs.
            MirInstId const vrOffsField = vaFieldPtr(base, vl.vrOffsField, i32Ptr);
            if (!vrOffsField.valid()) return InvalidMirInst;
            {
                std::int64_t const vrOffs =
                    (currentFnFixedFpr_ >= vl.fpSaveCount)
                        ? 0
                        : -static_cast<std::int64_t>(
                              (vl.fpSaveCount - currentFnFixedFpr_) * vl.fpSlotBytes);
                std::array<MirInstId, 2> st{constIntOfType(vrOffs, i32), vrOffsField};
                return mir.addInst(MirOpcode::Store, st);   // last Store — a valid inst
            }
        }
        }
        unsupported(node, "internal: unknown VaListStrategy in va_start");
        return InvalidMirInst;
    }

    // `va_end(ap)` → nothing (SysV + Win64 teardown is a no-op). A valid placeholder
    // value for the discard chokepoint; DCE drops it. (The node + opcode-less
    // lowering are the hook a future ABI needing real teardown would fill.)
    [[nodiscard]] MirInstId lowerVaEnd(HirNodeId node) {
        if (!config.vaListLayout.has_value()) {
            unsupported(node, "variadic callee (va_end) is unsupported for this "
                              "target's calling convention — it declares no "
                              "'vaListLayout' (D-FC12A-VARIADIC-CALLEE)");
            return InvalidMirInst;
        }
        // FC12b/FC12c: strategy-dispatch. va_end teardown is a no-op on all three
        // realized ABIs (SysV register-save, Win64/Apple pointer, AAPCS64 dual-cursor).
        switch (config.vaListLayout->strategy) {
        case VaListStrategy::SysVRegisterSave:
        case VaListStrategy::HomogeneousPointer:
        case VaListStrategy::Aapcs64DualCursor:
            return constInt(0);   // no teardown on any realized ABI
        }
        unsupported(node, "internal: unknown VaListStrategy in va_end");
        return InvalidMirInst;
    }

    // `va_arg(ap, T)` for a SCALAR T → the reg-vs-overflow diamond (SysV §3.5.7).
    // STRUCT/UNION T STAYS FAIL-LOUD this cycle (the FC12a-struct boundary). The
    // diamond mirrors the Ternary value-diamond: classify T (GPR vs SSE) → pick the
    // gp_offset/fp_offset cursor → if cursor < limit, read reg_save_area+cursor &
    // bump the cursor by the slot stride; else read overflow_arg_area & bump it by a
    // stack slot → Phi the two address arms → Load the value.
    [[nodiscard]] MirInstId lowerVaArg(HirNodeId node) {
        if (!config.vaListLayout.has_value()) {
            unsupported(node, "variadic callee (va_arg) is unsupported for this "
                              "target's calling convention — it declares no "
                              "'vaListLayout' (D-FC12A-VARIADIC-CALLEE)");
            return InvalidMirInst;
        }
        VaListLayout const& vl = *config.vaListLayout;
        TypeId const t = hir.typeId(node);   // the read type T (the node's result)
        if (!t.valid()) {
            unsupported(node, "VaArg has no result type");
            return InvalidMirInst;
        }
        // FC12a-struct: an AGGREGATE-typed va_arg has NO aggregate-width SSA value
        // (the compiler's aggregate model is MEMORY-based). It MUST be lowered BY
        // ADDRESS via lowerLvalueAddress's VaArg arm (→ lowerVaArgAggregate), exactly
        // like the aggregate Ternary/SeqExpr carriers; every consumer (VarDecl init /
        // AssignStmt / discard) reaches it that way. Reaching here with an aggregate
        // type means a NEW consumer lowered an aggregate va_arg as a bare rvalue —
        // fail loud rather than synthesize a Phi-of-aggregate / aggregate-width Load
        // (a latent miscompile), mirroring the Ternary anti-resurrection guard.
        // D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt` va_arg is memory-resident too —
        // it must reach the by-address VaArg arm; a bare scalar read would silently
        // load only its low limb. (Wide-BitInt va_arg codegen is itself a later cycle,
        // so the address arm currently fails loud too — but never SILENTLY here.)
        if (isByValueClass(interner, t)) {
            unsupported(node,
                "internal: an aggregate-typed va_arg must be lowered by ADDRESS via "
                "lowerLvalueAddress, never as a bare SSA rvalue — the MIR has no "
                "aggregate-width value (D-CSUBSET-AGGREGATE-VALUED-CONTROL-EXPR / "
                "D-FC12A-VARIADIC-CALLEE)");
            return InvalidMirInst;
        }
        auto kids = hir.children(node);
        if (kids.size() != 2) {
            unsupported(node, "malformed VaArg (expect [apExpr, TypeRef])");
            return InvalidMirInst;
        }
        MirInstId const tagBase = vaTagBase(kids[0]);   // child 1 (TypeRef) NEVER lowered
        if (!tagBase.valid()) return InvalidMirInst;

        // FC12b: strategy-dispatch. The Win64 HomogeneousPointer path is LINEAR (no
        // diamond) and returns here; the Aapcs64 path fails loud; SysVRegisterSave
        // falls through to the reg-vs-overflow diamond below. A fail-loud default
        // guards against a half-migrated site silently running the SysV diamond on a
        // non-SysV layout.
        if (vl.strategy == VaListStrategy::HomogeneousPointer) {
            // `va_list` is a `char*`: tagBase is its lvalue address (a `char**`).
            // Load the current cursor, Load T from it, bump by namedArgSlotBytes,
            // store the bumped cursor back. Crosses home→overflow uniformly because
            // they are contiguous from the va_start base.
            TypeId const voidPtr = interner.pointer(interner.primitive(TypeKind::Void));
            std::array<MirInstId, 1> loadApOps{tagBase};
            MirInstId const apVal = mir.addInst(MirOpcode::Load, loadApOps, voidPtr);
            std::array<MirInstId, 1> loadValOps{apVal};
            MirInstId const result = mir.addInst(MirOpcode::Load, loadValOps, t);
            MirInstId const stepK =
                constInt(static_cast<std::int64_t>(vl.namedArgSlotBytes));
            if (!stepK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> gepOps{apVal, stepK};
            MirInstId const apNext = mir.addInst(MirOpcode::Gep, gepOps, voidPtr);
            std::array<MirInstId, 2> st{apNext, tagBase};
            mir.addInst(MirOpcode::Store, st);
            return result;
        }
        if (vl.strategy == VaListStrategy::Aapcs64DualCursor) {
            // ── AAPCS64 §B.6: the per-class dual-cursor diamond (SCALAR) ──
            // `ap` is the `__va_list` struct; tagBase is its address. Classify T:
            // GPR (integer/pointer) walks __gr_offs/__gr_top/gpSlotBytes; FPR (float)
            // walks __vr_offs/__vr_top/fpSlotBytes. The cursor is a NEGATIVE i32 that
            // counts up toward 0; while < 0 a register slot remains.
            bool const isFpr = (scalarArgClass(t) == AbiPieceClass::Fpr);
            VaListLayout::Field const& offField =
                isFpr ? vl.vrOffsField : vl.grOffsField;
            VaListLayout::Field const& topField =
                isFpr ? vl.vrTopField : vl.grTopField;
            std::uint32_t const regSlotBytes =
                isFpr ? vl.fpSlotBytes : vl.gpSlotBytes;

            TypeId const i32      = interner.primitive(TypeKind::I32);
            TypeId const i64      = interner.primitive(TypeKind::I64);
            TypeId const i32Ptr   = interner.pointer(i32);
            TypeId const voidPtr  = interner.pointer(interner.primitive(TypeKind::Void));
            TypeId const voidPtrPtr = interner.pointer(voidPtr);
            TypeId const boolTy   = interner.primitive(TypeKind::Bool);

            // curOffs = Load(offPtr, i32) — a NEGATIVE cursor.
            MirInstId const offPtr = vaFieldPtr(tagBase, offField, i32Ptr);
            if (!offPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadOffOps{offPtr};
            MirInstId const curOffs = mir.addInst(MirOpcode::Load, loadOffOps, i32);

            // inReg = curOffs < 0 (a SIGNED compare — §B.6: gr_offs<0 ⇒ reg slot left).
            MirInstId const zeroK = constIntOfType(0, i32);
            std::array<MirInstId, 2> cmpOps{curOffs, zeroK};
            MirInstId const inReg = mir.addInst(MirOpcode::ICmpSlt, cmpOps, boolTy);

            MirBlockId const regBB  = mir.createBlock(StructCfMarker::IfThen);
            MirBlockId const ovfBB  = mir.createBlock(StructCfMarker::IfElse);
            MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
            mir.addCondBr(inReg, regBB, ovfBB);

            // ── register arm: addr = <gr|vr>_top + SExt(curOffs); cursor += slot. ──
            // BLOCKER-2: the i32 cursor is NEGATIVE; it MUST be SIGN-EXTENDED to i64
            // before the byte Gep. A 32-bit -40 used directly as a Gep index would be
            // zero-extended to 0xFFFFFFD8 (≈ +4 GiB) → a wild address → segfault.
            // SExt (AArch64 SXTW) gives the correct negative 64-bit index.
            mir.beginBlock(regBB);
            MirInstId const topPtr = vaFieldPtr(tagBase, topField, voidPtrPtr);
            if (!topPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadTopOps{topPtr};
            MirInstId const top = mir.addInst(MirOpcode::Load, loadTopOps, voidPtr);
            std::array<MirInstId, 1> sextOps{curOffs};
            MirInstId const idx64 = mir.addInst(MirOpcode::SExt, sextOps, i64);
            std::array<MirInstId, 2> regGepOps{top, idx64};   // negative byte index
            MirInstId const regAddr = mir.addInst(MirOpcode::Gep, regGepOps, voidPtr);
            // bump cursor: store curOffs + regSlotBytes back.
            MirInstId const slotK =
                constIntOfType(static_cast<std::int64_t>(regSlotBytes), i32);
            std::array<MirInstId, 2> addOps{curOffs, slotK};
            MirInstId const bumped = mir.addInst(MirOpcode::Add, addOps, i32);
            std::array<MirInstId, 2> bumpStore{bumped, offPtr};
            mir.addInst(MirOpcode::Store, bumpStore);
            MirBlockId const regPred = mir.currentlyOpenBlock();
            mir.addBr(joinBB);

            // ── overflow arm: addr = __stack; __stack += gpSlotBytes. ──
            // The stack bump is the NSAA round-up-to-8 quantum (gpSlotBytes=8) even for
            // a double on the stack — NOT fpSlotBytes=16.
            mir.beginBlock(ovfBB);
            MirInstId const stackPtr = vaFieldPtr(tagBase, vl.stackField, voidPtrPtr);
            if (!stackPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadStackOps{stackPtr};
            MirInstId const stack = mir.addInst(MirOpcode::Load, loadStackOps, voidPtr);
            MirInstId const stepK =
                constInt(static_cast<std::int64_t>(vl.gpSlotBytes));
            if (!stepK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> stackGepOps{stack, stepK};
            MirInstId const stackNext = mir.addInst(MirOpcode::Gep, stackGepOps, voidPtr);
            std::array<MirInstId, 2> stackStore{stackNext, stackPtr};
            mir.addInst(MirOpcode::Store, stackStore);
            MirBlockId const ovfPred = mir.currentlyOpenBlock();
            mir.addBr(joinBB);

            // ── join: pick the address, then Load the value. ──
            mir.beginBlock(joinBB);
            std::array<MirPhiIncoming, 2> incomings{
                MirPhiIncoming{regAddr, regPred},
                MirPhiIncoming{stack,   ovfPred},
            };
            MirInstId const argPtr = mir.addPhi(voidPtr, incomings);
            std::array<MirInstId, 1> finalLoad{argPtr};
            return mir.addInst(MirOpcode::Load, finalLoad, t);
        }
        if (vl.strategy != VaListStrategy::SysVRegisterSave) {
            unsupported(node, "internal: unknown VaListStrategy in va_arg");
            return InvalidMirInst;
        }

        // ── SysVRegisterSave: the reg-vs-overflow diamond (SysV §3.5.7) ──
        // Classify the SCALAR: SSE (FPR) reads the fp cursor; INTEGER (GPR) the gp
        // cursor. (A scalar isn't an aggregate; `scalarArgClass` is the direct
        // equivalent of the eightbyte INTEGER/SSE split for one register.)
        bool const isSse = (scalarArgClass(t) == AbiPieceClass::Fpr);
        VaListLayout::Field const& offField =
            isSse ? vl.fpOffsetField : vl.gpOffsetField;
        std::uint32_t const offLimit = isSse ? vl.fpOffsetLimit : vl.gpOffsetLimit;
        std::uint32_t const regSlotBytes = isSse ? vl.fpSlotBytes : vl.gpSlotBytes;

        TypeId const u32      = interner.primitive(TypeKind::U32);
        TypeId const u32Ptr   = interner.pointer(u32);
        TypeId const voidPtr  = interner.pointer(interner.primitive(TypeKind::Void));
        TypeId const voidPtrPtr = interner.pointer(voidPtr);

        // Load the current per-class offset cursor.
        MirInstId const offPtr = vaFieldPtr(tagBase, offField, u32Ptr);
        if (!offPtr.valid()) return InvalidMirInst;
        std::array<MirInstId, 1> loadOffOps{offPtr};
        MirInstId const curOff = mir.addInst(MirOpcode::Load, loadOffOps, u32);

        // inReg = curOff < offLimit.
        MirInstId const limitK = constIntOfType(offLimit, u32);
        std::array<MirInstId, 2> cmpOps{curOff, limitK};
        MirInstId const inReg = mir.addInst(MirOpcode::ICmpUlt, cmpOps,
                                            interner.primitive(TypeKind::Bool));

        MirBlockId const regBB = mir.createBlock(StructCfMarker::IfThen);
        MirBlockId const ovfBB = mir.createBlock(StructCfMarker::IfElse);
        MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
        mir.addCondBr(inReg, regBB, ovfBB);

        // The argument's ADDRESS is carried through the diamond as a `void*` (every
        // arm produces a byte address; the join Phi unifies them and the final Load
        // reads T from it). Keeping the address `void*` (not `T*`) avoids minting a
        // per-T pointer type for the carrier — the Load's result type is what selects
        // the load width, agnostic to the address operand's pointee.

        // ── register arm: addr = reg_save_area + curOff; cursor += regSlotBytes. ──
        mir.beginBlock(regBB);
        MirInstId const rsaField = vaFieldPtr(tagBase, vl.regSaveAreaField, voidPtrPtr);
        if (!rsaField.valid()) return InvalidMirInst;
        std::array<MirInstId, 1> loadRsaOps{rsaField};
        MirInstId const rsa = mir.addInst(MirOpcode::Load, loadRsaOps, voidPtr);
        std::array<MirInstId, 2> regGepOps{rsa, curOff};   // byte-offset index
        MirInstId const regAddr = mir.addInst(MirOpcode::Gep, regGepOps, voidPtr);
        // bump cursor: store curOff + regSlotBytes back.
        MirInstId const slotK = constIntOfType(static_cast<std::int64_t>(regSlotBytes), u32);
        std::array<MirInstId, 2> addOps{curOff, slotK};
        MirInstId const bumped = mir.addInst(MirOpcode::Add, addOps, u32);
        std::array<MirInstId, 2> bumpStore{bumped, offPtr};
        mir.addInst(MirOpcode::Store, bumpStore);
        MirBlockId const regPred = mir.currentlyOpenBlock();
        mir.addBr(joinBB);

        // ── overflow arm: addr = overflow_arg_area; overflow += one stack slot. ──
        mir.beginBlock(ovfBB);
        MirInstId const ovfPtr = vaFieldPtr(tagBase, vl.overflowArgAreaField, voidPtrPtr);
        if (!ovfPtr.valid()) return InvalidMirInst;
        std::array<MirInstId, 1> loadOvfOps{ovfPtr};
        MirInstId const ovfArea = mir.addInst(MirOpcode::Load, loadOvfOps, voidPtr);
        // SysV rounds each stack vararg up to one 8-byte (pointer-width) slot; a
        // scalar (≤8B) consumes exactly one. Bump overflow_arg_area by gpSlotBytes
        // (= pointer width = the stack-arg stride) via a base+disp Gep.
        MirInstId const stepK =
            constInt(static_cast<std::int64_t>(vl.gpSlotBytes));
        if (!stepK.valid()) return InvalidMirInst;
        std::array<MirInstId, 2> ovfStepOps{ovfArea, stepK};
        MirInstId const ovfNext = mir.addInst(MirOpcode::Gep, ovfStepOps, voidPtr);
        std::array<MirInstId, 2> ovfStore{ovfNext, ovfPtr};
        mir.addInst(MirOpcode::Store, ovfStore);
        MirBlockId const ovfPred = mir.currentlyOpenBlock();
        mir.addBr(joinBB);

        // ── join: pick the address, then Load the value. ──
        mir.beginBlock(joinBB);
        std::array<MirPhiIncoming, 2> incomings{
            MirPhiIncoming{regAddr, regPred},
            MirPhiIncoming{ovfArea, ovfPred},
        };
        MirInstId const argPtr = mir.addPhi(voidPtr, incomings);
        std::array<MirInstId, 1> finalLoad{argPtr};
        return mir.addInst(MirOpcode::Load, finalLoad, t);
    }

    // `va_arg(ap, T)` for an AGGREGATE T (struct/union) → the struct's ADDRESS
    // (SysV §3.5.7). AGNOSTIC: the field offsets / save-area geometry come from
    // `config.vaListLayout`, the eightbyte CLASSIFICATION from `classifyAggregate`
    // (never a target/format/cc identity branch). Algorithm:
    //   * Classify T. MEMORY class (>16B / empty) ⇒ the struct sits ENTIRELY by value
    //     in the overflow area (SysV §3.5.7): dispatch EARLY to the overflow-only arm
    //     (no register pieces → no diamond), the read-side mirror of the caller's
    //     appendByValueStackArg (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT).
    //   * InRegisters (1-2 eightbyte pieces): the ATOMIC reg-vs-overflow decision —
    //     ALL pieces' classes must fit in the remaining save-area slots
    //     (gp_offset + num_gp*gpSlot <= gpLimit AND fp_offset + num_fp*fpSlot <=
    //     fpLimit), else the whole aggregate sits in the overflow area by value.
    //     - register arm: copy each piece from reg_save_area+cursor into a fresh
    //       temp at piece.byteOffset, bumping that CLASS's cursor per piece; store
    //       the final cursors back. The temp's address is the result.
    //     - overflow arm: the struct sits by value at overflow_arg_area; that
    //       pointer IS the result; bump overflow_arg_area by roundUp(sizeof T, 8)
    //       (distinct from the scalar arm's one-slot bump).
    //     - join: Phi the two addresses (a void*). NO Load — aggregates by address.
    [[nodiscard]] MirInstId lowerVaArgAggregate(HirNodeId node) {
        if (!config.vaListLayout.has_value()) {
            unsupported(node, "variadic callee (va_arg) is unsupported for this "
                              "target's calling convention — it declares no "
                              "'vaListLayout' (D-FC12A-VARIADIC-CALLEE)");
            return InvalidMirInst;
        }
        VaListLayout const& vl = *config.vaListLayout;
        // FC12b: strategy-dispatch. The SysV eightbyte gather is below; AAPCS64 is
        // FC12c. Fail loud rather than fall through to the SysV by-value gather on a
        // non-SysV layout (a silent wrong-ABI struct read).
        if (vl.strategy == VaListStrategy::HomogeneousPointer) {
            // ── Win64 (ms_x64) AND Apple (apple_arm64) struct va_arg: a contiguous
            // 8-byte-slot walk, value-or-pointer ── (D-FC12B-WIN64-STRUCT-VARARG +
            // D-FC12C-APPLE-ARM64-VARIADIC-CALLEE — this ONE arm covers BOTH CCs: both
            // declare the HomogeneousPointer strategy, so this is NOT a separate
            // arm64-Apple feature). BOTH CCs use a `char*` va_list over a contiguous arg
            // area; Apple's spec forces ALL variadic args (including aggregates) onto the
            // stack in 8-byte slots (`variadicArgsAlwaysStack` handles the caller side),
            // so the callee sees exactly the same LINEAR slot walk as Win64 — no
            // register-save-area, no diamond. Classify T and dispatch on the slot's
            // meaning — the read-side mirror of the caller's `appendByValueArg`. A
            // pow2-≤8B struct sits in the slot(s) BY VALUE (InRegisters); a >16B struct
            // (and, for Win64, any non-pow2/>8B) rides as a hidden POINTER to a caller
            // copy (ByReference). The size-aware InRegisters bump below (Step 2.0) is
            // what makes Apple's 16B HFA — which spans TWO 8-byte slots — advance the
            // cursor correctly (Win64 InRegisters structs are pow2-≤8B ⇒ one slot).
            // `ap` is the `char*` cursor; tagBase is its lvalue address (`char**`).
            // The home/overflow areas are contiguous from the va_start base, so one
            // Gep bump crosses them uniformly, exactly like the scalar arm above.
            TypeId const t = hir.typeId(node);
            if (!t.valid()) {
                unsupported(node, "VaArg has no result type");
                return InvalidMirInst;
            }
            auto const abi = byValueClassify(t);
            if (!abi.has_value()) {
                unsupported(node,
                    "va_arg of a struct/union is not supported by this target's "
                    "calling convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                return InvalidMirInst;
            }
            // The aggregate's byte size drives the InRegisters slot advance (Apple's
            // 16B HFA spans TWO 8-byte slots; Win64's InRegisters structs are pow2-≤8B
            // so roundUp == one slot). A complete, sizeable layout is required.
            StructLayout const* hpLayout = cachedLayout(t);
            if (hpLayout == nullptr) {
                unsupported(node, "va_arg of a struct/union requires a sizeable layout "
                                  "(target 'aggregateLayout' / complete type)");
                return InvalidMirInst;
            }
            auto kids = hir.children(node);
            if (kids.size() != 2) {
                unsupported(node, "malformed VaArg (expect [apExpr, TypeRef])");
                return InvalidMirInst;
            }
            MirInstId const tagBase = vaTagBase(kids[0]);   // child 1 NEVER lowered
            if (!tagBase.valid()) return InvalidMirInst;
            TypeId const voidPtr =
                interner.pointer(interner.primitive(TypeKind::Void));
            // apVal = Load(tagBase) — the current slot cursor (a void* into the
            // contiguous home/overflow arg area).
            std::array<MirInstId, 1> loadApOps{tagBase};
            MirInstId const apVal = mir.addInst(MirOpcode::Load, loadApOps, voidPtr);
            MirInstId structAddr;
            if (abi->kind == AbiPassing::Kind::ByReference) {
                // The slot HOLDS A POINTER to the caller's by-value copy: deref it.
                // apVal is the slot's address (void*); Load the stored pointer (the
                // Load's void* result width reads the pointer, agnostic to pointee).
                std::array<MirInstId, 1> derefOps{apVal};
                structAddr = mir.addInst(MirOpcode::Load, derefOps, voidPtr);
            } else {
                // InRegisters (pow2 ≤8B): the slot IS the storage — the struct's
                // bytes were written into this slot as one I64 by the caller's
                // appendByValueArg. The slot's address (apVal) is the struct's
                // address; the consumer byte-copies sizeof(T) (≤8) bytes from it.
                // NO extra Load (mirrors the SysV overflow-arm `return ovfArea`).
                structAddr = apVal;
            }
            // Bump the cursor by the slot(s) this struct occupied. The advance is a
            // function of `abi->kind` ONLY (NOT the strategy — agnostic):
            //   * ByReference  → ONE slot (namedArgSlotBytes): the slot holds a single
            //     hidden POINTER to the caller copy, regardless of the pointee size.
            //   * InRegisters  → roundUp(sizeof T, namedArgSlotBytes): as MANY 8-byte
            //     slots as the struct's bytes need. For Win64 every InRegisters struct
            //     is pow2-≤8B so roundUp == namedArgSlotBytes (one slot) — byte-for-byte
            //     unchanged. For Apple a 16B HFA spans TWO 8-byte slots, so this bumps
            //     by 16 — without it the NEXT va_arg re-reads this struct's tail (Apple
            //     spec: "assigned to the appropriate number of 8-byte stack slots").
            std::uint64_t const slotQ =
                static_cast<std::uint64_t>(vl.namedArgSlotBytes);
            std::uint64_t const stepBytes =
                (abi->kind == AbiPassing::Kind::ByReference)
                    ? slotQ
                    : ((hpLayout->size + slotQ - 1u) / slotQ) * slotQ;
            MirInstId const stepK =
                constInt(static_cast<std::int64_t>(stepBytes));
            if (!stepK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> gepOps{apVal, stepK};
            MirInstId const apNext = mir.addInst(MirOpcode::Gep, gepOps, voidPtr);
            std::array<MirInstId, 2> st{apNext, tagBase};
            mir.addInst(MirOpcode::Store, st);
            // The aggregate's ADDRESS is the result (by address — NO value Load).
            return structAddr;
        }
        if (vl.strategy == VaListStrategy::Aapcs64DualCursor) {
            // ── AAPCS64 §B.6 va_arg(struct): the per-class dual-cursor GATHER ──
            // (D-FC12C-AAPCS64-HFA-STRUCT-VARARG). The aggregate ABI splits into THREE
            // shapes (the Aapcs64Hfa classifier never mixes piece classes):
            //   * ByReference (>16B): the caller placed a hidden POINTER in ONE GR slot
            //     — read that slot (GR cursor), deref it. Result = the caller's copy.
            //   * InRegisters HFA (1-4 pure-FPR pieces): gather each piece from the VR
            //     save block (`__vr_top`/`__vr_offs`, 16B stride) into a fresh temp.
            //   * InRegisters non-HFA (1-2 pure-GPR pieces): gather from the GR save
            //     block (`__gr_top`/`__gr_offs`, 8B stride) into a fresh temp.
            // GEOMETRY (mirrors the scalar arm above): the class cursor is a NEGATIVE
            // i32 counting UP toward 0; while it WILL STILL BE < 0 after consuming this
            // aggregate's slots, the registers hold it (atomic-fit), else it spills to
            // `__stack`. ALL cursor arithmetic stays i32; the i32→i64 SExt (SXTW) is
            // used ONLY as the byte Gep index, NEVER stored back (a stored i64 would
            // truncate / type-mismatch). The bump stored back is Add(cur_i32,K_i32,i32).
            TypeId const t = hir.typeId(node);
            if (!t.valid()) {
                unsupported(node, "VaArg has no result type");
                return InvalidMirInst;
            }
            auto const abi = byValueClassify(t);
            if (!abi.has_value()) {
                unsupported(node,
                    "va_arg of a struct/union is not supported by this target's "
                    "calling convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                return InvalidMirInst;
            }
            StructLayout const* layout = cachedLayout(t);
            if (layout == nullptr) {
                unsupported(node, "va_arg of a struct/union requires a sizeable layout "
                                  "(target 'aggregateLayout' / complete type)");
                return InvalidMirInst;
            }
            auto kids = hir.children(node);
            if (kids.size() != 2) {
                unsupported(node, "malformed VaArg (expect [apExpr, TypeRef])");
                return InvalidMirInst;
            }
            MirInstId const tagBase = vaTagBase(kids[0]);   // child 1 NEVER lowered
            if (!tagBase.valid()) return InvalidMirInst;

            // Count + class-check the pieces (the classifier guarantees pure-class).
            std::uint32_t numGp = 0, numFp = 0;
            for (AbiPiece const& p : abi->pieces) {
                if (p.cls == AbiPieceClass::Fpr) ++numFp; else ++numGp;
            }
            bool const isHfa =
                (abi->kind == AbiPassing::Kind::InRegisters && numFp > 0 && numGp == 0);
            bool const isGprInReg =
                (abi->kind == AbiPassing::Kind::InRegisters && numGp > 0 && numFp == 0);
            if (abi->kind == AbiPassing::Kind::InRegisters && !isHfa && !isGprInReg) {
                // Mixed-class InRegisters — AAPCS64 §5.4 forbids it for the Aapcs64Hfa
                // strategy (HFA is pure-FPR; a non-HFA ≤16B is pure-GPR). Fail loud
                // rather than gather from the wrong save block.
                unsupported(node,
                    "AAPCS64 va_arg: mixed-class aggregate (HFA or non-HFA only) "
                    "(internal: classifier invariant violated)");
                return InvalidMirInst;
            }

            TypeId const i32       = interner.primitive(TypeKind::I32);
            TypeId const i64       = interner.primitive(TypeKind::I64);
            TypeId const i32Ptr    = interner.pointer(i32);
            TypeId const voidPtr   = interner.pointer(interner.primitive(TypeKind::Void));
            TypeId const voidPtrPtr = interner.pointer(voidPtr);
            TypeId const boolTy    = interner.primitive(TypeKind::Bool);
            MirInstId const zeroK  = constIntOfType(0, i32);

            // The overflow (`__stack`) bump quantum: AAPCS64 §B.4 rounds EVERY stack
            // vararg up to an 8-byte NSAA slot — gpSlotBytes (NOT fpSlotBytes, even for
            // an HFA; hazard H6). A ByReference struct on the stack occupies ONE 8-byte
            // pointer slot; an InRegisters struct occupies roundUp(size,8) bytes.
            std::uint64_t const stackQ = static_cast<std::uint64_t>(vl.gpSlotBytes);

            // ── Path A1: ByReference (>16B) — ONE GR slot holds a hidden pointer ──
            if (abi->kind == AbiPassing::Kind::ByReference) {
                MirInstId const offPtr = vaFieldPtr(tagBase, vl.grOffsField, i32Ptr);
                if (!offPtr.valid()) return InvalidMirInst;
                std::array<MirInstId, 1> loadOffOps{offPtr};
                MirInstId const curOffs =
                    mir.addInst(MirOpcode::Load, loadOffOps, i32);
                // inReg = curOffs < 0 (one GR slot left). SINGLE-slot ⇒ the scalar test.
                std::array<MirInstId, 2> cmpOps{curOffs, zeroK};
                MirInstId const inReg = mir.addInst(MirOpcode::ICmpSlt, cmpOps, boolTy);
                MirBlockId const regBB  = mir.createBlock(StructCfMarker::IfThen);
                MirBlockId const ovfBB  = mir.createBlock(StructCfMarker::IfElse);
                MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
                mir.addCondBr(inReg, regBB, ovfBB);

                // reg arm: addr = __gr_top + SExt(curOffs); deref → structPtr; bump +8.
                mir.beginBlock(regBB);
                MirInstId const topPtr = vaFieldPtr(tagBase, vl.grTopField, voidPtrPtr);
                if (!topPtr.valid()) return InvalidMirInst;
                std::array<MirInstId, 1> loadTopOps{topPtr};
                MirInstId const grTop =
                    mir.addInst(MirOpcode::Load, loadTopOps, voidPtr);
                std::array<MirInstId, 1> sextOps{curOffs};
                MirInstId const idx64 = mir.addInst(MirOpcode::SExt, sextOps, i64);
                std::array<MirInstId, 2> slotGepOps{grTop, idx64};
                MirInstId const slotAddr =
                    mir.addInst(MirOpcode::Gep, slotGepOps, voidPtr);
                std::array<MirInstId, 1> derefOps{slotAddr};
                MirInstId const regStructPtr =
                    mir.addInst(MirOpcode::Load, derefOps, voidPtr);   // deref pointer
                MirInstId const slotK =
                    constIntOfType(static_cast<std::int64_t>(vl.gpSlotBytes), i32);
                std::array<MirInstId, 2> addOps{curOffs, slotK};
                MirInstId const bumped = mir.addInst(MirOpcode::Add, addOps, i32);
                std::array<MirInstId, 2> bumpStore{bumped, offPtr};
                mir.addInst(MirOpcode::Store, bumpStore);
                MirBlockId const regPred = mir.currentlyOpenBlock();
                mir.addBr(joinBB);

                // overflow arm: the __stack slot holds a pointer too — deref it; bump
                // __stack by ONE 8-byte slot (the slot holds a pointer, not the bytes).
                mir.beginBlock(ovfBB);
                MirInstId const stackPtr = vaFieldPtr(tagBase, vl.stackField, voidPtrPtr);
                if (!stackPtr.valid()) return InvalidMirInst;
                std::array<MirInstId, 1> loadStackOps{stackPtr};
                MirInstId const stack =
                    mir.addInst(MirOpcode::Load, loadStackOps, voidPtr);
                std::array<MirInstId, 1> ovfDerefOps{stack};
                MirInstId const ovfStructPtr =
                    mir.addInst(MirOpcode::Load, ovfDerefOps, voidPtr);   // deref pointer
                MirInstId const stackStepK =
                    constInt(static_cast<std::int64_t>(vl.gpSlotBytes));
                if (!stackStepK.valid()) return InvalidMirInst;
                std::array<MirInstId, 2> stackGepOps{stack, stackStepK};
                MirInstId const stackNext =
                    mir.addInst(MirOpcode::Gep, stackGepOps, voidPtr);
                std::array<MirInstId, 2> stackStore{stackNext, stackPtr};
                mir.addInst(MirOpcode::Store, stackStore);
                MirBlockId const ovfPred = mir.currentlyOpenBlock();
                mir.addBr(joinBB);

                // join: pick the struct ADDRESS (NO value Load — aggregate by address).
                mir.beginBlock(joinBB);
                std::array<MirPhiIncoming, 2> incomings{
                    MirPhiIncoming{regStructPtr, regPred},
                    MirPhiIncoming{ovfStructPtr, ovfPred},
                };
                return mir.addPhi(voidPtr, incomings);
            }

            // ── Paths A2/A3: InRegisters — gather N same-class pieces, atomic-fit ──
            // A2 (HFA) walks the VR block (vrOffs/vrTop, fpSlotBytes); A3 (non-HFA)
            // walks the GR block (grOffs/grTop, gpSlotBytes). Identical shape; the only
            // differences are the cursor/top fields, the slot stride, and the piece
            // count (numFp vs numGp). The fail-loud above guarantees exactly one holds.
            VaListLayout::Field const& offField =
                isHfa ? vl.vrOffsField : vl.grOffsField;
            VaListLayout::Field const& topField =
                isHfa ? vl.vrTopField : vl.grTopField;
            std::uint32_t const regSlotBytes =
                isHfa ? vl.fpSlotBytes : vl.gpSlotBytes;
            std::uint32_t const pieceCount = isHfa ? numFp : numGp;

            MirInstId const offPtr = vaFieldPtr(tagBase, offField, i32Ptr);
            if (!offPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadOffOps{offPtr};
            MirInstId const curOffs = mir.addInst(MirOpcode::Load, loadOffOps, i32);

            // ATOMIC fit (FOLD 4): the WHOLE aggregate fits in registers iff, AFTER
            // consuming all `pieceCount` slots, the cursor is STILL negative —
            // ICmpSlt(Add(curOffs, pieceCount*regSlotBytes), 0). STRICT less-than:
            // the last fitting slot has cur=-slot, and -slot+slot=0 is NOT <0, which
            // is correct (zero slots remain ⇒ go to the stack). No piece-by-piece check
            // (AAPCS64 forbids a register/stack split).
            MirInstId const fitK = constIntOfType(
                static_cast<std::int64_t>(pieceCount * regSlotBytes), i32);
            std::array<MirInstId, 2> fitAddOps{curOffs, fitK};
            MirInstId const fitSum = mir.addInst(MirOpcode::Add, fitAddOps, i32);
            std::array<MirInstId, 2> fitCmpOps{fitSum, zeroK};
            MirInstId const inReg = mir.addInst(MirOpcode::ICmpSlt, fitCmpOps, boolTy);

            MirBlockId const regBB  = mir.createBlock(StructCfMarker::IfThen);
            MirBlockId const ovfBB  = mir.createBlock(StructCfMarker::IfElse);
            MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
            mir.addCondBr(inReg, regBB, ovfBB);

            // reg arm: copy each piece from <gr|vr>_top + (curOffs + i*slot) into a
            // fresh temp at piece.byteOffset; then store the final cursor back.
            mir.beginBlock(regBB);
            MirInstId const temp = freshAggregateTemp(t);
            if (!temp.valid()) {
                unsupported(node, "va_arg of a struct/union requires a sizeable layout "
                                  "(target 'aggregateLayout' / complete type)");
                return InvalidMirInst;
            }
            MirInstId const topPtr = vaFieldPtr(tagBase, topField, voidPtrPtr);
            if (!topPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadTopOps{topPtr};
            MirInstId const top = mir.addInst(MirOpcode::Load, loadTopOps, voidPtr);
            std::uint32_t pieceIdx = 0;
            for (AbiPiece const& p : abi->pieces) {
                // localCursor = curOffs + pieceIdx*regSlotBytes (i32; NOT re-loaded).
                MirInstId const stepK = constIntOfType(
                    static_cast<std::int64_t>(pieceIdx * regSlotBytes), i32);
                std::array<MirInstId, 2> curAddOps{curOffs, stepK};
                MirInstId const localCursor =
                    mir.addInst(MirOpcode::Add, curAddOps, i32);
                // SExt (SXTW) the NEGATIVE i32 cursor before the byte Gep (hazard H1).
                std::array<MirInstId, 1> sextOps{localCursor};
                MirInstId const idx64 = mir.addInst(MirOpcode::SExt, sextOps, i64);
                std::array<MirInstId, 2> srcGepOps{top, idx64};
                MirInstId const srcAddr =
                    mir.addInst(MirOpcode::Gep, srcGepOps, voidPtr);
                // The piece WIDTH selects the load form (F32/F64 for an HFA element,
                // I64 for a GPR piece) — a wrong-width FP load would clobber the next
                // element (hazard via pieceType, reused unchanged).
                TypeId const pty = pieceType(p);
                std::array<MirInstId, 1> loadPieceOps{srcAddr};
                MirInstId const pieceVal =
                    mir.addInst(MirOpcode::Load, loadPieceOps, pty);
                // dst = temp + piece.byteOffset.
                MirInstId const offK =
                    constInt(static_cast<std::int64_t>(p.byteOffset));
                if (!offK.valid()) return InvalidMirInst;
                std::array<MirInstId, 2> dstGepOps{temp, offK};
                MirInstId const dstAddr =
                    mir.addInst(MirOpcode::Gep, dstGepOps, interner.pointer(pty));
                std::array<MirInstId, 2> stOps{pieceVal, dstAddr};
                mir.addInst(MirOpcode::Store, stOps);
                ++pieceIdx;
            }
            // Store the final cursor back: curOffs + pieceCount*regSlotBytes (i32).
            {
                std::array<MirInstId, 2> finalAddOps{curOffs, fitK};
                MirInstId const newOffs =
                    mir.addInst(MirOpcode::Add, finalAddOps, i32);
                std::array<MirInstId, 2> offStore{newOffs, offPtr};
                mir.addInst(MirOpcode::Store, offStore);
            }
            MirInstId const regAddr = temp;
            MirBlockId const regPred = mir.currentlyOpenBlock();
            mir.addBr(joinBB);

            // overflow arm: the struct sits BY VALUE at __stack; that pointer IS the
            // result. Bump __stack by roundUp(size, gpSlotBytes) (8-byte NSAA slots —
            // NOT fpSlotBytes, even for an HFA; hazard H6).
            mir.beginBlock(ovfBB);
            MirInstId const stackPtr = vaFieldPtr(tagBase, vl.stackField, voidPtrPtr);
            if (!stackPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadStackOps{stackPtr};
            MirInstId const stack = mir.addInst(MirOpcode::Load, loadStackOps, voidPtr);
            std::uint64_t const roundedSize =
                ((layout->size + stackQ - 1u) / stackQ) * stackQ;
            MirInstId const stackStepK =
                constInt(static_cast<std::int64_t>(roundedSize));
            if (!stackStepK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> stackGepOps{stack, stackStepK};
            MirInstId const stackNext =
                mir.addInst(MirOpcode::Gep, stackGepOps, voidPtr);
            std::array<MirInstId, 2> stackStore{stackNext, stackPtr};
            mir.addInst(MirOpcode::Store, stackStore);
            MirInstId const ovfAddr = stack;
            MirBlockId const ovfPred = mir.currentlyOpenBlock();
            mir.addBr(joinBB);

            // join: pick the struct ADDRESS (NO value Load — aggregate by address).
            mir.beginBlock(joinBB);
            std::array<MirPhiIncoming, 2> incomings{
                MirPhiIncoming{regAddr, regPred},
                MirPhiIncoming{ovfAddr, ovfPred},
            };
            return mir.addPhi(voidPtr, incomings);
        }
        if (vl.strategy != VaListStrategy::SysVRegisterSave) {
            unsupported(node, "internal: unknown VaListStrategy in va_arg(struct)");
            return InvalidMirInst;
        }
        TypeId const t = hir.typeId(node);
        if (!t.valid()) {
            unsupported(node, "VaArg has no result type");
            return InvalidMirInst;
        }
        auto const abi = byValueClassify(t);
        if (!abi.has_value()) {
            unsupported(node,
                "va_arg of a struct/union is not supported by this target's calling "
                "convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
            return InvalidMirInst;
        }
        StructLayout const* layout = cachedLayout(t);
        if (layout == nullptr) {
            unsupported(node, "va_arg of a struct/union requires a sizeable layout "
                              "(target 'aggregateLayout' / complete type)");
            return InvalidMirInst;
        }
        // Count the eightbyte pieces per class (1-2 total for SysV).
        std::uint32_t numGp = 0, numFp = 0;
        for (AbiPiece const& p : abi->pieces) {
            if (p.cls == AbiPieceClass::Fpr) ++numFp; else ++numGp;
        }

        auto kids = hir.children(node);
        if (kids.size() != 2) {
            unsupported(node, "malformed VaArg (expect [apExpr, TypeRef])");
            return InvalidMirInst;
        }
        MirInstId const tagBase = vaTagBase(kids[0]);   // child 1 (TypeRef) NEVER lowered
        if (!tagBase.valid()) return InvalidMirInst;

        TypeId const u32       = interner.primitive(TypeKind::U32);
        TypeId const u32Ptr    = interner.pointer(u32);
        TypeId const voidPtr   = interner.pointer(interner.primitive(TypeKind::Void));
        TypeId const voidPtrPtr = interner.pointer(voidPtr);
        TypeId const boolTy    = interner.primitive(TypeKind::Bool);

        // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a MEMORY-class (>16B)
        // aggregate is passed ENTIRELY by value in the overflow area (SysV §3.5.7) —
        // it has NO register pieces (numGp == numFp == 0), so it must NOT enter the
        // reg-vs-overflow diamond below (a zero-piece atomic fit is trivially TRUE →
        // the register arm would gather ZERO pieces → a garbage address). Dispatch
        // EARLY to the overflow-only path: the struct sits at overflow_arg_area; that
        // pointer is the result; bump overflow_arg_area by roundUp(size, gpSlotBytes)
        // (a >16B struct occupies multiple stack eightbytes). Straight-line (no
        // branch) since there is no register alternative. This is the read-side mirror
        // of the caller's appendByValueStackArg overflow placement.
        if (abi->kind == AbiPassing::Kind::ByReference) {
            MirInstId const ovfPtr =
                vaFieldPtr(tagBase, vl.overflowArgAreaField, voidPtrPtr);
            if (!ovfPtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 1> loadOvfOps{ovfPtr};
            MirInstId const ovfArea = mir.addInst(MirOpcode::Load, loadOvfOps, voidPtr);
            // Round the struct up to a whole stack slot — the CC's pointer-width arg
            // slot `vl.gpSlotBytes` (the same quantum the scalar/InRegisters overflow
            // arms bump by), read from config, never a hardcoded 8 (agnostic).
            std::uint64_t const slotQ = static_cast<std::uint64_t>(vl.gpSlotBytes);
            std::uint64_t const roundedSize =
                ((layout->size + slotQ - 1u) / slotQ) * slotQ;
            MirInstId const stepK =
                constInt(static_cast<std::int64_t>(roundedSize));
            if (!stepK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> ovfStepOps{ovfArea, stepK};
            MirInstId const ovfNext = mir.addInst(MirOpcode::Gep, ovfStepOps, voidPtr);
            std::array<MirInstId, 2> ovfStore{ovfNext, ovfPtr};
            mir.addInst(MirOpcode::Store, ovfStore);
            // The aggregate's address IS overflow_arg_area (by address — NO Load).
            return ovfArea;
        }

        // Load both class cursors up front (the atomic decision needs both).
        MirInstId const gpOffPtr = vaFieldPtr(tagBase, vl.gpOffsetField, u32Ptr);
        if (!gpOffPtr.valid()) return InvalidMirInst;
        MirInstId const fpOffPtr = vaFieldPtr(tagBase, vl.fpOffsetField, u32Ptr);
        if (!fpOffPtr.valid()) return InvalidMirInst;
        std::array<MirInstId, 1> loadGpOps{gpOffPtr};
        MirInstId const gpOff = mir.addInst(MirOpcode::Load, loadGpOps, u32);
        std::array<MirInstId, 1> loadFpOps{fpOffPtr};
        MirInstId const fpOff = mir.addInst(MirOpcode::Load, loadFpOps, u32);

        // Per-class fit thresholds, folded to constants (limits & counts are known):
        // a class fits iff cursor <= limit - num*slot. With num==0 the RHS is the
        // limit (always satisfiable since cursor<=limit by construction), so a
        // single-class aggregate's other-class check is a tautology — harmless.
        std::uint32_t const gpThresh =
            vl.gpOffsetLimit - numGp * vl.gpSlotBytes;
        std::uint32_t const fpThresh =
            vl.fpOffsetLimit - numFp * vl.fpSlotBytes;
        MirInstId const gpThreshK = constIntOfType(gpThresh, u32);
        MirInstId const fpThreshK = constIntOfType(fpThresh, u32);
        std::array<MirInstId, 2> gpCmpOps{gpOff, gpThreshK};
        MirInstId const gpFits = mir.addInst(MirOpcode::ICmpUle, gpCmpOps, boolTy);
        std::array<MirInstId, 2> fpCmpOps{fpOff, fpThreshK};
        MirInstId const fpFits = mir.addInst(MirOpcode::ICmpUle, fpCmpOps, boolTy);

        // ATOMIC decision via NESTED CondBr (both classes must fit): check gp first
        // → if ok check fp → regBB, else ovfBB. (Mirrors the canonical `&&` short-
        // circuit topology the StructCfMarker derivation recognizes; the final
        // rederive at lowering finish stamps the markers, so the creation-time
        // markers below are placeholders.) ovfBB has two CFG predecessors (the gp-
        // fail edge and the fp-fail edge); its overflow read/bump runs exactly once.
        MirBlockId const fpChkBB = mir.createBlock(StructCfMarker::IfThen);
        MirBlockId const regBB   = mir.createBlock(StructCfMarker::IfThen);
        MirBlockId const ovfBB   = mir.createBlock(StructCfMarker::IfElse);
        MirBlockId const joinBB  = mir.createBlock(StructCfMarker::IfJoin);
        mir.addCondBr(gpFits, fpChkBB, ovfBB);
        mir.beginBlock(fpChkBB);
        mir.addCondBr(fpFits, regBB, ovfBB);

        // ── register arm: gather each piece from reg_save_area + per-class cursor
        // into a fresh temp at piece.byteOffset; bump that class's cursor per piece;
        // store the final cursors back. The temp's address is this arm's result. ──
        mir.beginBlock(regBB);
        MirInstId const temp = freshAggregateTemp(t);
        if (!temp.valid()) {
            unsupported(node, "va_arg of a struct/union requires a sizeable layout "
                              "(target 'aggregateLayout' / complete type)");
            return InvalidMirInst;
        }
        MirInstId const rsaField = vaFieldPtr(tagBase, vl.regSaveAreaField, voidPtrPtr);
        if (!rsaField.valid()) return InvalidMirInst;
        std::array<MirInstId, 1> loadRsaOps{rsaField};
        MirInstId const rsa = mir.addInst(MirOpcode::Load, loadRsaOps, voidPtr);
        // Local per-class cursors, bumped as pieces are consumed (so a 2-GPR struct
        // reads consecutive save-area slots). Initialized to the loaded offsets.
        MirInstId gpCursor = gpOff;
        MirInstId fpCursor = fpOff;
        for (AbiPiece const& p : abi->pieces) {
            bool const isFp = (p.cls == AbiPieceClass::Fpr);
            MirInstId const cursor = isFp ? fpCursor : gpCursor;
            // src = reg_save_area + cursor (byte-offset Gep).
            std::array<MirInstId, 2> srcGepOps{rsa, cursor};
            MirInstId const srcAddr = mir.addInst(MirOpcode::Gep, srcGepOps, voidPtr);
            TypeId const pty = pieceType(p);
            std::array<MirInstId, 1> loadPieceOps{srcAddr};
            MirInstId const pieceVal = mir.addInst(MirOpcode::Load, loadPieceOps, pty);
            // dst = temp + piece.byteOffset.
            MirInstId const offK =
                constInt(static_cast<std::int64_t>(p.byteOffset));
            if (!offK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> dstGepOps{temp, offK};
            MirInstId const dstAddr =
                mir.addInst(MirOpcode::Gep, dstGepOps, interner.pointer(pty));
            std::array<MirInstId, 2> stOps{pieceVal, dstAddr};
            mir.addInst(MirOpcode::Store, stOps);
            // Bump this class's cursor by its slot stride.
            std::uint32_t const slot = isFp ? vl.fpSlotBytes : vl.gpSlotBytes;
            MirInstId const slotK = constIntOfType(slot, u32);
            std::array<MirInstId, 2> addOps{cursor, slotK};
            MirInstId const bumped = mir.addInst(MirOpcode::Add, addOps, u32);
            if (isFp) fpCursor = bumped; else gpCursor = bumped;
        }
        // Store the final cursors back into the tag.
        {
            std::array<MirInstId, 2> gpSt{gpCursor, gpOffPtr};
            mir.addInst(MirOpcode::Store, gpSt);
            std::array<MirInstId, 2> fpSt{fpCursor, fpOffPtr};
            mir.addInst(MirOpcode::Store, fpSt);
        }
        MirInstId const regAddr = temp;
        MirBlockId const regPred = mir.currentlyOpenBlock();
        mir.addBr(joinBB);

        // ── overflow arm: the struct sits BY VALUE at overflow_arg_area; that
        // pointer is this arm's result. Bump overflow_arg_area by roundUp(size,8)
        // (NOT one slot — a >8B struct occupies multiple stack eightbytes). ──
        mir.beginBlock(ovfBB);
        MirInstId const ovfPtr = vaFieldPtr(tagBase, vl.overflowArgAreaField, voidPtrPtr);
        if (!ovfPtr.valid()) return InvalidMirInst;
        std::array<MirInstId, 1> loadOvfOps{ovfPtr};
        MirInstId const ovfArea = mir.addInst(MirOpcode::Load, loadOvfOps, voidPtr);
        // Round the struct up to a whole stack slot. The stack-slot quantum is the
        // CC's pointer-width arg slot — `vl.gpSlotBytes` (the same field the scalar
        // overflow arm bumps by) — read from config, never a hardcoded 8 (agnostic).
        std::uint64_t const slotQ = static_cast<std::uint64_t>(vl.gpSlotBytes);
        std::uint64_t const roundedSize = ((layout->size + slotQ - 1u) / slotQ) * slotQ;
        MirInstId const stepK =
            constInt(static_cast<std::int64_t>(roundedSize));
        if (!stepK.valid()) return InvalidMirInst;
        std::array<MirInstId, 2> ovfStepOps{ovfArea, stepK};
        MirInstId const ovfNext = mir.addInst(MirOpcode::Gep, ovfStepOps, voidPtr);
        std::array<MirInstId, 2> ovfStore{ovfNext, ovfPtr};
        mir.addInst(MirOpcode::Store, ovfStore);
        MirInstId const ovfAddr = ovfArea;
        MirBlockId const ovfPred = mir.currentlyOpenBlock();
        mir.addBr(joinBB);

        // ── join: pick the struct's address (NO Load — aggregates by address). ──
        mir.beginBlock(joinBB);
        std::array<MirPhiIncoming, 2> incomings{
            MirPhiIncoming{regAddr, regPred},
            MirPhiIncoming{ovfAddr, ovfPred},
        };
        return mir.addPhi(voidPtr, incomings);
    }

    // ── Plan 24 Stage 4b — the iterative statement-lowering driver ─────────
    // A POD work-stack frame for ONE flattened control-flow statement arm.
    // `phase` advances the per-arm machine across resumes (each resume runs the
    // emission slice up to the next sub-statement request, then re-enters the
    // driver for that sub-statement). The block handles a control-flow arm mints
    // + the per-arm bookkeeping it carries across phases live in dedicated POD
    // fields (NEVER a `work.back()` reference that dangles after a sub-push) — the
    // realloc-safe rule from the expression driver: copy frame fields to locals
    // and advance `phase` BEFORE any `enterStmt`/`push_back`. `ok` carries the
    // bool a finished sub-statement delivered (the analogue of the expression
    // driver's `result` slot). For the unbounded child-statement lists (Block's
    // stmts, a switch arm's body), the iteration cursor lives in a SEPARATE LIFO
    // accumulator `blockCtxs` referenced by the stable index `aux` (the
    // `callCtxs` pattern) — a nested block grows `blockCtxs`, so a held reference
    // would dangle; the index does not.
    struct StmtFrame {
        enum class Kind : std::uint8_t {
            Block, If, While, DoWhile, For, Label, Switch
        } kind;
        HirNodeId node;
        std::uint8_t phase;
        // Block handles minted by a control-flow arm, carried across phases.
        // If    : bb0=thenBB, bb1=elseBB(invalid if no else), bb2=joinBB.
        // While : bb0=header, bb1=body, bb2=exit.
        // DoWhile: bb0=body, bb1=continueBB, bb2=exit.
        // For   : bb0=header, bb1=body, bb2=update(invalid if none), bb3=exit,
        //         bb4=backTarget.
        // Label : bb0=label block.
        // Switch: bb0=exitBB (per-arm + case blocks live in blockCtxs/exprdata).
        MirBlockId bb0{};
        MirBlockId bb1{};
        MirBlockId bb2{};
        MirBlockId bb3{};
        MirBlockId bb4{};
        // If: tracks whether any path reaches the join (the recursive
        // `joinReached`). While/For/DoWhile: unused.
        bool flag0{};
        // Block: index into `blockCtxs` (the child cursor). Unused (0) by the
        // others. (c60: the Switch frame no longer needs a per-arm cursor — its
        // body lowers as ONE Block via the work-stack — so it carries only `bb0`.)
        std::uint32_t aux{};
        // VLA C5 (D-CSUBSET-VLA): for a Block frame, `vlaScopeStack_.size()` captured
        // at the block's entry (phase 0). At the block's fall-through finish, the
        // frames [vlaBase, size) are the VLAs this block declared — restore SP to the
        // shallowest (index vlaBase) + pop them (`closeVlaBlockScope`). Unused (0) by
        // the other kinds.
        std::uint32_t vlaBase{};
    };

    // LIFO cursor for an unbounded child-statement list (a Block's stmts, or a
    // switch arm's body). Created when the arm starts iterating, popped when it
    // finishes. A nested Block/Switch pushes its OWN ctx on top, so the vector
    // grows — the owning frame re-addresses `blockCtxs[aux]` by INDEX each
    // resume (never holds a reference across a sub-statement push).
    struct BlockIterCtx {
        std::uint32_t idx{};   // next child index to lower
    };
    std::vector<BlockIterCtx> blockCtxs;

    // The public statement-lowering entry: a driver over an explicit heap
    // work-stack. For each node, `enterStmt` either PUSHES a frame for a
    // deeply-nesting control-flow arm or delegates to `lowerStmtNode` (which
    // lowers that one node; for a non-flattened LEAF arm it does NOT recurse
    // into `lowerStmt`). Returns the success/failure bool. Output-identity: the
    // flattened arms reproduce the recursive `lowerStmtNode` createBlock order,
    // branch successors, and sub-statement lowering order EXACTLY, so the
    // emitted MIR (block ids, branch targets, op order, vreg ids) is
    // byte-identical to the recursive `lowerStmt`.
    //
    // NOTE — the EXTERNAL callers (`lowerFunction`'s body, `lowerForClauseNode`,
    // the expression driver's SeqExpr arm) call this driver; each spins up its
    // OWN local work-stack, so a for-init/SeqExpr statement subtree drains fully
    // before its caller resumes — identical ordering to the recursive nesting.
    [[nodiscard]] bool lowerStmt(HirNodeId node) {
        std::vector<StmtFrame> work;
        // `enterStmt` ALWAYS assigns `ok` for a delegated node, and every pushed
        // frame delivers into `ok` before it is read (then popped), so this
        // sentinel never leaks.
        bool ok = false;

        // Classify `n`: push a frame for a flattenable control-flow arm (and
        // return), else lower it here via `lowerStmtNode` (a leaf arm — it does
        // not recurse into `lowerStmt`). A frame is pushed ONLY for the seven
        // arms whose recursion is `lowerStmt(child)`. `enterStmt` (push) MUST be
        // the LAST action of any caller path that copied out its frame fields —
        // `work.back()` may dangle after.
        auto const enterStmt = [&](HirNodeId n) {
            switch (hir.kind(n)) {
                case HirKind::Block:
                    work.push_back({.kind = StmtFrame::Kind::Block,
                                    .node = n, .phase = 0});
                    return;
                case HirKind::IfStmt:
                    work.push_back({.kind = StmtFrame::Kind::If,
                                    .node = n, .phase = 0});
                    return;
                case HirKind::WhileStmt:
                    work.push_back({.kind = StmtFrame::Kind::While,
                                    .node = n, .phase = 0});
                    return;
                case HirKind::DoWhileStmt:
                    work.push_back({.kind = StmtFrame::Kind::DoWhile,
                                    .node = n, .phase = 0});
                    return;
                case HirKind::ForStmt:
                    work.push_back({.kind = StmtFrame::Kind::For,
                                    .node = n, .phase = 0});
                    return;
                case HirKind::LabelStmt:
                    work.push_back({.kind = StmtFrame::Kind::Label,
                                    .node = n, .phase = 0});
                    return;
                case HirKind::SwitchStmt:
                    work.push_back({.kind = StmtFrame::Kind::Switch,
                                    .node = n, .phase = 0});
                    return;
                default: break;
            }
            ok = lowerStmtNode(n);   // leaf (Return/ExprStmt/VarDecl/Assign/…)
        };

        enterStmt(node);
        while (!work.empty()) {
            StmtFrame& f = work.back();
            switch (f.kind) {
            // ── Block: lower each child in order, minting a fresh dead Linear
            // block between a sealed child and its next sibling (byte-identical
            // to the recursive loop). The child cursor is in `blockCtxs[aux]`.
            case StmtFrame::Kind::Block: {
                if (f.phase == 0) {
                    f.aux = static_cast<std::uint32_t>(blockCtxs.size());
                    // VLA C5 (D-CSUBSET-VLA): watermark the VLA-scope stack at this
                    // block's entry, so its fall-through finish restores + pops
                    // exactly the VLAs it declared.
                    f.vlaBase = static_cast<std::uint32_t>(vlaScopeStack_.size());
                    blockCtxs.push_back({.idx = 0});
                    f.phase = 1;
                    // fall into phase 1 below (no sub-request yet)
                }
                // phase 1: dispatch the next child (or finish). Re-address the
                // ctx by index — a nested block grew `blockCtxs`.
                std::uint32_t const ctxIdx = f.aux;
                std::uint32_t const vlaBase = f.vlaBase;
                HirNodeId const node2 = f.node;
                if (f.phase == 2) {
                    // Resuming after a child finished. Bail on failure (pop the VLA
                    // frames this block opened; no restore — we are aborting).
                    if (!ok) {
                        closeVlaBlockScope(vlaBase, /*emitRestore=*/false);
                        blockCtxs.pop_back(); work.pop_back(); break;
                    }
                    auto kids = hir.children(node2);
                    std::uint32_t const justDone = blockCtxs[ctxIdx].idx;
                    // A child may have sealed the open block mid-block; a
                    // FOLLOWING sibling needs a fresh dead block to lower into.
                    if (justDone + 1u < kids.size()
                        && mir.openBlockHasTerminator()) {
                        MirBlockId const dead =
                            mir.createBlock(StructCfMarker::Linear);
                        mir.beginBlock(dead);
                    }
                    blockCtxs[ctxIdx].idx = justDone + 1u;
                    f.phase = 1;
                }
                // phase 1: request the child at the cursor, or finish.
                auto kids = hir.children(node2);
                std::uint32_t const i = blockCtxs[ctxIdx].idx;
                if (i >= kids.size()) {
                    // VLA C5: the block ran to completion — on a fall-through exit
                    // restore SP to the shallowest VLA it declared (before the
                    // enclosing frame's back-edge / Br), then pop those frames.
                    closeVlaBlockScope(vlaBase, /*emitRestore=*/true);
                    blockCtxs.pop_back();
                    work.pop_back();
                    ok = true;
                    break;
                }
                HirNodeId const childN = kids[i];
                f.phase = 2;
                enterStmt(childN);   // lower child — may invalidate `f`
                break;
            }
            // ── IfStmt: diamond. phase 0 lowers cond (via lowerExpr — already
            // flat), mints then/else?/join IN ORDER, CondBr, beginBlock(then),
            // requests then. phase 1 (after then): Br(join) if fell through,
            // then if else exists beginBlock(else)+request else, else finalize.
            // phase 2 (after else): Br(join) if fell through, finalize join.
            case StmtFrame::Kind::If: {
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    HirNodeId const condN = hir.ifCondition(node2);
                    auto const elseN = hir.ifElse(node2);
                    MirInstId const cond = lowerExpr(condN);
                    if (!cond.valid()) { work.pop_back(); ok = false; break; }
                    MirBlockId const thenBB =
                        mir.createBlock(StructCfMarker::IfThen);
                    MirBlockId const elseBB = elseN.has_value()
                        ? mir.createBlock(StructCfMarker::IfElse)
                        : MirBlockId{};
                    MirBlockId const joinBB =
                        mir.createBlock(StructCfMarker::IfJoin);
                    MirBlockId const falseTarget =
                        elseN.has_value() ? elseBB : joinBB;
                    mir.addCondBr(cond, thenBB, falseTarget);
                    mir.beginBlock(thenBB);
                    f.bb0 = thenBB; f.bb1 = elseBB; f.bb2 = joinBB;
                    f.flag0 = false;   // joinReached
                    f.phase = 1;
                    HirNodeId const thenN = hir.ifThen(node2);
                    enterStmt(thenN);   // lower then — may invalidate `f`
                } else if (f.phase == 1) {
                    MirBlockId const elseBB = f.bb1;
                    MirBlockId const joinBB = f.bb2;
                    if (!ok) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(elseBB);
                        sealCreatedAsUnreachable(joinBB);
                        work.pop_back(); ok = false; break;
                    }
                    if (!mir.openBlockHasTerminator()) {
                        mir.addBr(joinBB);
                        f.flag0 = true;   // joinReached
                    }
                    auto const elseN = hir.ifElse(f.node);
                    if (elseN.has_value()) {
                        mir.beginBlock(elseBB);
                        f.phase = 2;
                        enterStmt(*elseN);   // lower else — may invalidate `f`
                    } else {
                        // No else: the false edge targets join directly.
                        f.flag0 = true;   // joinReached
                        mir.beginBlock(joinBB);
                        work.pop_back(); ok = true;
                    }
                } else {  // phase 2: after else
                    MirBlockId const joinBB = f.bb2;
                    bool joinReached = f.flag0;
                    if (!ok) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(joinBB);
                        work.pop_back(); ok = false; break;
                    }
                    if (!mir.openBlockHasTerminator()) {
                        mir.addBr(joinBB);
                        joinReached = true;
                    }
                    mir.beginBlock(joinBB);
                    if (!joinReached) mir.addUnreachable();
                    work.pop_back(); ok = true;
                }
                break;
            }
            // ── WhileStmt: header CondBr(body,exit); body Br(header); exit.
            // phase 0: mint header/body/exit, Br(header), beginBlock(header),
            //          lower cond (flat), CondBr(cond,body,exit),
            //          beginBlock(body), push BranchFrame{header,exit},
            //          request body. phase 1 (after body): pop BranchFrame,
            //          Br(header) if fell through, beginBlock(exit).
            case StmtFrame::Kind::While: {
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    HirNodeId const condN = *hir.loopCondition(node2);
                    MirBlockId const header =
                        mir.createBlock(StructCfMarker::LoopHeader);
                    MirBlockId const body =
                        mir.createBlock(StructCfMarker::Linear);
                    MirBlockId const exit =
                        mir.createBlock(StructCfMarker::LoopExit);
                    mir.addBr(header);
                    mir.beginBlock(header);
                    MirInstId const cond = lowerExpr(condN);
                    if (!cond.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(body);
                        sealCreatedAsUnreachable(exit);
                        work.pop_back(); ok = false; break;
                    }
                    mir.addCondBr(cond, body, exit);
                    mir.beginBlock(body);
                    branchStack.push_back({header, exit});
                    // VLA C5 (D-CSUBSET-VLA): record the VLA depth so a break/continue
                    // targeting this loop restores SP past every VLA opened inside it.
                    branchStack.back().vlaDepthAtPush = vlaScopeStack_.size();
                    f.bb0 = header; f.bb1 = body; f.bb2 = exit;
                    f.phase = 1;
                    HirNodeId const bodyN = hir.loopBody(node2);
                    enterStmt(bodyN);   // lower body — may invalidate `f`
                } else {  // phase 1: after body
                    MirBlockId const header = f.bb0;
                    MirBlockId const exit   = f.bb2;
                    branchStack.pop_back();
                    if (!ok) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(exit);
                        work.pop_back(); ok = false; break;
                    }
                    if (!mir.openBlockHasTerminator()) mir.addBr(header);
                    mir.beginBlock(exit);
                    work.pop_back(); ok = true;
                }
                break;
            }
            // ── DoWhileStmt: body; contBB CondBr(cond,body,exit); exit.
            // phase 0: mint body/continueBB/exit, Br(body), beginBlock(body),
            //          push BranchFrame{continueBB,exit,false}, request body.
            // phase 1 (after body): read continueReferenced from the frame,
            //          pop BranchFrame, Br(continueBB) if body fell through;
            //          then if (continueReferenced || bodyFellThrough)
            //          beginBlock(continueBB)+lower cond (flat)+CondBr, else
            //          seal continueBB unreachable; beginBlock(exit).
            case StmtFrame::Kind::DoWhile: {
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    MirBlockId const body =
                        mir.createBlock(StructCfMarker::LoopHeader);
                    MirBlockId const continueBB =
                        mir.createBlock(StructCfMarker::LoopLatch);
                    MirBlockId const exit =
                        mir.createBlock(StructCfMarker::LoopExit);
                    mir.addBr(body);
                    mir.beginBlock(body);
                    branchStack.push_back({continueBB, exit, false});
                    branchStack.back().vlaDepthAtPush = vlaScopeStack_.size();  // VLA C5
                    f.bb0 = body; f.bb1 = continueBB; f.bb2 = exit;
                    f.phase = 1;
                    HirNodeId const bodyN = hir.loopBody(node2);
                    enterStmt(bodyN);   // lower body — may invalidate `f`
                } else {  // phase 1: after body
                    MirBlockId const body       = f.bb0;
                    MirBlockId const continueBB = f.bb1;
                    MirBlockId const exit       = f.bb2;
                    bool const continueReferenced =
                        branchStack.back().continueReferenced;
                    branchStack.pop_back();
                    if (!ok) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(continueBB);
                        sealCreatedAsUnreachable(exit);
                        work.pop_back(); ok = false; break;
                    }
                    bool const bodyFellThrough = !mir.openBlockHasTerminator();
                    if (bodyFellThrough) mir.addBr(continueBB);
                    if (continueReferenced || bodyFellThrough) {
                        mir.beginBlock(continueBB);
                        MirInstId const cond =
                            lowerExpr(*hir.loopCondition(f.node));
                        if (!cond.valid()) {
                            if (!mir.openBlockHasTerminator())
                                mir.addUnreachable();
                            sealCreatedAsUnreachable(exit);
                            work.pop_back(); ok = false; break;
                        }
                        mir.addCondBr(cond, body, exit);
                    } else {
                        sealCreatedAsUnreachable(continueBB);
                    }
                    mir.beginBlock(exit);
                    work.pop_back(); ok = true;
                }
                break;
            }
            // ── ForStmt: init?; header <cond?CondBr:Br>; body Br(backTarget);
            // update? Br(header); exit. The init/update CLAUSES lower via
            // `lowerForClauseNode` (which re-enters this driver for a Block/
            // VarDecl/Assign init — a bounded, shallow subtree) BEFORE/at the
            // back-edge; only the BODY is flattened onto THIS work-stack.
            // phase 0: lower init?, mint header/body/update?/exit, Br(header),
            //          beginBlock(header), lower cond? (flat) + CondBr/Br(body),
            //          beginBlock(body), push BranchFrame{backTarget,exit},
            //          request body. phase 1 (after body): pop BranchFrame,
            //          Br(backTarget) if fell through; if update beginBlock+
            //          lower update + Br(header); beginBlock(exit).
            case StmtFrame::Kind::For: {
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    auto const initN   = hir.forInit(node2);
                    auto const condN   = hir.loopCondition(node2);
                    auto const updateN = hir.forUpdate(node2);
                    // VLA C5 (D-CSUBSET-VLA): watermark the VLA stack BEFORE the init,
                    // so a for-INIT VLA is pushed at exactly `vlaBase`. The init runs
                    // ONCE at loop entry, so its VLA PERSISTS across every iteration —
                    // the For driver frees it at the loop EXIT (phase 1), NEVER on the
                    // back-edge (the body block's watermark is deeper, so its per-
                    // iteration teardown cannot reach the for-init frame).
                    f.vlaBase = static_cast<std::uint32_t>(vlaScopeStack_.size());
                    if (initN.has_value()) {
                        if (!lowerForClauseNode(*initN)) {
                            if (!mir.openBlockHasTerminator())
                                mir.addUnreachable();
                            vlaScopeStack_.resize(f.vlaBase);  // pop any for-init frame
                            work.pop_back(); ok = false; break;
                        }
                    }
                    MirBlockId const header =
                        mir.createBlock(StructCfMarker::LoopHeader);
                    MirBlockId const body =
                        mir.createBlock(StructCfMarker::Linear);
                    MirBlockId const update = updateN.has_value()
                        ? mir.createBlock(StructCfMarker::LoopLatch)
                        : MirBlockId{};
                    MirBlockId const exit =
                        mir.createBlock(StructCfMarker::LoopExit);
                    MirBlockId const backTarget =
                        updateN.has_value() ? update : header;
                    mir.addBr(header);
                    mir.beginBlock(header);
                    if (condN.has_value()) {
                        MirInstId const cond = lowerExpr(*condN);
                        if (!cond.valid()) {
                            if (!mir.openBlockHasTerminator())
                                mir.addUnreachable();
                            sealCreatedAsUnreachable(body);
                            sealCreatedAsUnreachable(update);
                            sealCreatedAsUnreachable(exit);
                            vlaScopeStack_.resize(f.vlaBase);  // pop the for-init frame
                            work.pop_back(); ok = false; break;
                        }
                        mir.addCondBr(cond, body, exit);
                    } else {
                        mir.addBr(body);  // for(;;)
                    }
                    mir.beginBlock(body);
                    branchStack.push_back({backTarget, exit});
                    branchStack.back().vlaDepthAtPush = vlaScopeStack_.size();  // VLA C5
                    f.bb0 = header; f.bb1 = body; f.bb2 = update;
                    f.bb3 = exit;   f.bb4 = backTarget;
                    f.phase = 1;
                    HirNodeId const bodyN = hir.loopBody(node2);
                    enterStmt(bodyN);   // lower body — may invalidate `f`
                } else {  // phase 1: after body
                    HirNodeId const node2     = f.node;
                    MirBlockId const header     = f.bb0;
                    MirBlockId const update     = f.bb2;
                    MirBlockId const exit       = f.bb3;
                    MirBlockId const backTarget = f.bb4;
                    std::size_t const vlaBase   = f.vlaBase;  // VLA C5: for-init watermark
                    branchStack.pop_back();
                    if (!ok) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(update);
                        sealCreatedAsUnreachable(exit);
                        vlaScopeStack_.resize(vlaBase);  // pop the for-init frame (aborting)
                        work.pop_back(); ok = false; break;
                    }
                    if (!mir.openBlockHasTerminator()) mir.addBr(backTarget);
                    auto const updateN = hir.forUpdate(node2);
                    if (updateN.has_value()) {
                        mir.beginBlock(update);
                        if (!lowerForClauseNode(*updateN)) {
                            if (!mir.openBlockHasTerminator())
                                mir.addUnreachable();
                            sealCreatedAsUnreachable(exit);
                            vlaScopeStack_.resize(vlaBase);  // pop the for-init frame
                            work.pop_back(); ok = false; break;
                        }
                        mir.addBr(header);
                    }
                    mir.beginBlock(exit);
                    // VLA C5 (D-CSUBSET-VLA): free the for-INIT VLA at the loop EXIT.
                    // The exit block is the merge of the cond-false edge AND every
                    // `break` (a break already restored the body VLAs to THIS post-init
                    // watermark, and the back-edge kept the for-init live — so both
                    // predecessors arrive with SP at the post-init level). ONE restore
                    // here reclaims the for-init on every exit path; a `goto` OUT of the
                    // loop restores it via the ancestry walk instead (it bypasses this
                    // block). No-op when the for-init declared no VLA.
                    if (vlaScopeStack_.size() > vlaBase) {
                        emitVlaRestore(vlaBase);
                        vlaScopeStack_.resize(vlaBase);
                    }
                    work.pop_back(); ok = true;
                }
                break;
            }
            // ── LabelStmt: `label: stmt`. phase 0: getOrCreate label block,
            // Br into it if the open block fell through, beginBlock(label),
            // request the labeled statement. phase 1: deliver its result.
            // (The recursive arm tail-returns `lowerStmt(labelBody)`, so the
            // labeled statement's bool IS the label's bool.)
            case StmtFrame::Kind::Label: {
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    MirBlockId const lb =
                        getOrCreateLabelBlock(hir.labelOrdinal(node2));
                    if (!mir.openBlockHasTerminator()) mir.addBr(lb);
                    mir.beginBlock(lb);
                    f.phase = 1;
                    HirNodeId const bodyN = hir.labelBody(node2);
                    enterStmt(bodyN);   // lower labeled stmt — may invalidate `f`
                } else {  // phase 1: deliver labeled-stmt result
                    // `ok` already holds the labeled statement's result.
                    work.pop_back();
                }
                break;
            }
            // ── SwitchStmt (c60, Design I-A): the dispatch arms map each case
            // value (+ default) to the ordinal of a synthetic LabelStmt marker in
            // the FLAT body Block. phase 0 lowers the discriminant, getOrCreates the
            // case markers' label-blocks (the SAME machinery goto/label use), emits
            // addSwitch targeting them, pushes the break-frame, opens a fresh
            // pre-case block, and requests the body Block; phase 1 wires the body's
            // fall-off-the-end to the join. `bb0` = the join/exit block. NO per-arm
            // blocks — fall-through is straight-line inside the body.
            case StmtFrame::Kind::Switch: {
                if (f.phase == 0) {
                    HirNodeId const node2 = f.node;
                    HirNodeId const discN = hir.switchDiscriminant(node2);
                    auto       const arms  = hir.switchArms(node2);
                    MirInstId const disc = lowerExpr(discN);
                    if (!disc.valid()) { work.pop_back(); ok = false; break; }
                    MirBlockId const exitBB =
                        mir.createBlock(StructCfMarker::SwitchJoin);
                    // Build (caseValue, target-label-block) + resolve defaultBB,
                    // targeting the case markers' label-blocks (lazy/forward via
                    // getOrCreateLabelBlock — the body's LabelStmt markers fill them).
                    std::vector<std::pair<MirInstId, MirBlockId>> cases;
                    cases.reserve(arms.size());
                    MirBlockId defaultBB{};
                    bool failed = false;
                    for (HirNodeId const arm : arms) {
                        MirBlockId const target =
                            getOrCreateLabelBlock(hir.caseArmLabelOrdinal(arm));
                        if (hir.caseArmIsDefault(arm)) {
                            if (defaultBB.valid()) {
                                unsupported(arm, "switch has more than one "
                                                  "default arm (HIR verifier "
                                                  "should have flagged this)");
                                failed = true; break;
                            }
                            defaultBB = target;
                            continue;
                        }
                        auto const valN = hir.caseArmValue(arm);
                        if (!valN.has_value()) {
                            unsupported(arm, "non-default CaseArm without "
                                              "match value (HIR verifier "
                                              "should have flagged this)");
                            failed = true; break;
                        }
                        MirInstId const caseVal = lowerExpr(*valN);
                        if (!caseVal.valid()) { failed = true; break; }
                        cases.emplace_back(caseVal, target);
                    }
                    if (failed) {
                        sealCreatedAsUnreachable(exitBB);
                        work.pop_back(); ok = false; break;
                    }
                    if (!defaultBB.valid()) defaultBB = exitBB;
                    mir.addSwitch(disc, cases, defaultBB);
                    // Push the break-frame ONCE for the whole switch.
                    branchStack.push_back({MirBlockId{}, exitBB});
                    branchStack.back().vlaDepthAtPush = vlaScopeStack_.size();  // VLA C5
                    // Only a non-label-first body (a jumped-over leading decl) needs a
                    // fresh predecessor-less block (pruned as unreachable); a case
                    // marker opens its own label-block (see the recursive form).
                    if (!switchBodyStartsWithLabel(hir.switchBody(node2))) {
                        MirBlockId const preCase =
                            mir.createBlock(StructCfMarker::Linear);
                        mir.beginBlock(preCase);
                    }
                    f.bb0 = exitBB;
                    f.phase = 1;
                    enterStmt(hir.switchBody(node2));  // lower body — may invalidate `f`
                    break;
                }
                // phase 1: the body Block finished (`ok`). Wire fall-off-the-end to
                // the join, pop the break-frame, and continue emitting at the join.
                if (!ok) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(f.bb0);
                    branchStack.pop_back();
                    work.pop_back(); ok = false; break;
                }
                if (!mir.openBlockHasTerminator()) mir.addBr(f.bb0);
                branchStack.pop_back();
                mir.beginBlock(f.bb0);
                work.pop_back(); ok = true;
                break;
            }
            }
        }
        return ok;
    }

    // Lower ONE HIR statement node in the currently-open MIR block, given that
    // its child SUB-STATEMENTS are lowered by RE-ENTERING `lowerStmt` (the
    // driver below). Returns true on success, false on a hard error (caller
    // bails).
    //
    // Plan 24 Stage 4b (D-PARSE-DEEP-NEST-RECURSION-MEMORY): the public entry
    // `lowerStmt` is now an explicit heap work-stack DRIVER. The deeply-nesting
    // control-flow arms — Block (its child list), IfStmt (then/else), While/
    // DoWhile/For (body), LabelStmt (labeled stmt), SwitchStmt (each arm's body
    // list) — are flattened onto that work-stack so a deeply-nested `{{{…}}}` /
    // `if(if(if(…)))` / `while(while(…))` / `label: label: …` nest carries FLAT
    // O(1) host-stack cost per nesting level. This per-NODE handler is the
    // byte-identical emission body for EVERY arm: the LEAF arms (Return,
    // Unreachable, ExprStmt, VarDecl, AssignStmt, Break, Continue, Goto,
    // IndirectGoto) are reached through the driver UNCHANGED, and the flattened
    // control-flow arms here are retained as the dead-via-driver recursive
    // fallback (the driver's `StmtFrame` machine reproduces their createBlock
    // order, branch successors, and sub-statement lowering order BYTE-FOR-BYTE;
    // the EXPRESSION lowering inside any statement — conditions, rhs, discrim —
    // still flattens via `lowerExpr`/`runExprDriver`, called exactly as today).
    bool lowerStmtNode(HirNodeId node) {
        HirKind const k = hir.kind(node);
        switch (k) {
            case HirKind::Block: {
                // NOTE (VLA C5, D-CSUBSET-VLA): this recursive Block arm is DEAD via
                // the StmtFrame driver (enterStmt intercepts Block); the LIVE block-
                // scope VLA teardown lives in StmtFrame::Kind::Block. The same
                // `closeVlaBlockScope` helper is mirrored here so a future
                // reactivation of this fallback cannot silently leak the dynamic stack.
                std::size_t const vlaBase = vlaScopeStack_.size();
                auto kids = hir.children(node);
                for (std::size_t i = 0; i < kids.size(); ++i) {
                    if (!lowerStmt(kids[i])) {
                        closeVlaBlockScope(vlaBase, /*emitRestore=*/false);
                        return false;
                    }
                    // A child may unconditionally transfer control and seal the
                    // open block mid-block (the `Block{ infinite-loop, Unreachable }`
                    // wrapper cst_to_hir synthesizes for a provably-infinite loop —
                    // D-HIR-INFINITE-LOOP-NOT-TERMINATING — seals its exit with an
                    // `Unreachable`). Any FOLLOWING sibling is dynamically
                    // unreachable, but the HIR dead-code rule deliberately permits
                    // it after such a wrapper (a `Block` is not an unconditional
                    // terminator), so it still reaches MIR and must lower somewhere.
                    // Open a fresh dead block for the remainder; `addInst` aborts on
                    // a sealed/absent open block otherwise. The dead block has no
                    // predecessor and is removed by the mandatory MIR
                    // unreachable-prune (D-MIR-UNREACHABLE-PRUNE-NORMALIZE).
                    if (i + 1 < kids.size() && mir.openBlockHasTerminator()) {
                        MirBlockId const dead = mir.createBlock(StructCfMarker::Linear);
                        mir.beginBlock(dead);
                    }
                }
                closeVlaBlockScope(vlaBase, /*emitRestore=*/true);
                return true;
            }
            case HirKind::ReturnStmt: {
                auto v = hir.returnValue(node);
                if (!v.has_value()) {
                    mir.addReturn();
                    return true;
                }
                // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value
                // struct/union return is lowered specially (sret copy-through or
                // multi-piece Return) — never as a single truncating value.
                // D-CSUBSET-BITINT-C2-WIDE: a wide-`_BitInt` return is likewise a
                // multi-limb by-value return (isByValueClass → lowerStructReturn copies
                // the limbs through the sret / 2-GPR pieces).
                if (currentFnResult_.valid()
                    && isByValueClass(interner, currentFnResult_))
                    return lowerStructReturn(*v);
                MirInstId const value = lowerExpr(*v);
                if (!value.valid()) return false;
                mir.addReturn(value);
                return true;
            }
            case HirKind::Unreachable: {
                // A statement-position `Unreachable` (cst_to_hir synthesizes one
                // after a provably-infinite loop — D-HIR-INFINITE-LOOP-NOT-
                // TERMINATING — wrapping it as `Block{ loop, Unreachable }` so the
                // HIR verifier's structural-termination check matches the dynamic
                // truth). It lowers to a MIR `Unreachable` terminator on the open
                // block. Control provably never arrives here, so the block (and
                // this terminator) is dropped by the MIR unreachable-prune
                // (D-MIR-UNREACHABLE-PRUNE-NORMALIZE) — runtime is unaffected.
                // Guard on `openBlockHasTerminator` so a preceding terminator in
                // the same block (e.g. the loop already sealed its exit) is not
                // double-sealed.
                if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                return true;
            }
            case HirKind::ExprStmt: {
                // Discard the value; emit for side effects. The aggregate-discard
                // chokepoint is shared with the for-clause site — see
                // lowerDiscardedExpr (one funnel, no per-kind/per-position miss).
                return lowerDiscardedExpr(hir.exprStmtExpr(node));
            }
            case HirKind::VarDecl: {
                // Allocate the local's slot on the current block. The declared
                // type drives the alloca's pointer result type via the lattice.
                // If the var has an initializer, evaluate it and store into
                // the slot. Body-locals are slot-backed: reads emit
                // `Load(alloca)`, writes emit `Store(value, alloca)`.
                TypeId   const ty  = hir.varDeclType(node);
                SymbolId const sym = hir.varDeclSymbol(node);
                if (!ty.valid() || !sym.valid()) {
                    unsupported(node, "VarDecl with invalid type/symbol "
                                       "(HIR verifier should have flagged)");
                    return false;
                }
                // c69 (D-MIR-ENTRY-BLOCK-ALLOCA-HOIST): the slot was pre-emitted
                // into the ENTRY block by lowerFunction's hoist pre-pass (so a
                // decl in an entry-unreachable block can't strand its slot). Reuse
                // it; the pre-pass skips only un-sizeable aggregate/array locals,
                // for which we emit here (the old path, which then fail-louds).
                MirInstId alloca;
                if (auto it = addressableLocal.find(sym.v);
                    it != addressableLocal.end()) {
                    alloca = it->second;
                } else {
                    // VLA C5 (D-CSUBSET-VLA): a dynamic-stack VLA local (`int a[n]`,
                    // a multi-dim VLA, or a VLA-typedef object) descends SP via
                    // `sub sp`. Open a block-scope teardown watermark FIRST — capture
                    // SP (StackSave) just before that `sub sp` and push a
                    // vlaScopeStack_ frame — so every non-return exit of the enclosing
                    // block restores it (the fixed stride-slot allocas emitted inside
                    // allocaForLocal do NOT move SP). This is the SAME predicate
                    // allocaForLocal uses to route the dynamic alloca; a ptr-to-VLA
                    // (a fixed 8-byte slot, no `sub sp`) is deliberately NOT matched.
                    // A non-VLA local emits NEITHER op (byte-clean).
                    if (interner.isVlaArray(ty) || interner.typeContainsVla(ty)) {
                        if (!emitStackSaveForVla(node)) return false;
                    }
                    alloca = allocaForLocal(sym, ty, node);
                    if (!alloca.valid()) return false;
                }
                // VLA C4a-local (D-CSUBSET-VLA): a LOCAL pointer-to-VLA (`int (*p)[n]`)
                // freezes its runtime POINTEE row stride HERE, at the decl point in
                // program order — CRITICAL-2: the pointer's own 8-byte alloca is HOISTED
                // to entry, where `n` is not yet stored, so freezing the stride there
                // would read garbage. `scaleIndexToBytes` recovers the `p[i]` stride from
                // the `(p, pointeeTy)` slot exactly as a declared VLA row. A ptr to a
                // FIXED array (`int (*p)[5]`) has a non-VLA pointee → skipped (compile-
                // time stride). Frozen BEFORE the initializer lowers: the pointee's size
                // is part of `p`'s type, evaluated when the declaration is reached.
                if (interner.kind(ty) == TypeKind::Ptr) {
                    auto const pops = interner.operands(ty);
                    if (!pops.empty() && interner.typeContainsVla(pops[0]))
                        if (!storePtrToVlaStride(sym, pops[0], node)) return false;
                }
                if (auto initN = hir.varDeclInit(node); initN.has_value()) {
                    // FC7 (D-FC7-MEMBER-ACCESS): a struct/union initializer
                    // (`P p = {3,4}` / `{.y=7}`) lowers ELEMENT-WISE — one
                    // Gep+Store per field into the slot — never as an
                    // aggregate-SSA Store (no LIR aggregate-width Store).
                    TypeKind const initKind = interner.kind(ty);
                    // c27 (D-CSUBSET-VOLATILE-POINTEE): a `volatile`-qualified
                    // aggregate's brace-init (`volatile struct S s = {…}`) writes
                    // every field as a volatile access (C 6.7.3p5). The dest's
                    // volatility = its declared type's VolatileQual (c27) OR the
                    // VarDecl object annotation (c21) — flag every init Store.
                    MirInstFlags const initVf =
                        volatileFlagFor(node) | volatileFlagForType(ty);
                    if (hir.kind(*initN) == HirKind::ConstructAggregate
                        && (initKind == TypeKind::Struct
                            || initKind == TypeKind::Union)) {
                        if (!lowerAggregateInitIntoSlot(*initN, alloca, ty, initVf))
                            return false;
                    } else if (hir.kind(*initN) == HirKind::ConstructAggregate
                               && initKind == TypeKind::Array) {
                        // D-MIR-ARRAY-FIELD-AGGREGATE-INIT (array-LOCAL form):
                        // `int a[3] = {1,2,3}` — element-wise into the slot via
                        // the same helper the array-field recurse-guard uses.
                        if (!lowerArrayInitIntoSlot(*initN, alloca, ty, initVf))
                            return false;
                    } else if (initKind == TypeKind::Array
                               && hir.kind(*initN) == HirKind::Literal) {
                        // c62 (C 6.7.9p14, D-CSUBSET-STRING-LITERAL-ARRAY-ZERO-FILL):
                        // `char x[7] = "hi";` — a STRING LITERAL initializing a CHAR
                        // ARRAY local. The HIR coerce arm retyped the literal to the
                        // slot's `char[N]`, so its lvalue address materializes the
                        // rodata global SIZED at N (string bytes + NUL, zero-padded to
                        // N by the asm producer); copy those N bytes into the stack
                        // slot. This is the array-COPY twin of the `int a[3]={…}`
                        // element-wise arm above and the struct-field string arm —
                        // the global is N bytes so the N-byte copy never reads OOB
                        // (the Option-A pad-at-the-producer invariant).
                        MirInstId const srcPtr = lowerLvalueAddress(*initN);
                        if (!srcPtr.valid()) return false;
                        if (!lowerAggregateCopy(*initN, srcPtr, alloca, ty, initVf))
                            return false;
                    } else if (isByValueClass(interner, ty)) {
                        // FC7 (D-FC7-MEMBER-ACCESS): struct/union COPY-init
                        // from another aggregate VALUE (`T a = b;`) — copy
                        // field-wise from the source lvalue's address. An
                        // aggregate-width Load+Store would truncate to one
                        // register. D-CSUBSET-BITINT-C2-WIDE: a wide `_BitInt`
                        // init (`_BitInt(200) a = b;` / `= b+c;` / `= (int)x;`)
                        // is memory-resident — copy from the (existing or freshly
                        // materialized) source ADDRESS, byte-wise; a scalar Store
                        // of the (pointer-width) rvalue would truncate to 8 bytes.
                        MirInstId const srcPtr = lowerLvalueAddress(*initN);
                        if (!srcPtr.valid()) return false;
                        // c21/c27: a WHOLE-VOLATILE aggregate copy flags every
                        // structural Load/Store. Either side being volatile makes
                        // the copy volatile: c21 via the object-volatile dest local
                        // `node` / source `*initN`; c27 via the source's ACCESSED
                        // TYPE being top-level VolatileQual (a `volatile struct`
                        // value, or a deref of a `volatile struct *`). OR all so no
                        // side's volatility is dropped (safe-conservative).
                        MirInstFlags const aggVf =
                            volatileFlagFor(node) | volatileFlagFor(*initN)
                            | volatileFlagForType(ty)
                            | volatileFlagForType(hir.typeId(*initN));
                        if (!lowerAggregateCopy(*initN, srcPtr, alloca, ty, aggVf))
                            return false;
                    } else {
                        MirInstId const initVal = lowerExpr(*initN);
                        if (!initVal.valid()) return false;
                        std::array<MirInstId, 2> ops{initVal, alloca};
                        // c21/c27: the init store into a `volatile` local's slot
                        // carries the flag — via the VarDecl object annotation (c21)
                        // OR the declared type's VolatileQual (c27, e.g. a `vint x`
                        // typedef = `volatile int`). OR both so neither is missed.
                        // FC17.9(d): an `_Atomic` local's scalar init becomes
                        // AtomicStore (harmless-if-stronger on a fresh, unshared
                        // slot; keeps the scalar-store funnel uniform — the DISTINCT
                        // param/global runtime-init paths stay plain + belt-exempt).
                        emitScalarStore(ops, ty, node);
                    }
                }
                return true;
            }
            case HirKind::AssignStmt: {
                // Lower the rhs first (its evaluation order is HIR-fixed), then
                // resolve the lhs's storage and emit `Store(rhs, ptr)`. Two
                // lvalue shapes lower here:
                //   - `Ref(sym)` where sym is an addressable local → store
                //     into its alloca.
                //   - `Deref(ptr)` → store into the lowered pointer.
                HirNodeId const targetN = hir.assignTarget(node);
                HirNodeId const valueN  = hir.assignValue(node);
                // FC7 (D-FC7-MEMBER-ACCESS): a struct/union COPY assignment
                // (`a = b`, `*pa = *pb`) copies field-wise from the source
                // lvalue to the target lvalue — an aggregate-width Load+Store
                // would TRUNCATE a wider-than-register struct to its low
                // bytes (a silent miscompile). D-CSUBSET-BITINT-C2-WIDE: a wide
                // `_BitInt` assignment (`a = b`, `a = b+c`) is the multi-limb
                // memory-copy twin — reach both sides by address, byte-wise copy.
                TypeId const valTy = hir.typeId(valueN);
                if (isByValueClass(interner, valTy)) {
                    MirInstId const dstPtr = lowerLvalueAddress(targetN);
                    if (!dstPtr.valid()) return false;
                    MirInstId const srcPtr = lowerLvalueAddress(valueN);
                    if (!srcPtr.valid()) return false;
                    // c21/c27: a WHOLE-VOLATILE aggregate assignment flags every
                    // structural Load/Store — either the target or source being
                    // volatile makes the copy volatile. c21 via object-volatile
                    // target/source; c27 via either side's ACCESSED TYPE being
                    // top-level VolatileQual (a `volatile struct` lvalue, or a deref
                    // of a `volatile struct *`). OR all so no side is dropped.
                    MirInstFlags const aggVf =
                        volatileFlagFor(targetN) | volatileFlagFor(valueN)
                        | volatileFlagForType(hir.typeId(targetN))
                        | volatileFlagForType(hir.typeId(valueN));
                    return lowerAggregateCopy(node, srcPtr, dstPtr, valTy, aggVf);
                }
                MirInstId const rhs = lowerExpr(valueN);
                if (!rhs.valid()) return false;
                MirInstId const ptr = lowerLvalueAddress(targetN);
                if (!ptr.valid()) return false;
                // c21/c27: the store's volatility is the TARGET lvalue's. c21:
                // `targetN` is a Ref (object-volatile) or MemberAccess
                // (object-volatile member), annotated at CST→HIR. c27: the target's
                // ACCESSED TYPE is volatile — a Deref target `*p = x` where `p` is
                // `volatile int *` (targetN's type = the pointee `VolatileQual(int)`),
                // an Index `va[i] = x` into a volatile element, or a member whose
                // field type is top-level VolatileQual. OR the type-derived flag so
                // a volatile-pointee STORE is never dropped (the c21 comment's
                // "Deref carries no flag" no longer holds — pointees compile now).
                MirInstFlags const vf =
                    volatileFlagFor(targetN) | volatileFlagForType(hir.typeId(targetN));
                // FC8 D-CSUBSET-BITFIELD: a bit-field write is a READ-MODIFY-WRITE
                // of the allocation unit (the Gep targets the unit) — clear the
                // field's bits, OR in the new value, store back. A plain Store
                // would overwrite the neighbour bits packed in the same unit.
                if (BitFieldPlacement const* bf = bitfieldPlacementOf(targetN)) {
                    return emitBitfieldInsert(ptr, rhs, *bf, hir.typeId(targetN), vf);
                }
                // FC17.9(d): an `_Atomic` scalar assignment becomes AtomicStore via
                // the scalar-access chokepoint (else the exact c21|c27 flag `vf`).
                std::array<MirInstId, 2> ops{rhs, ptr};
                emitScalarStore(ops, hir.typeId(targetN), targetN);
                return true;
            }
            case HirKind::IfStmt: {
                // Diamond: entry → CondBr(cond, then, else?), then → Br(join),
                // else → Br(join), join is the continuation. If a branch
                // returns or otherwise seals, we skip its Br(join). If BOTH
                // branches seal, no join is needed — the if is a terminator-
                // shaped statement (e.g. `if (x) return a; else return b;`).
                HirNodeId const condN = hir.ifCondition(node);
                HirNodeId const thenN = hir.ifThen(node);
                auto const elseN = hir.ifElse(node);

                MirInstId const cond = lowerExpr(condN);
                if (!cond.valid()) return false;

                MirBlockId const thenBB = mir.createBlock(StructCfMarker::IfThen);
                MirBlockId const elseBB = elseN.has_value()
                    ? mir.createBlock(StructCfMarker::IfElse)
                    : MirBlockId{};
                // Join block is created lazily — only if at least one branch
                // doesn't seal itself.
                MirBlockId const joinBB = mir.createBlock(StructCfMarker::IfJoin);
                MirBlockId const falseTarget = elseN.has_value() ? elseBB : joinBB;
                mir.addCondBr(cond, thenBB, falseTarget);

                bool joinReached = false;

                mir.beginBlock(thenBB);
                if (!lowerStmt(thenN)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(elseBB);
                    sealCreatedAsUnreachable(joinBB);
                    return false;
                }
                if (!mir.openBlockHasTerminator()) {
                    mir.addBr(joinBB);
                    joinReached = true;
                }

                if (elseN.has_value()) {
                    mir.beginBlock(elseBB);
                    if (!lowerStmt(*elseN)) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(joinBB);
                        return false;
                    }
                    if (!mir.openBlockHasTerminator()) {
                        mir.addBr(joinBB);
                        joinReached = true;
                    }
                } else {
                    // No else: the false edge of CondBr targets joinBB
                    // directly, which counts as "reaching" join.
                    joinReached = true;
                }

                // Open the join block iff at least one path needs it. If
                // neither path falls through (both returned/unreachable),
                // the join block is unreferenced — seal it with Unreachable
                // so this lowering's finish() doesn't abort on a created-
                // but-unfilled block. The block is then UNREACHABLE-from-
                // entry: the mandatory post-lowering prune
                // (D-MIR-UNREACHABLE-PRUNE-NORMALIZE, runPruneUnreachableBlocks
                // in optimizeModule) drops it centrally before any verifier
                // sees the module — this seal exists only to satisfy the
                // local finish() invariant.
                mir.beginBlock(joinBB);
                if (!joinReached) {
                    mir.addUnreachable();
                }
                return true;
            }
            case HirKind::SehTryExcept: {
                // c115 SEH (D-WIN64-SEH-FUNCLETS) — the region skeleton:
                //   pre:      SehTryBegin(id), succs [tryBB, filterBB]
                //   tryBB:    guarded body …; SehTryEnd(id); Br(joinBB)
                //             (the SINGLE exit — option (C), verifier-enforced
                //             D-CSUBSET-SEH-EARLY-EXIT; a body sealing itself
                //             (infinite loop) simply has no End marker)
                //   filterBB: filter expr …; SehFilterReturn(i32, id) → handlerBB
                //   handlerBB: handler body …; Br(joinBB)
                //   joinBB:   the continuation.
                // All four blocks stamp Linear — the canonical marker derive
                // (rederiveStructCfMarkers, which OVERWRITES creation stamps
                // after finish()) classifies them from CFG shape; its if/switch
                // rules key on CondBr/Switch only, and the verifier's single-
                // pred rules keep back-edge/loop-exit claims off filter/handler.
                // Locals stay memory-true: mem2reg skips SEH-containing
                // functions (fault-time state must be observable), and
                // SehTryBegin/End/FilterReturn are opcodeClobbersMemory members
                // so CSE/LICM never move Load/Store across region boundaries.
                HirNodeId const tryN     = hir.sehTryBody(node);
                HirNodeId const filterN  = hir.sehTryFilter(node);
                HirNodeId const handlerN = hir.sehTryHandler(node);

                std::uint32_t const regionId = sehRegionCounter_++;
                MirBlockId const tryBB     = mir.createBlock(StructCfMarker::Linear);
                MirBlockId const filterBB  = mir.createBlock(StructCfMarker::Linear);
                MirBlockId const handlerBB = mir.createBlock(StructCfMarker::Linear);
                MirBlockId const joinBB    = mir.createBlock(StructCfMarker::Linear);
                mir.addSehTryBegin(tryBB, filterBB, regionId);

                bool joinReached = false;

                // ── the guarded body ──
                mir.beginBlock(tryBB);
                if (!lowerStmt(tryN)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(filterBB);
                    sealCreatedAsUnreachable(handlerBB);
                    sealCreatedAsUnreachable(joinBB);
                    return false;
                }
                if (!mir.openBlockHasTerminator()) {
                    mir.addInst(MirOpcode::SehTryEnd, {}, InvalidType, regionId);
                    mir.addBr(joinBB);
                    joinReached = true;
                }

                // ── the filter expression (i32-coerced: MSVC's filter contract;
                //    a Bool comparison like `_exception_code()==E` ZExts) ──
                mir.beginBlock(filterBB);
                MirInstId fval = lowerExpr(filterN);
                if (!fval.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(handlerBB);
                    sealCreatedAsUnreachable(joinBB);
                    return false;
                }
                TypeId const i32Ty = interner.primitive(TypeKind::I32);
                TypeId const ft    = hir.typeId(filterN);
                if (ft.valid() && ft != i32Ty) {
                    MirOpcode const castOp = mapCast(interner.kind(ft), TypeKind::I32);
                    if (castOp == MirOpcode::Invalid) {
                        unsupported(node, "SEH __except filter expression must be "
                                          "an integer (MSVC: the filter value is "
                                          "an int)");
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(handlerBB);
                        sealCreatedAsUnreachable(joinBB);
                        return false;
                    }
                    std::array<MirInstId, 1> const castOps{fval};
                    fval = mir.addInst(castOp, castOps, i32Ty);
                }
                mir.addSehFilterReturn(fval, handlerBB, regionId);

                // ── the handler body ──
                mir.beginBlock(handlerBB);
                if (!lowerStmt(handlerN)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(joinBB);
                    return false;
                }
                if (!mir.openBlockHasTerminator()) {
                    mir.addBr(joinBB);
                    joinReached = true;
                }

                mir.beginBlock(joinBB);
                if (!joinReached) {
                    mir.addUnreachable();   // both paths sealed — pruned centrally
                }
                return true;
            }
            case HirKind::WhileStmt: {
                // header: CondBr(cond, body, exit)
                // body:   …; Br(header)
                // exit:   continuation
                // continue → header; break → exit.
                HirNodeId const condN = *hir.loopCondition(node);
                HirNodeId const bodyN = hir.loopBody(node);

                MirBlockId const header = mir.createBlock(StructCfMarker::LoopHeader);
                MirBlockId const body   = mir.createBlock(StructCfMarker::Linear);
                MirBlockId const exit   = mir.createBlock(StructCfMarker::LoopExit);

                mir.addBr(header);

                mir.beginBlock(header);
                MirInstId const cond = lowerExpr(condN);
                if (!cond.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(body);
                    sealCreatedAsUnreachable(exit);
                    return false;
                }
                mir.addCondBr(cond, body, exit);

                mir.beginBlock(body);
                branchStack.push_back({header, exit});
                bool const bodyOk = lowerStmt(bodyN);
                branchStack.pop_back();
                if (!bodyOk) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(exit);
                    return false;
                }
                if (!mir.openBlockHasTerminator()) mir.addBr(header);

                mir.beginBlock(exit);
                return true;
            }
            case HirKind::DoWhileStmt: {
                // body:   …; (fall-through?) Br(continueBB)
                // contBB: CondBr(cond, body, exit)
                // exit:   continuation
                // continue → continueBB (runs the cond test), break → exit.
                // `continueBB` is created ONLY when something might branch
                // to it: either the body falls through, or a `continue;`
                // inside the body resolves to this loop's frame. If the
                // body self-seals AND no continue references the frame,
                // we elide continueBB entirely — that prevents lowering
                // the dead cond expression (which would otherwise surface
                // spurious unsupported-construct diagnostics) and avoids
                // unreachable MIR bloat.
                HirNodeId const bodyN = hir.loopBody(node);
                HirNodeId const condN = *hir.loopCondition(node);

                MirBlockId const body       = mir.createBlock(StructCfMarker::LoopHeader);
                MirBlockId const continueBB = mir.createBlock(StructCfMarker::LoopLatch);
                MirBlockId const exit       = mir.createBlock(StructCfMarker::LoopExit);

                mir.addBr(body);

                mir.beginBlock(body);
                branchStack.push_back({continueBB, exit, false});
                bool const bodyOk = lowerStmt(bodyN);
                bool const continueReferenced =
                    branchStack.back().continueReferenced;
                branchStack.pop_back();
                if (!bodyOk) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(continueBB);
                    sealCreatedAsUnreachable(exit);
                    return false;
                }
                bool const bodyFellThrough = !mir.openBlockHasTerminator();
                if (bodyFellThrough) mir.addBr(continueBB);

                if (continueReferenced || bodyFellThrough) {
                    mir.beginBlock(continueBB);
                    MirInstId const cond = lowerExpr(condN);
                    if (!cond.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(exit);
                        return false;
                    }
                    mir.addCondBr(cond, body, exit);
                } else {
                    // No predecessor → seal as unreachable; cond is dead.
                    // This NORMAL-path orphan (body self-sealed AND no
                    // `continue` referenced the frame) is dropped centrally
                    // by the mandatory post-lowering prune
                    // (D-MIR-UNREACHABLE-PRUNE-NORMALIZE); the seal only
                    // satisfies this lowering's finish() invariant.
                    sealCreatedAsUnreachable(continueBB);
                }

                mir.beginBlock(exit);
                return true;
            }
            case HirKind::ForStmt: {
                // C-style for: shape is `init?; cond?; update?; body`. Lower as
                // a while-with-init-prefix-and-update-suffix-on-back-edge:
                //   entry: <init?>; Br(header)
                //   header: <cond? CondBr(body, exit) : Br(body)>
                //   body:   <body>; (if not sealed) Br(update_or_header)
                //   update: <update>; Br(header)   -- created only when update present
                //   exit:   continuation
                // Update lives on the continue-target so `continue` runs the
                // step before re-testing the condition (matches C semantics).
                auto const initN   = hir.forInit(node);
                auto const condN   = hir.loopCondition(node);  // optional
                auto const updateN = hir.forUpdate(node);
                HirNodeId const bodyN = hir.loopBody(node);

                if (initN.has_value()) {
                    if (!lowerForClauseNode(*initN)) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        return false;
                    }
                }

                MirBlockId const header = mir.createBlock(StructCfMarker::LoopHeader);
                MirBlockId const body   = mir.createBlock(StructCfMarker::Linear);
                MirBlockId const update = updateN.has_value()
                    ? mir.createBlock(StructCfMarker::LoopLatch)
                    : MirBlockId{};
                MirBlockId const exit   = mir.createBlock(StructCfMarker::LoopExit);
                MirBlockId const backTarget = updateN.has_value() ? update : header;

                mir.addBr(header);

                mir.beginBlock(header);
                if (condN.has_value()) {
                    MirInstId const cond = lowerExpr(*condN);
                    if (!cond.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(body);
                        sealCreatedAsUnreachable(update);
                        sealCreatedAsUnreachable(exit);
                        return false;
                    }
                    mir.addCondBr(cond, body, exit);
                } else {
                    mir.addBr(body);  // for(;;) — infinite loop
                }

                mir.beginBlock(body);
                // continue → update (or header if no update); break → exit.
                branchStack.push_back({backTarget, exit});
                bool const bodyOk = lowerStmt(bodyN);
                branchStack.pop_back();
                if (!bodyOk) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(update);
                    sealCreatedAsUnreachable(exit);
                    return false;
                }
                if (!mir.openBlockHasTerminator()) mir.addBr(backTarget);

                if (updateN.has_value()) {
                    mir.beginBlock(update);
                    if (!lowerForClauseNode(*updateN)) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        sealCreatedAsUnreachable(exit);
                        return false;
                    }
                    mir.addBr(header);
                }

                mir.beginBlock(exit);
                return true;
            }
            case HirKind::BreakStmt: {
                // HIR's `branchDepth` is a de Bruijn index into the
                // innermost-first stack of enclosing loop/switch frames.
                // `branchStack` mirrors that stack (innermost on the back),
                // so depth-N break targets `branchStack[size-1-N].breakBB`.
                std::uint32_t const depth = hir.branchDepth(node);
                if (depth >= branchStack.size()) {
                    unsupported(node, std::format(
                        "BreakStmt depth {} exceeds enclosing-frame count {} "
                        "(HIR verifier should have flagged this)",
                        depth, branchStack.size()));
                    return false;
                }
                BranchFrame const& targetFrame =
                    branchStack[branchStack.size() - 1 - depth];
                MirBlockId const target = targetFrame.breakBB;
                // VLA C5 (D-CSUBSET-VLA): `break` exits every VLA scope opened INSIDE
                // the target loop/switch — restore SP to that frame's entry watermark
                // (the shallowest VLA scope strictly inside it) BEFORE the branch.
                // Nothing to restore when the target declared no VLA.
                if (vlaScopeStack_.size() > targetFrame.vlaDepthAtPush)
                    emitVlaRestore(targetFrame.vlaDepthAtPush);
                mir.addBr(target);
                return true;
            }
            case HirKind::ContinueStmt: {
                // C 6.8.6.2: `continue` targets the innermost LOOP; switch frames are
                // TRANSPARENT (a switch frame has continueBB invalid). Skip them — the
                // depth counts loop frames, matching the verifier's loops-only view
                // (so a `continue` inside a switch inside a loop reaches the loop).
                std::uint32_t depth = hir.branchDepth(node);
                MirBlockId target{};
                std::size_t targetVlaDepth = 0;   // VLA C5: the target loop's watermark
                for (std::size_t i = branchStack.size(); i-- > 0;) {
                    BranchFrame& frame = branchStack[i];
                    if (!frame.continueBB.valid()) continue;   // skip switch frames
                    if (depth == 0) {
                        frame.continueReferenced = true;
                        target = frame.continueBB;
                        targetVlaDepth = frame.vlaDepthAtPush;
                        break;
                    }
                    --depth;
                }
                if (!target.valid()) {
                    unsupported(node,
                        "ContinueStmt resolves to no enclosing loop (HIR "
                        "verifier should have flagged this)");
                    return false;
                }
                // VLA C5 (D-CSUBSET-VLA): `continue` exits the VLA scopes opened in the
                // current iteration's body — restore SP to the loop's entry watermark
                // BEFORE the back-edge (nothing if the loop body declared no VLA).
                if (vlaScopeStack_.size() > targetVlaDepth)
                    emitVlaRestore(targetVlaDepth);
                mir.addBr(target);
                return true;
            }
            case HirKind::GotoStmt: {
                // VLA C5 (D-CSUBSET-VLA): restore SP for every VLA scope this goto
                // EXITS (computed from HIR label ancestry) BEFORE the branch. The
                // entry-side ban (H_VlaJumpIntoScope) guarantees a legal goto never
                // enters a VLA scope past its decl, so the restore-target watermark
                // dominates the goto. No-op when the goto frees no VLA scope.
                emitGotoVlaTeardown(node);
                // Unconditional jump to the target label's block (created lazily so
                // forward + backward gotos resolve identically). The open block is
                // now terminated; a following sibling opens a fresh dead block (the
                // Block-lowering does this), which the unreachable-prune drops.
                mir.addBr(getOrCreateLabelBlock(hir.labelOrdinal(node)));
                return true;
            }
            case HirKind::IndirectGotoStmt: {
                // D-CSUBSET-COMPUTED-GOTO: `goto *expr` — an indirect branch to the
                // computed address. Successors = EVERY address-taken block (collected
                // at function entry), so the CFG edges are correct by construction
                // (reachability/DCE/phi see each `&&label` target reachable). A
                // computed goto with NO `&&label` anywhere in the function can never
                // have a valid target → fail loud rather than emit a successorless
                // IndirectBr (opcodeInfo requires ≥1 successor).
                //
                // VLA C5 (D-CSUBSET-VLA): a computed goto LEXICALLY inside a VLA scope
                // has a runtime target set — no single SP-restore watermark is provably
                // correct. Fail loud (defense-in-depth; the HIR verifier's
                // H_VlaComputedGotoInScope catches it pre-MIR). Runs fine with no open
                // VLA scope.
                if (!vlaScopeStack_.empty()) {
                    unsupported(node,
                        "computed `goto *` inside the scope of a variable-length "
                        "array is not supported — its runtime target set cannot be "
                        "proven to share the array's dynamic stack frame "
                        "(D-CSUBSET-VLA)");
                    return false;
                }
                if (addressTakenLabelOrdinals_.empty()) {
                    unsupported(node,
                        "computed `goto *` in a function that takes no label address "
                        "(`&&label`) — there is no valid target");
                    return false;
                }
                MirInstId const addr = lowerExpr(hir.indirectGotoTarget(node));
                if (!addr.valid()) return false;
                // Deterministic successor order: sort the ordinals so the IndirectBr's
                // successor list is stable across runs (the blocks are created lazily
                // here if a `&&label` was not yet lowered).
                std::vector<std::uint32_t> ordinals(addressTakenLabelOrdinals_.begin(),
                                                    addressTakenLabelOrdinals_.end());
                std::sort(ordinals.begin(), ordinals.end());
                std::vector<MirBlockId> targets;
                targets.reserve(ordinals.size());
                for (std::uint32_t const ord : ordinals) {
                    targets.push_back(getOrCreateLabelBlock(ord));
                }
                mir.addIndirectBr(addr, targets);
                return true;
            }
            case HirKind::LabelStmt: {
                // `label: stmt` — the label is a control-flow merge point. If the
                // open block still falls through (control arrives by fall-through,
                // not only by goto), branch into the label block; then continue
                // emitting INTO it and lower the labeled statement.
                // INVARIANT: every label ordinal is UNIQUE here — a duplicate label
                // name emits S_DuplicateLabel at the HIR-tier label pre-scan, which
                // halts the pipeline before MIR. (Without that gate, two LabelStmts
                // sharing an ordinal would self-branch B→B + double-beginBlock here.)
                MirBlockId const lb = getOrCreateLabelBlock(hir.labelOrdinal(node));
                if (!mir.openBlockHasTerminator()) mir.addBr(lb);
                mir.beginBlock(lb);
                return lowerStmt(hir.labelBody(node));
            }
            case HirKind::SwitchStmt: {
                // c60 (Design I-A): a FLATTENED switch — the dispatch arms map each
                // case value (and `default`) to the ordinal of a synthetic LabelStmt
                // marker in the flat body Block; the body is lowered as ONE statement
                // sequence with fall-through, exactly like a block. Lower as:
                //   - lower discriminant
                //   - getOrCreate a label-block per dispatch arm (the SAME label-block
                //     machinery goto/label use); `default`'s block is `defaultBB`
                //     (else the exit/join)
                //   - emit `Switch(disc, cases…, defaultBB)` targeting those blocks
                //   - push `{invalid-continue, exit}` so `break;` targets the join
                //   - lower the flat body Block; its case/default LabelStmt markers
                //     beginBlock their label-blocks (fall-through is straight-line);
                //     a fall-off-the-end branches to the join.
                HirNodeId const discN = hir.switchDiscriminant(node);
                auto       const arms  = hir.switchArms(node);

                MirInstId const disc = lowerExpr(discN);
                if (!disc.valid()) return false;

                MirBlockId const exitBB = mir.createBlock(StructCfMarker::SwitchJoin);

                // Build the (caseValue, target-label-block) list + resolve defaultBB,
                // targeting the case markers' label-blocks (created lazily / forward
                // by getOrCreateLabelBlock — the body's LabelStmt markers fill them).
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                cases.reserve(arms.size());
                MirBlockId defaultBB{};
                for (HirNodeId const arm : arms) {
                    MirBlockId const target =
                        getOrCreateLabelBlock(hir.caseArmLabelOrdinal(arm));
                    if (hir.caseArmIsDefault(arm)) {
                        if (defaultBB.valid()) {
                            unsupported(arm, "switch has more than one "
                                              "default arm (HIR verifier "
                                              "should have flagged this)");
                            sealCreatedAsUnreachable(exitBB);
                            return false;
                        }
                        defaultBB = target;
                        continue;
                    }
                    auto const valN = hir.caseArmValue(arm);
                    if (!valN.has_value()) {
                        unsupported(arm, "non-default CaseArm without "
                                          "match value (HIR verifier "
                                          "should have flagged this)");
                        sealCreatedAsUnreachable(exitBB);
                        return false;
                    }
                    MirInstId const caseVal = lowerExpr(*valN);
                    if (!caseVal.valid()) {
                        sealCreatedAsUnreachable(exitBB);
                        return false;
                    }
                    cases.emplace_back(caseVal, target);
                }
                if (!defaultBB.valid()) defaultBB = exitBB;

                mir.addSwitch(disc, cases, defaultBB);

                // The discriminant block is now terminated by the Switch. If the body
                // starts with a case marker (a LabelStmt — the usual shape), that
                // marker's lowering opens its own label-block directly. ONLY if the
                // body starts with a non-label statement (a jumped-over leading decl)
                // do we need a fresh predecessor-less block for it to lower into (the
                // unreachable-prune drops it, since the dispatch jumped straight to
                // the case/default blocks).
                branchStack.push_back({MirBlockId{}, exitBB});
                if (!switchBodyStartsWithLabel(hir.switchBody(node))) {
                    MirBlockId const preCase =
                        mir.createBlock(StructCfMarker::Linear);
                    mir.beginBlock(preCase);
                }
                bool const bodyOk = lowerStmt(hir.switchBody(node));
                if (!bodyOk) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    sealCreatedAsUnreachable(exitBB);
                    branchStack.pop_back();
                    return false;
                }
                // Fall-off-the-end of the body → the join.
                if (!mir.openBlockHasTerminator()) mir.addBr(exitBB);
                branchStack.pop_back();

                mir.beginBlock(exitBB);
                return true;
            }
            case HirKind::TypeDecl: {
                // c30 (D-CSUBSET-LOCAL-TYPEDEF): a BLOCK-SCOPED typedef lowers to
                // a statement-position TypeDecl. For an ORDINARY (non-VLA) typedef
                // this is a no-op — like the top-level TypeDecl, it "interns a type
                // into the lattice but emits no code" (the alias was resolved at
                // semantic time).
                //
                // VLA C4b (D-CSUBSET-VLA): a VARIABLE-LENGTH-array typedef
                // (`typedef int R[n];` / multi-dim `R[n][m]`/`R[5][n]`/`R[n][5]`)
                // is the ONE typedef with a runtime effect: C99 §6.7.7p2 evaluates
                // the size expr `n` ONCE, when the typedef declaration is REACHED
                // (NOT at each later `R a;`). FREEZE it HERE, in program order:
                // `computeVlaByteSize` lowers each dim exactly once + Allocas+Stores
                // R's per-level 8-byte U64 size slots keyed by R's SymbolId
                // (`vlaStrideSlot[(R, levelType)]`). Every later `R a;` COPIES those
                // frozen slots down into its own — so `n` changing between two `R`
                // uses does NOT re-size them (the freeze-once invariant). The dim
                // bound(s) were captured in `lowerTypeDecl` under R's SymbolId. On a
                // belt fail-loud (M4): seal the open block + return false, mirroring
                // the `vlaAllocaForLocal` caller (never a silently-dropped freeze).
                TypeId   const ty = hir.typeDeclType(node);
                SymbolId const R  = hir.typeDeclSymbol(node);
                if (R.valid() && ty.valid()
                    && (interner.isVlaArray(ty) || interner.typeContainsVla(ty))) {
                    if (!computeVlaByteSize(R, ty, node, /*freezeLevelSlots=*/true)
                             .has_value()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        return false;
                    }
                }
                return true;
            }
            default: break;
        }
        unsupported(node,
            std::format("HIR statement kind ordinal {} not yet supported "
                        "(HIR id {})", static_cast<unsigned>(k), node.v));
        return false;
    }

    // Lower a single HIR Function declaration: open a MirFunc, create the
    // entry block, emit Arg instructions for each param, lower the body.
    //
    // Open-block discipline (review-fix): every code path that has called
    // `mir.beginBlock` MUST seal the block before returning, even on error
    // — otherwise `MirBuilder::finish()` aborts the process. We seal failed
    // paths with `addUnreachable()` so finish() can complete and the
    // collected diagnostics reach the caller. Successful void-bodied
    // functions also get an implicit `addReturn()` here (the comment-only
    // "deferred" was a hazard — the comment didn't avoid the crash).
    bool lowerFunction(HirNodeId node) {
        TypeId const signature = hir.functionSignature(node);
        SymbolId const symbol  = hir.functionSymbol(node);
        // Pre-block checks: bail BEFORE opening any block.
        if (!signature.valid()) {
            unsupported(node, "Function with InvalidType signature (HIR "
                              "verifier should have flagged this)");
            return false;
        }
        auto params = hir.functionParams(node);
        // COPY the param types out of the interner's pool: `fnParams` returns a
        // SPAN into that pool, and the sret setup + by-value param/return
        // synthesis below intern fresh `Ptr<…>` types — an intern can REALLOCATE
        // the pool, dangling a retained span (an interner-state-dependent
        // "TypeId out of range" crash; FC7 C1c). The owning vector is immune.
        std::vector<TypeId> const paramTypes = [&] {
            auto const s = interner.fnParams(signature);
            return std::vector<TypeId>(s.begin(), s.end());
        }();
        if (params.size() != paramTypes.size()) {
            unsupported(node,
                std::format("Function param count {} mismatches FnSig param "
                            "count {}", params.size(), paramTypes.size()));
            return false;
        }
        // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): a by-value struct/union PARAM
        // is classified + reconstructed in the param loop below (per the active
        // CC's strategy, via `receiveByValueParam`). A by-value struct/union
        // RETURN by value (D-FC7-SYSV-STRUCT-RETURN-IN-REGS) is classified +
        // synthesized AFTER the entry block opens: an sret return prepends a
        // hidden result-pointer `Arg`, and an in-register return loads its
        // eightbyte pieces in the `ReturnStmt`. See the `currentFnResult_` setup
        // just below `beginBlock`. A CC whose strategy can't classify the
        // aggregate (e.g. AAPCS64 until C3) fails loud there.

        // Per-function context reset: each function owns its own SSA/alloca
        // bindings — entries from the previous function are stale.
        symbolToValue.clear();
        addressableLocal.clear();
        vlaStrideSlot.clear();   // VLA C2/C3 (D-CSUBSET-VLA): per-function size/stride slots
        labelBlocks_.clear();   // FC5: labels are function-scoped
        addressTakenLabelOrdinals_.clear();  // D-CSUBSET-COMPUTED-GOTO
        // VLA C5 (D-CSUBSET-VLA): the block-scope teardown state is per-function.
        vlaScopeStack_.clear();
        vlaScopeCounter_ = 0;
        labelNodeByOrdinal_.clear();
        // FC7 C1c: per-function by-value return state.
        currentFnResult_ = interner.fnResult(signature);
        sretPtr_         = InvalidMirInst;

        // Pre-pass: scan the body to find params (and locals) whose address
        // is taken. Address-taken params must live in memory (alloca-backed),
        // not as pure SSA `Arg` values — otherwise the address would be
        // undefined. Body locals always get a slot at their `VarDecl`, so
        // the pre-pass result is only consulted for the param loop below.
        HirNodeId const body = hir.functionBody(node);
        std::unordered_set<std::uint32_t> addressTaken;
        collectAddressTakenSymbols(body, addressTaken);
        // c116b H1 (D-WIN64-SEH-FUNCLETS): also force every SEH `__except` FILTER-
        // referenced symbol memory-backed. The filter is extracted into a funclet
        // that recovers parent locals off the establisher frame (RecoverParentFrame
        // Slot), which only works for a FRAME slot — a bare `Arg` param is otherwise
        // unrecoverable. Union into the address-taken set so the param-reception loop
        // below alloca-backs it (sqlite's `sehExceptionFilter(pWal, …)` param).
        collectSehFilterReferencedSymbols(body, addressTaken);
        // D-CSUBSET-COMPUTED-GOTO: collect the address-taken LABEL ordinals up front
        // (a forward `&&end` must be a known IndirectBr successor regardless of
        // textual order). The blocks are created lazily at first reference.
        collectAddressTakenLabels(body, addressTakenLabelOrdinals_);
        // VLA C5 (D-CSUBSET-VLA): map every LabelStmt ordinal → node so a `goto`
        // inside a VLA scope can resolve its target and compute the exited scopes.
        collectLabelNodes(body);

        // From here on a block is open — any return-false MUST seal it.
        // D-CSUBSET-LINKAGE-SPECIFIERS / D-OPT7-LINKAGE-HIR-TO-MIR-MAPPING
        // (pre-OPT7 P2): stamp the declared linkage (default Global/Default when
        // unannotated) so DCE's `isExternallyVisible` protect predicate sees a
        // `static` function as Local (eliminable) and `__attribute__((weak))` as
        // Weak. Closes the P2 stub: linkage now flows source → HIR → MIR.
        LinkageAttr la{};
        if (linkageMap != nullptr)
            if (auto const* p = linkageMap->tryGet(node)) la = *p;
        mir.addFunction(signature, symbol, la.binding, la.visibility);
        MirBlockId const entry = mir.createBlock(StructCfMarker::EntryBlock);
        mir.beginBlock(entry);

        // FC7 C1c: the per-class arg-ordinal counter is SHARED by the sret hidden
        // pointer (below) and the real params, so a struct-returning function's
        // first INTEGER arg register goes to the result pointer and every real arg
        // shifts by one — hoisted above the sret `Arg` so both use one counter.
        ArgOrdinalCounter argCtr{config.argSlotAligned};
        // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: reset the incoming-
        // stack-byte cursor + the stacked-aggregate residual guards per function
        // (accumulated below as the param-reception loop places each fixed param).
        currentFnFixedStackBytes_     = 0;
        currentFnSawStackedAggregate_ = false;
        currentFnSawFixedStackParam_  = false;

        // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value struct/union
        // RETURN. Classify under the active CC's strategy. ByReference (class
        // MEMORY, >16B) ⇒ sret: prepend a hidden result-pointer `Arg` (consuming
        // the first INTEGER ordinal); the `ReturnStmt` copies the result through
        // it and returns it. InRegisters (≤16B) ⇒ no hidden arg; the `ReturnStmt`
        // loads the eightbyte pieces. A strategy that can't classify the aggregate
        // (AAPCS64 until C3) fails loud (sealing the open block first).
        if (currentFnResult_.valid()) {
            // D-CSUBSET-BITINT-C2-WIDE: a wide-`_BitInt` return uses the SAME sret /
            // 2-GPR-piece prologue as a by-value struct/union (isByValueClass).
            if (isByValueClass(interner, currentFnResult_)) {
                auto const abi = byValueClassify(currentFnResult_);
                if (!abi.has_value()) {
                    unsupported(node,
                        "returning a by-value struct/union is not supported by "
                        "this target's calling convention "
                        "(D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                if (abi->kind == AbiPassing::Kind::ByReference) {
                    // SysV/Win64 ⇒ the result pointer is a hidden first INTEGER arg
                    // (consumes the first GPR ordinal, shifting real args by one).
                    // AAPCS64/Apple x8-sret ⇒ it arrives in the dedicated indirect-
                    // result register via `ReadIndirectResult` and consumes NO arg
                    // ordinal (real args still start at x0). FC7 C3.
                    // The sret slot is ALWAYS pushed as the first actual
                    // operand (position 0) — for the hidden-arg CC it is this
                    // Arg; for x8-sret it is the (Arg-less) slot the
                    // ReadIndirectResult stands in for. Advance the flat
                    // position for it in BOTH cases so the real params start at
                    // position 1, matching the operand list (arg_payload.hpp).
                    std::uint32_t const sretPos = argCtr.nextPosition();
                    sretPtr_ = config.aggregateSretViaHiddenArg
                        ? mir.addArg(argCtr.next(AbiPieceClass::Gpr),
                                     interner.pointer(currentFnResult_), sretPos)
                        : mir.addReadIndirectResult(
                              interner.pointer(currentFnResult_));
                    if (!sretPtr_.valid()) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        return false;
                    }
                }
            }
        }

        // Params: one `Arg` instruction per param, in declaration order. If
        // the param's address is ever taken in the body, ALSO allocate a slot
        // and store the arg into it — every read of that symbol then goes
        // through `Load(alloca)`. Otherwise the param stays in the pure-SSA
        // `symbolToValue` map.
        // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG): a by-value struct/union param
        // expands to register pieces (or a by-ref pointer); `argCtr` hands out the
        // PER-CLASS (or, for a slot-aligned CC, flat) physical-arg ordinal, kept in
        // lockstep with the caller's operand order. For an all-GPR scalar-only
        // function the per-class GPR ordinal equals the param index (no change); a
        // mixed int/float signature now lands each arg in its own class (fixes
        // D-ML7-2.10). `argCtr` is hoisted above (shared with the sret arg).
        // c63 (D-CSUBSET-VA-LIST-PARAM-SLOT): is `t` a SysV `va_list` PARAM — an
        // `__va_list_tag[1]` array (or, defensively, a Ptr<__va_list_tag>) — under the
        // SysVRegisterSave strategy? Such a param's incoming GPR is a POINTER to the
        // caller's tag (C 6.7.6.3p7 array-param adjustment); a dedicated arm below
        // registers that pointer. Win64/Apple (`char*`) + AAPCS64 (`__va_list` struct)
        // va_list params are NOT this — they ride the addressTaken-scalar / by-value-
        // struct arms (the former marked address-taken by va_arg usage, since their
        // Ptr<I8> type is indistinguishable from a plain char*).
        auto isSysVVaListParam = [&](TypeId t) -> bool {
            if (!config.vaListLayout.has_value()
                || config.vaListLayout->strategy
                       != VaListStrategy::SysVRegisterSave)
                return false;
            if (!t.valid()) return false;
            TypeKind const tk = interner.kind(t);
            if (tk != TypeKind::Array && tk != TypeKind::Ptr) return false;
            auto const ops = interner.operands(t);
            return !ops.empty() && ops[0].valid()
                && interner.kind(ops[0]) == TypeKind::Struct
                && interner.name(ops[0]) == "__va_list_tag";
        };
        for (std::size_t i = 0; i < params.size(); ++i) {
            HirNodeId const p = params[i];
            // A param is a VarDecl whose typeId carries the param's type;
            // verifier already enforced the kind invariant upstream.
            SymbolId const sym = hir.varDeclSymbol(p);
            TypeId const ty = paramTypes[i];
            // D-CSUBSET-BITINT-C2-WIDE: a wide-`_BitInt` PARAM is received BY VALUE
            // exactly like a struct/union (isByValueClass → receiveByValueParam places
            // the 2-GPR pieces / by-ref pointer into the param's local slot).
            if (isByValueClass(interner, ty)) {
                // Classify FIRST (FC12a-struct): the variadic check now keys on the
                // ABI kind, not the type kind. An InRegisters fixed struct param in
                // a variadic function is fine — receiveByValueParam advances argCtr
                // per-class per piece, so currentFnFixedGpr_/Fpr_ (stamped below)
                // include the struct's pieces and va_start's cursors reflect them.
                auto const abi = byValueClassify(ty);
                if (!abi.has_value()) {
                    unsupported(p, "a by-value struct/union parameter is not "
                                   "supported by this target's calling "
                                   "convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                // FC12a-struct (D-FC12A-VARIADIC-MEMORY-CLASS-STRUCT): a ByReference
                // (MEMORY class, >16B) FIXED param in a variadic fn uses the hidden-
                // pointer convention — receiveByValueParam's ByReference arm receives
                // it as ONE GPR arg (counted in argCtr.gpr → currentFnFixedGpr_), so
                // va_start's gp_offset starts past it correctly, exactly like any
                // other fixed GPR param. The SEPARATE fixed-params-OVERFLOW-to-stack
                // case (≥7 fixed integer regs of THIS class → the va_start guard fires
                // D-FC12A-VARIADIC-OVERFLOW-FIXED-STACK-ARGS) is unchanged; the common
                // case (one >16B fixed param = 1 GPR + ≤5 scalars → currentFnFixedGpr_
                // ≤ 6) lowers cleanly.
                // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS:
                // receiveByValueParam now PLACES the aggregate (InRegisters pieces, a
                // ByReference hidden pointer, OR — when it straddles the reg/stack
                // boundary — all-or-nothing from the incoming stack via a
                // RecvByValueStackParam) AND accounts any stacked bytes in
                // `currentFnFixedStackBytes_` for va_start's overflow base. No
                // separate flag is needed (the old fail-loud fold is now a real fix).
                if (!receiveByValueParam(sym, ty, p, *abi, argCtr)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                continue;
            }
            // c63 (D-CSUBSET-VA-LIST-PARAM-SLOT): a SysV `va_list` PARAMETER. By C
            // 6.7.6.3p7 array-param adjustment the caller passes a POINTER to ITS OWN
            // `__va_list_tag` in one GPR; `va_arg` must advance the CALLER's tag
            // (forwarding semantics), so register that incoming pointer AS the symbol's
            // address — NO alloca, NO 24-byte copy (a copy reads a stale tag = a silent
            // miscompile). Mirrors receiveByValueParam's ByReference precedent ("the
            // caller's copy is the param"). Consumes ONE GPR ordinal — exactly what the
            // scalar arm would for this array-decayed pointer — so the remaining params'
            // ordinals + the fixed-arg count stay correct. (Win64/Apple `char*` va_list
            // params take the addressTaken-scalar arm below; AAPCS64 `__va_list` struct
            // params took the by-value-struct arm above.)
            if (isSysVVaListParam(ty)) {
                auto const ops = interner.operands(ty);
                std::uint32_t const pos = argCtr.nextPosition();  // one operand
                MirInstId const ptr = mir.addArg(argCtr.next(AbiPieceClass::Gpr),
                                                 interner.pointer(ops[0]), pos);
                if (!ptr.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                addressableLocal[sym.v] = ptr;
                continue;
            }
            // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG / fixes D-ML7-2.10): a scalar
            // param's `Arg` payload is its PER-CLASS register ordinal (GPR/FPR
            // counted separately for an independent CC), not the param index.
            AbiPieceClass const sCls = scalarArgClass(ty);
            // D-FC12-VARIADIC-OVERFLOW-FIXED-AGGREGATE-STACK-ARGS: a scalar whose
            // per-class ordinal has reached the pool rides the INCOMING stack
            // (independent CC); account it in the byte cursor so a body va_start's
            // overflow base skips it, and fail loud on the post-stacked-aggregate
            // desync residual. (Win64/slot-aligned uses currentFnFixedFlat_ + the
            // home-base path, not the byte cursor — untouched.)
            if (!config.argSlotAligned) {
                std::uint32_t const ord =
                    (sCls == AbiPieceClass::Fpr) ? argCtr.fpr : argCtr.gpr;
                std::uint32_t const pool =
                    (sCls == AbiPieceClass::Fpr) ? config.argFprCount
                                                 : config.argGprCount;
                if (ord >= pool && !accountFixedStackScalar(p)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
            }
            std::uint32_t const scalarPos = argCtr.nextPosition();  // one operand
            MirInstId const arg = mir.addArg(argCtr.next(sCls), ty, scalarPos);
            if (addressTaken.contains(sym.v)) {
                MirInstId const slot = allocaForLocal(sym, ty, p);
                if (!slot.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                std::array<MirInstId, 2> ops{arg, slot};
                // c21 (D-CSUBSET-VOLATILE-QUALIFIER): a `volatile` address-taken
                // scalar PARAM is a volatile object — its incoming-arg→slot init
                // store carries the flag (the param VarDecl node `p` was annotated
                // at CST→HIR's `lowerVarLikeInto`, same path as a body local), so
                // the read side (flagged at the Ref) and the init store agree.
                // FC17.9(d) (D-CSUBSET-ATOMIC): an `_Atomic` param's incoming-arg→slot
                // reception is INITIALIZATION (C11 7.17.2.1 — not itself atomic), so
                // it stays a plain Store, marked AtomicInitExempt so the verifier's
                // atomic belt spares it despite the atomic-qualified slot pointee.
                mir.addInst(MirOpcode::Store, ops, InvalidType, /*payload=*/0,
                            volatileFlagFor(p) | MirInstFlags::AtomicInitExempt);
            } else {
                symbolToValue[sym.v] = arg;
            }
            // VLA C4a-param (D-CSUBSET-VLA): a PARAMETER pointer-to-VLA (`int (*p)[n]`,
            // or the adjusted `int a[][n]`) freezes its runtime POINTEE row stride HERE,
            // in the entry block at the param's decl point — mirroring the local VarDecl
            // store. SIMPLER than the local case: `n` (an EARLIER param) is ALREADY
            // placed by a prior loop iteration (symbolToValue / addressableLocal), so
            // there is NO decl-site-vs-entry-hoist hazard — reading `n` here is in
            // program order. `scaleIndexToBytes` recovers the `p[i]` row stride from the
            // `(p, pointeeTy)` slot exactly as a declared VLA row. A ptr to a FIXED array
            // (`int (*p)[5]`) has a non-VLA pointee → skipped (compile-time stride). On a
            // belt fail-loud (e.g. a non-standard `int (*p)[n], int n` where `n` is not yet
            // placed) TERMINATE the open entry block before bailing — the same convention as
            // every other param-loop bail above — so the half-built function finalizes as a
            // clean `mir` fail (diagnostic preserved), never a MirBuilder "no terminator" fatal.
            if (interner.kind(ty) == TypeKind::Ptr) {
                auto const pops = interner.operands(ty);
                if (!pops.empty() && interner.typeContainsVla(pops[0])
                    && !storePtrToVlaStride(sym, pops[0], p)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
            }
        }
        // FC12a-core (D-FC12A-VARIADIC-CALLEE): after every FIXED param is placed,
        // `argCtr.gpr`/`.fpr` hold the count of fixed params that consumed an
        // integer / SSE arg register (the sret hidden GPR, if any, is already
        // included — argCtr is the single source). A SysVRegisterSave body `va_start`
        // reads these to initialize gp_offset/fp_offset past the fixed args.
        // FC12b: `argCtr.flat` is the slot-aligned named-arg SLOT count (Win64); a
        // HomogeneousPointer `va_start` reads it to set ap = &home[flat], past ALL
        // named slots (int OR float). For SysV (`slotAligned=false`) `flat` stays 0
        // and is unused; for Win64 `gpr`/`fpr` stay 0 and `flat` is the live count.
        currentFnFixedGpr_  = argCtr.gpr;
        currentFnFixedFpr_  = argCtr.fpr;
        currentFnFixedFlat_ = argCtr.flat;
        // c69 (D-MIR-ENTRY-BLOCK-ALLOCA-HOIST): pre-emit every body-local's
        // storage `Alloca` into the ENTRY block, which is still the open block
        // here (the param loop appended Args/param-slots; no terminator yet) and
        // dominates every use. This is the conventional "all allocas in entry"
        // discipline: it keeps an ENTRY-UNREACHABLE block (e.g. a switch's
        // pre-first-case block, c60) free of value-producing instructions so the
        // mandatory unreachable-prune can drop it without stranding a slot that a
        // reachable use still references (the c69 MirFunctionRebuilder abort,
        // D-OPT2-REWRITE-MAP-COMPLETENESS). The init Store stays at the (possibly
        // dead) decl site. The append-only MirBuilder cannot insert into a sealed
        // block, so the slots MUST be emitted now, up front; the VarDecl lowering
        // then reuses the pre-emitted slot. Mem2Reg still promotes/removes the
        // unused ones. A by-value aggregate PARAM slot is already placed by the
        // param loop (a distinct symbol); an un-sizeable aggregate/array local is
        // skipped here and left to the VarDecl site's fail-loud (no behavior
        // change for that invalid-C edge case).
        {
            std::vector<HirNodeId> localDecls;
            collectLocalDecls(body, localDecls);
            for (HirNodeId d : localDecls) {
                TypeId   const dty  = hir.varDeclType(d);
                SymbolId const dsym = hir.varDeclSymbol(d);
                if (!dty.valid() || !dsym.valid()) continue;  // VarDecl site fail-louds
                if (addressableLocal.contains(dsym.v)
                    || symbolToValue.contains(dsym.v))
                    continue;  // already slotted (defensive; locals/params are distinct)
                TypeKind const dtk = interner.kind(dty);
                // VLA C1a (D-CSUBSET-VLA) — LOAD-BEARING: a variable-length array
                // (kind==Array, `aggregateByteSize` nullopt because computeLayout
                // nullopts on the -2 length) matches this skip, so its Alloca is NOT
                // hoisted to the entry block — it emits at the VarDecl site in program
                // order, AFTER the runtime bound (`n` / a side-effecting `f()`) has
                // been evaluated. Hoisting it would read an uninitialized bound. The
                // MIR-tier ORDERING pin (IMPORTANT-3) guards this implicit dependency.
                if ((dtk == TypeKind::Struct || dtk == TypeKind::Union
                     || dtk == TypeKind::Array)
                    && !aggregateByteSize(dty).has_value())
                    continue;  // un-sizeable here → leave to the VarDecl site (old path)
                if (!allocaForLocal(dsym, dty, d).valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
            }
        }
        // Body: a single Block of statements.
        if (!lowerStmt(body)) {
            // Failed mid-body — seal the block so finish() can complete and
            // the caller sees the actual diagnostic instead of an abort.
            // Inner error paths may have already sealed (e.g. an If/While
            // catch); `openBlockHasTerminator` makes this idempotent.
            if (!mir.openBlockHasTerminator()) mir.addUnreachable();
            return false;
        }
        // Implicit-void-return synthesis (review-fix). Source like
        // `void f() {}` has no explicit return; HR6's checkReturnCompleteness
        // accepts that for Void-result functions, but the MIR block still
        // needs a terminator. Detect "no terminator emitted by the body" via
        // the builder's open-block state and synthesize the implicit return.
        // A non-void function that fell through without a return is a HIR
        // verifier failure (already flagged upstream); seal it with
        // Unreachable so finish() can complete + diagnostics propagate.
        if (mir.openBlockHasTerminator()) return true;
        TypeId const resultType = interner.fnResult(signature);
        if (resultType.valid() && interner.kind(resultType) == TypeKind::Void) {
            mir.addReturn();
        } else {
            mir.addUnreachable();
        }
        return true;
    }

    // Pre-pass: collect the set of module-level function symbols so that a
    // direct `Call` whose callee is a `Ref` to a function declared LATER in
    // the module still resolves. Direct calls go through `GlobalAddr(symbol)`,
    // so we only need the set of valid symbols — the MirFunc is built lazily
    // when each function is lowered in the main pass.
    void collectFunctions(HirNodeId moduleNode) {
        for (HirNodeId decl : hir.moduleDecls(moduleNode)) {
            if (hir.kind(decl) != HirKind::Function) continue;
            TypeId const sig    = hir.functionSignature(decl);
            SymbolId const sym  = hir.functionSymbol(decl);
            if (!sig.valid() || !sym.valid()) continue;
            functionSymbols.insert(sym.v);
        }
    }

    // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): pre-pass — SEED `functionSymbols` with
    // every pe64 <threads.h> shim symbol (mtx_lock etc.). CST→HIR SKIPPED these from
    // extern-import synthesis (kernel32 exports no such name — the eager-import law), so
    // `collectExterns` never registers them; without this seed the user's
    // `mtx_lock(&m)` call would fail loud "HIR Ref to unbound symbol" at the Ref
    // lowering BEFORE the synth pass runs. Seeding routes the call through the ordinary
    // `GlobalAddr(sym)` path (a not-yet-defined function callee, which MirVerifier
    // tolerates); `synthesizeThreadsShim` (mir/merge) supplies the definition pre-link.
    // Empty for every non-threads / elf / macho TU. Agnostic: keys on the config-carried
    // recipe tag, never a format check.
    void collectThreadShimSymbols() {
        if (synthRecipeMap == nullptr) return;
        for (auto const& [symV, recipeId] : *synthRecipeMap) {
            (void)recipeId;
            functionSymbols.insert(symV);
        }
    }

    // Pre-pass: collect extern function SymbolIds + build per-row
    // `ExternImport` records by consulting the optional
    // `HirAttribute<FfiMetadata>` side-table. Extern SymbolIds are
    // ALSO inserted into `functionSymbols` so a `Ref` to an extern
    // routes through the existing GlobalAddr emission path —
    // structurally a direct call to an extern produces
    // `Call GlobalAddr(externSym), args...`, the same MIR shape as
    // intra-module calls. The downstream LIR + assembler emit a
    // relocation against the SymbolId and the linker resolves it
    // against `AssembledModule.externImports` instead of
    // `functions`. (LK6 cycle 2d — D-LK6-6 closure.)
    //
    // Fail-loud contract (the post-fold review verified this against
    // the silent-failure rule): if the FFI map is absent OR a per-
    // node entry is missing OR its `mangledName` / `importLibrary`
    // is empty OR the extern's SymbolId collides with an intra-
    // module function OR a prior extern in this module declares
    // the same SymbolId OR the FnSig is invalid — the pre-pass
    // emits `H_UnsupportedLoweringForKind` anchored at the HIR
    // node's source span and the row is NOT pushed. Surfacing at
    // the HIR tier preserves source-span context that the linker
    // tier can no longer recover (which would otherwise reduce
    // every metadata problem to "unresolved extern" with no
    // pinpoint location).
    void collectExterns(HirNodeId moduleNode) {
        // Track extern SymbolIds we've already emitted a row for, to
        // catch duplicate-extern-declaration drift. The linker
        // (LK6 cycle 2a) also rejects extern-table duplicates, but
        // surfacing here keeps the source span attached to the
        // diagnostic (silent-failure HIGH fold, LK6 cycle 2d
        // post-fold review).
        std::unordered_set<std::uint32_t> seenExternSyms;
        for (HirNodeId decl : hir.moduleDecls(moduleNode)) {
            // c82 (D-LK-EXTERN-DATA-IMPORT): ExternGlobal — an extern DATA
            // object with no intra-module definition (the same-TU-definition
            // case never reaches HIR: the semantic merge suppresses the
            // extern node). Registered EXACTLY like an extern function —
            // an ExternImport row (flagged isData) + a symbol registration —
            // except the symbol goes into `globalSymbols`, so a `Ref` lowers
            // through the SAME GlobalAddr(+Load) path an intra-module global
            // uses. Cross-TU: the LK11 merge collapses the row onto a
            // sibling CU's definition (sqlite3.c's `sqlite3_version`) and
            // drops the import; a TRUE library data import (libc `stdout`)
            // survives to the link tier, which fail-louds until the
            // extern-data binding model lands (the c82 §B).
            if (hir.kind(decl) == HirKind::ExternGlobal) {
                TypeId const ty = hir.externGlobalType(decl);
                if (!ty.valid()) {
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — invalid TypeId; the "
                        "semantic model failed to resolve this extern "
                        "object's declared type.", decl.v));
                    continue;
                }
                SymbolId const sym = hir.externGlobalSymbol(decl);
                if (!sym.valid()) {
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — missing SymbolId; the "
                        "semantic model failed to bind a symbol to this "
                        "extern declaration.", decl.v));
                    continue;
                }
                if (functionSymbols.contains(sym.v)
                    || globalSymbols.contains(sym.v)) {
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — SymbolId #{} is already "
                        "declared as an intra-module function or global. "
                        "Each SymbolId must belong to exactly one "
                        "definition surface.", decl.v, sym.v));
                    continue;
                }
                if (!seenExternSyms.insert(sym.v).second) {
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — SymbolId #{} is already "
                        "declared by a prior extern in this module.",
                        decl.v, sym.v));
                    continue;
                }
                FfiMetadata const* meta = (ffiMap != nullptr)
                    ? ffiMap->tryGet(decl)
                    : nullptr;
                // Same fail-loud metadata contract as the function arm
                // below: no name / no library ⇒ the linker could neither
                // resolve nor import the symbol — surface it HERE with the
                // source span.
                if (meta == nullptr || meta->mangledName.empty()) {
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — `mangledName` is missing "
                        "from the HirAttribute<FfiMetadata> side-table. "
                        "Every extern symbol must carry a non-empty mangled "
                        "name (the linker's import-table key).", decl.v));
                    continue;
                }
                if (meta->importLibrary.empty()) {
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — `importLibrary` is "
                        "missing from the HirAttribute<FfiMetadata> "
                        "side-table. Every extern symbol must declare the "
                        "dynamic library that owns it.", decl.v));
                    continue;
                }
                ExternImport row;
                row.symbol      = sym;
                row.mangledName = meta->mangledName;
                row.libraryPath = meta->importLibrary;
                row.isData      = true;
                // TLS C1 (D-CSUBSET-THREAD-LOCAL): `extern thread_local int
                // e;` — carry the declaration's thread-storage duration on
                // the import row (mir_merge's survivingExterns copies the
                // whole row, so the flag rides the LK11 merge for free) and
                // register the symbol as a TLS address-constant reject
                // target (★CRIT-1 — `int *p = &e;` at file scope is as
                // ill-formed for an extern TLS object as for a local one).
                if (threadLocalMap != nullptr) {
                    if (auto const* p = threadLocalMap->tryGet(decl);
                        p != nullptr && p->isThreadLocal) {
                        row.isThreadLocal = true;
                        threadLocalTargetSymbols.insert(sym.v);
                    }
                }
                // c84 (D-LK-EXTERN-DATA-IMPORT): derive the imported
                // OBJECT's byte size + alignment from the declared
                // type's LAYOUT (the same computeLayout every sizeof /
                // global-emission consumer uses — a `FILE*` object is
                // the DataModel's pointer width, never a hardcoded 8).
                // The ELF copy-relocation emitter reserves a `.bss`
                // slot of exactly this shape and stamps `st_size`.
                // An INCOMPLETE declared type (`extern const char
                // v[];` — no computable layout) legitimately leaves
                // both 0: legal C for a cross-TU extern the LK11
                // merge resolves against its defining sibling CU;
                // a TRUE library import surviving to the walker with
                // size 0 fails loud THERE (an unsized copy slot
                // cannot be reserved), keeping the incomplete-array
                // cross-TU case working.
                if (config.aggregateLayoutLoaded) {
                    auto const layout = computeLayout(
                        ty, interner, config.aggregateLayout,
                        config.dataModel);
                    if (layout.has_value()) {
                        row.dataSizeBytes  = layout->size;
                        row.dataAlignBytes = layout->align.bytes();
                    }
                }
                externImports.push_back(std::move(row));
                globalSymbols.insert(sym.v);
                continue;
            }
            if (hir.kind(decl) != HirKind::ExternFunction) continue;
            TypeId const sig = hir.externFunctionSignature(decl);
            if (!sig.valid()) {
                // Symmetric with `collectFunctions`'s `sig.valid()`
                // guard: a malformed extern with no FnSig has no
                // ABI shape the assembler / linker can resolve
                // against. Fail loud rather than emit a half-built
                // row. (silent-failure MEDIUM fold.)
                unsupported(decl, std::format(
                    "HIR ExternFunction (id {}) — invalid TypeId on "
                    "FnSig; the semantic model failed to resolve "
                    "this extern's signature.", decl.v));
                continue;
            }
            SymbolId const sym = hir.externFunctionSymbol(decl);
            if (!sym.valid()) {
                unsupported(decl, std::format(
                    "HIR ExternFunction (id {}) — missing SymbolId; "
                    "the semantic model failed to bind a symbol to "
                    "this extern declaration.", decl.v));
                continue;
            }
            if (functionSymbols.contains(sym.v)) {
                // Cross-table ambiguity: this SymbolId is already
                // owned by an intra-module `Function`. The linker's
                // `LK6 cycle 2a` cross-table reject catches this
                // too, but anchoring the diagnostic at the HIR
                // node here preserves source-span context that the
                // linker tier can no longer recover. (silent-
                // failure HIGH fold.)
                unsupported(decl, std::format(
                    "HIR ExternFunction (id {}) — SymbolId #{} is "
                    "already declared as an intra-module function. "
                    "Each SymbolId must belong to either a function "
                    "OR an extern, never both.", decl.v, sym.v));
                continue;
            }
            if (!seenExternSyms.insert(sym.v).second) {
                // Two ExternFunction decls with the same SymbolId
                // — likely a copy-paste in the test fixture or a
                // multi-CU SymbolId collision the semantic phase
                // failed to disambiguate. Fail loud.
                unsupported(decl, std::format(
                    "HIR ExternFunction (id {}) — SymbolId #{} is "
                    "already declared by a prior ExternFunction in "
                    "this module.", decl.v, sym.v));
                continue;
            }
            FfiMetadata const* meta = (ffiMap != nullptr)
                ? ffiMap->tryGet(decl)
                : nullptr;
            // FfiMetadata population is the caller's responsibility:
            // - CST→HIR (today, cycle 2d): not yet populating;
            //   tests build the FFI map manually until plan 11 FF5
            //   lands the c-subset attribution syntax.
            // - FF5 binary ingestion: populates from .so/.dll
            //   reads.
            // - Missing/empty `mangledName` (the linker-visible
            //   symbol name) is a hard error: every extern MUST
            //   carry a name the linker can resolve against.
            // - Missing/empty `importLibrary` is a hard error too:
            //   without a library path the linker can't emit a
            //   DT_NEEDED / LC_LOAD_DYLIB / IMAGE_IMPORT_DESCRIPTOR
            //   entry — the linker's per-extern validation
            //   (`linker.cpp`) already enforces this, but we fail
            //   loud here so the diagnostic anchors at the HIR
            //   node (with its source span) rather than at the
            //   linker (where the span has been lost).
            if (meta == nullptr || meta->mangledName.empty()) {
                unsupported(decl, std::format(
                    "HIR ExternFunction (id {}) — `mangledName` is "
                    "missing from the HirAttribute<FfiMetadata> "
                    "side-table. Every extern symbol must carry a "
                    "non-empty mangled name (the linker's import "
                    "table key). Plan 11 FF5 populates this from "
                    "binary ingestion; tests must attach the "
                    "attribute manually until then.", decl.v));
                continue;
            }
            // c86 (D-CSUBSET-BARE-PROTO-EXTERN-SYNTHESIS): a
            // `noLibraryBinding` extern is EXEMPT from the library
            // requirement — a bare-prototype cross-TU reference carries
            // an EMPTY libraryPath on purpose. The LK11 merge resolves
            // it against a sibling TU's definition (row stripped, calls
            // direct); an unresolved survivor is rejected LOUD at the
            // link tier as an undefined symbol (K_SymbolUndefined naming
            // the symbol). Every OTHER producer keeps the hard contract.
            if (meta->importLibrary.empty() && !meta->noLibraryBinding) {
                unsupported(decl, std::format(
                    "HIR ExternFunction (id {}) — `importLibrary` "
                    "is missing from the HirAttribute<FfiMetadata> "
                    "side-table. Every extern symbol must declare "
                    "the dynamic library that owns it (e.g. "
                    "'libc.so.6', 'kernel32.dll', "
                    "'/usr/lib/libSystem.B.dylib'). Without a "
                    "library path the linker cannot emit the "
                    "corresponding DT_NEEDED / LC_LOAD_DYLIB / "
                    "IMAGE_IMPORT_DESCRIPTOR entry.", decl.v));
                continue;
            }
            ExternImport row;
            row.symbol      = sym;
            row.mangledName = meta->mangledName;
            row.libraryPath = meta->importLibrary;
            externImports.push_back(std::move(row));
            functionSymbols.insert(sym.v);
        }
    }

    // Pre-pass: collect the set of module-level global symbols so that a
    // `Ref` to a global resolves to a `GlobalAddr`-then-`Load` (rvalue) or
    // a `GlobalAddr` (lvalue address). Mirrors `collectFunctions`. The
    // actual `addGlobal` is deferred to `emitGlobals_` (called after all
    // functions are lowered) so non-constant initializers can route through
    // a synthesized module-init function.
    void collectGlobals(HirNodeId moduleNode) {
        for (HirNodeId decl : hir.moduleDecls(moduleNode)) {
            if (hir.kind(decl) != HirKind::Global) continue;
            SymbolId const sym = hir.globalSymbol(decl);
            if (!sym.valid()) continue;
            globalSymbols.insert(sym.v);
            // TLS C1 (★CRIT-1): register thread-local globals BEFORE
            // `classifyGlobals` walks any initializer, so the
            // symbol-address classification can reject `&tls` as a
            // static initializer regardless of declaration order.
            if (threadLocalMap != nullptr) {
                if (auto const* p = threadLocalMap->tryGet(decl);
                    p != nullptr && p->isThreadLocal) {
                    threadLocalTargetSymbols.insert(sym.v);
                }
            }
        }
    }


    // Per-global emission record built during the pre-pass and consumed by
    // `emitGlobals_` after all functions are lowered.
    struct PendingGlobal {
        SymbolId  symbol;
        TypeId    type;
        // Mutually exclusive — exactly one set:
        std::optional<MirLiteralValue> constInit;   // foldable literal
        HirNodeId                      runtimeInit; // non-constant init expr
        // Both unset → zero-init (no `=` in the declaration).
        // D-CSUBSET-LINKAGE-SPECIFIERS: declared binding/visibility (default
        // Global/Default ⇒ externally visible). Carried so emitGlobals stamps a
        // `static`/`__attribute__` global's linkage onto the MirGlobal.
        LinkageAttr                    linkage{};
        // D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL: declared const-ness (default false
        // ⇒ mutable ⇒ writable `.data`/`.bss`). Stamped onto MirGlobal.isConst so
        // the assembler routes an initialized const global to read-only `.rodata`.
        bool                           isConst = false;
        // c21 (D-CSUBSET-VOLATILE-QUALIFIER): declared volatility (default false).
        // When set AND the global has a runtime initializer, its load-time init
        // Store carries MirInstFlags::Volatile.
        bool                           isVolatile = false;
        // TLS C1 (D-CSUBSET-THREAD-LOCAL): declared thread storage duration
        // (default false ⇒ ordinary process-shared). Stamped onto
        // MirGlobal.isThreadLocal so the assembler routes the item to the
        // thread-template sections (`.tdata`/`.tbss`) — checked BEFORE
        // isConst there (a `const thread_local` is per-thread first).
        bool                           isThreadLocal = false;
        // D-CSUBSET-ALIGNAS-VARIABLE-CODEGEN: declared explicit `alignas(N)`
        // alignment in bytes (0 ⇒ none). Stamped onto MirGlobal.alignment so the
        // assembler raises the emitted data item's section alignment.
        std::uint32_t                  explicitAlignment = 0;
        // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): init = the LINK-TIME-CONSTANT
        // address of `symbolAddrInit` (+ addend) — a string-literal rodata global,
        // another global, or a function. Routes to a MirSymbolAddrValue literal
        // (an abs64 relocation), NOT __module_init__. Mutually exclusive with
        // constInit / runtimeInit above.
        std::optional<SymbolId>        symbolAddrInit;
        std::int64_t                   symbolAddrAddend = 0;
    };
    std::vector<PendingGlobal> pendingGlobals;

    // F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): recognize a LINK-TIME-CONSTANT
    // symbol-address global initializer that const-eval cannot fold — a global
    // pointer initialized to another symbol's ADDRESS. Two shapes:
    //   `int* p = &x;`     → AddressOf(Ref(global-or-function)) → that symbol
    //   `char* g = "...";` → Cast(Literal(string), Ptr<Char>)   → a freshly minted
    //                        rodata string global (pushed to pendingGlobals so
    //                        emitGlobals_ emits its bytes; the pointer's reloc
    //                        targets it)
    // Returns {targetSymbol, addend} or nullopt (the caller falls back to
    // const-eval / runtime-init). mintSyntheticGlobalSymbol is lazy-seeded after
    // collect*, so minting here is safe; pushing the rodata PendingGlobal mid-
    // classify is safe (the classify loop walks moduleDecls, not pendingGlobals).
    //
    // TLS C1 (★CRIT-1, D-CSUBSET-THREAD-LOCAL): callers use the
    // `tryClassifyAsSymbolAddr` WRAPPER below — it screens every classified
    // target against `threadLocalTargetSymbols` (C11 6.6p9: a thread-local
    // object's address is NOT an address constant) so no MirSymbolAddrValue
    // targeting TLS is ever minted silently. This Impl recurses to ITSELF
    // (the Cast-peel arm) so one initializer emits at most ONE diagnostic.
    [[nodiscard]] std::optional<std::pair<SymbolId, std::int64_t>>
    tryClassifyAsSymbolAddrImpl(HirNodeId initNode, EvalEnvironment const& env,
                                EvalOptions const& opts) {
        HirKind const k = hir.kind(initNode);
        // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): a bare `Ref` to a
        // FUNCTION symbol is a function designator that has decayed to its
        // ADDRESS (C 6.3.2.1p4 — the value of a function designator is a
        // pointer to the function), e.g. the `posixOpen` / `f` member of a
        // function-pointer table. Its link-time value IS the function's
        // address. A bare `Ref` to a GLOBAL VARIABLE is excluded: that is an
        // rvalue LOAD of the variable's contents, not its address (`&global`
        // arrives as AddressOf(Ref) below). Only the FUNCTION designator
        // decays designator→address here.
        if (k == HirKind::Ref) {
            std::uint32_t const s = hir.payload(initNode);
            if (functionSymbols.contains(s))
                return std::make_pair(SymbolId{s}, std::int64_t{0});
            // c68 (D-CSUBSET-AGGREGATE-GLOBAL-NONSYMBOL-PTR-MEMBER): a bare `Ref`
            // to a GLOBAL ARRAY variable is an array designator that DECAYS to
            // `&arr[0]` (C 6.3.2.1p3) — a link-time symbol address, addend 0
            // (sqlite's `aWindowFuncs[].zName = row_numberName`, a `Ref` to a
            // `static const char[]` global, wrapped in the decay Cast peeled by
            // the Cast arm below). A bare `Ref` to a SCALAR global stays excluded
            // (that is an rvalue LOAD of the variable's contents, NOT its address
            // — `&global` arrives as AddressOf(Ref)); ONLY the array-to-pointer
            // designator decay yields an address here.
            if (globalSymbols.contains(s)) {
                TypeId const rt = hir.typeId(initNode);
                if (rt.valid() && interner.kind(rt) == TypeKind::Array)
                    return std::make_pair(SymbolId{s}, std::int64_t{0});
            }
            return std::nullopt;
        }
        if (k == HirKind::AddressOf) {
            // c80 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-BASE-INDEX): the operand
            // is a CONSTANT LVALUE PATH rooted at a global — the bare
            // `&global` (the original F5 arm: path = a lone Ref), or the
            // symbol-base element/field address `&arr[K]` / `&s.field`
            // (AddressOf(Index/MemberAccess chain)) → {rootSym, byteOffset}.
            // The path resolver owns the constant-address rules; a
            // non-constant path (a local, a pointer-typed base, a runtime
            // index) yields nullopt exactly as the old Ref-only arm did.
            auto kids = hir.children(initNode);
            if (kids.size() == 1)
                return tryResolveConstLvaluePath(kids[0], env, opts);
            return std::nullopt;
        }
        if (k == HirKind::Cast) {
            auto kids = hir.children(initNode);
            if (kids.size() != 1) return std::nullopt;
            TypeId const ct = hir.typeId(initNode);
            if (!ct.valid() || interner.kind(ct) != TypeKind::Ptr)
                return std::nullopt;
            // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): a POINTER-typed
            // cast WRAPPING another symbol-address — `(void*)&x`, the
            // `(sqlite3_syscall_ptr)posixOpen` member of aSyscall, any
            // pointer-to-pointer reinterpret. A reinterpret between pointer
            // types preserves the bit pattern, so the cast IS the same
            // link-time address (+ addend); peel it and recurse. (Also covers
            // the scalar top-level `void* p = (void*)&x;` form — same anchor.)
            // The result type is already gated to Ptr above, so a cast that
            // changes representation — `(long)&x` — never reaches here.
            if (hir.kind(kids[0]) != HirKind::Literal)
                return tryClassifyAsSymbolAddrImpl(kids[0], env, opts);
            std::uint32_t const litIdx0 = hir.payload(kids[0]);
            HirLiteralValue const& src = literals.at(litIdx0);
            if (!std::holds_alternative<std::string>(src.value))
                return std::nullopt;
            // FC17.5 F2 (code-audit MEDIUM-1): route through the SHARED memoized
            // producer core — a `static const char *p = __func__;` initializer and
            // a body read of `__func__` must reference the SAME rodata global
            // (C99 6.4.2.2 identity), and identical plain string literals dedup
            // here too (C 6.4.5p7 permits sharing). The prior per-occurrence
            // PendingGlobal mint broke the identity across positions.
            SymbolId const rodataSym = internStringLiteralGlobal(kids[0]);
            if (!rodataSym.valid()) return std::nullopt;  // exhausted → fall back
            return std::make_pair(rodataSym, std::int64_t{0});
        }
        return std::nullopt;
    }

    // TLS C1 (★CRIT-1, D-CSUBSET-THREAD-LOCAL): fail loud when a
    // STATIC-storage-duration initializer names a thread-local object's
    // address (C11 6.6p9 — thread storage ≠ static storage, so `&tls` is not
    // an address constant). Without this, the classified target would mint a
    // MirSymbolAddrValue whose abs64 relocation the walker resolves through
    // the SAME symbol-VA map the TLS layout poisons with signed tpoffs — a
    // silent garbage pointer in `.data`. A pointer INSIDE a thread_local
    // aggregate targeting a NON-TLS symbol is legal and never fires here
    // (the screen keys on the TARGET, not the initialized global).
    void rejectThreadLocalAddressTarget(SymbolId target, HirNodeId at) {
        if (!threadLocalTargetSymbols.contains(target.v)) return;
        ParseDiagnostic d;
        d.code     = DiagnosticCode::S_ThreadLocalAddressNotConstant;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = "the address of a thread-local object is not an address "
                     "constant (C 6.6p9) — a static-storage initializer cannot "
                     "name it; initialize the pointer at runtime instead";
        if (sourceMap != nullptr) {
            if (auto const* loc = sourceMap->tryGet(at); loc != nullptr) {
                d.buffer = loc->buffer;
                d.span   = loc->span;
            }
        }
        reporter.report(std::move(d));
    }

    // TLS C1 (★CRIT-1): the screened entry point EVERY symbol-address
    // classification consumer calls — the scalar classify loop and the
    // aggregate member loop both mint MirSymbolAddrValue from this result,
    // so the screen here covers every mint site by construction. The
    // classification is still RETURNED on a reject (the module stays
    // walkable — the Error gates the compile), matching the
    // abort-resilience discipline emitGlobals_ documents.
    [[nodiscard]] std::optional<std::pair<SymbolId, std::int64_t>>
    tryClassifyAsSymbolAddr(HirNodeId initNode, EvalEnvironment const& env,
                            EvalOptions const& opts) {
        auto const r = tryClassifyAsSymbolAddrImpl(initNode, env, opts);
        if (r.has_value()) rejectThreadLocalAddressTarget(r->first, initNode);
        return r;
    }

    // TLS C1 (★CRIT-1, belt over the fold path): screen a CONST-EVAL-FOLDED
    // literal for symbol-address leaves targeting a thread-local object —
    // the third producer of MirSymbolAddrValue (toMirLiteral's
    // HirAddressValue arm, fed by evaluateConstant's c43 address folds)
    // bypasses tryClassifyAsSymbolAddr entirely. Called on the two fold
    // outputs that become static data (the scalar constInit fold and the
    // aggregate member fold). Recursive over aggregate arms; scalar leaves
    // are O(1).
    void rejectTlsAddressesInFoldedLiteral(MirLiteralValue const& v,
                                           HirNodeId at) {
        if (auto const* sa = std::get_if<MirSymbolAddrValue>(&v.value)) {
            rejectThreadLocalAddressTarget(SymbolId{sa->symbol}, at);
            return;
        }
        if (auto const* agg = std::get_if<MirAggregateValue>(&v.value)) {
            for (auto const& f : agg->fields)
                rejectTlsAddressesInFoldedLiteral(f, at);
        }
    }

    // c80 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-BASE-INDEX): resolve a CONSTANT
    // LVALUE PATH rooted at a module global — the operand of a global
    // initializer's `&…` — to {rootSymbol, byteOffset}. Shapes:
    //   Ref(global|function)          → {sym, 0}   (the F5/c67 base arm)
    //   Index(arrayPath, constIdx)    → {sym, off + constIdx * elementStride}
    //   MemberAccess(recordPath, .f)  → {sym, off + fieldByteOffset(record, f)}
    // Canonical sqlite shape (sqlite3.c:24077): `const unsigned char
    // *sqlite3aLTb = &sqlite3UpperToLower[256-OP_Ne];` — an ADDRESS CONSTANT
    // (C 6.6p9): gcc emits `.quad sqlite3UpperToLower+203` (an abs64 reloc
    // with an addend), never a runtime store. c67's encoder already threads
    // the addend, so RECOGNIZING the shape is the whole fix.
    // ★ CONSERVATIVE (the c65/c68 no-over-fire discipline):
    //   - an Index base must be ARRAY-typed: indexing THROUGH a pointer-typed
    //     global (`&ptrGlobal[3]`) reads the pointer's RUNTIME VALUE — not an
    //     address constant (gcc rejects it as a static initializer too) →
    //     nullopt (stays fail-loud);
    //   - a MemberAccess base must be Struct/Union-typed: a `p->f` deref base
    //     arrives as Deref — no arm matches → nullopt;
    //   - the index must fold under the SAME const-eval policy the classify
    //     loop uses (env/opts threaded from it).
    // Offsets accumulate SIGNED (`&arr[i-j]` with i<j is a negative addend;
    // the Relocation addend is int64). Nested paths (`&s.arr[K]`,
    // `&arr[K].field`, `&m[i][j]`) compose by recursion.
    [[nodiscard]] std::optional<std::pair<SymbolId, std::int64_t>>
    tryResolveConstLvaluePath(HirNodeId n, EvalEnvironment const& env,
                              EvalOptions const& opts) {
        HirKind const k = hir.kind(n);
        if (k == HirKind::Ref) {
            std::uint32_t const s = hir.payload(n);
            if (globalSymbols.contains(s) || functionSymbols.contains(s))
                return std::make_pair(SymbolId{s}, std::int64_t{0});
            return std::nullopt;
        }
        if (k == HirKind::Index) {
            auto kids = hir.children(n);
            if (kids.size() != 2) return std::nullopt;
            TypeId const baseTy = hir.typeId(kids[0]);
            if (!baseTy.valid() || interner.kind(baseTy) != TypeKind::Array)
                return std::nullopt;
            auto const base = tryResolveConstLvaluePath(kids[0], env, opts);
            if (!base.has_value()) return std::nullopt;
            ConstEvalResult const ir =
                evaluateConstant(hir, interner, literals, kids[1], env, opts);
            if (!ir.value.has_value()) return std::nullopt;
            std::int64_t idxVal = 0;
            if (std::holds_alternative<std::int64_t>(ir.value->value))
                idxVal = std::get<std::int64_t>(ir.value->value);
            else if (std::holds_alternative<std::uint64_t>(ir.value->value))
                idxVal = static_cast<std::int64_t>(
                    std::get<std::uint64_t>(ir.value->value));
            else
                return std::nullopt;
            // Stride = the element type's layout size (the Index node's own
            // type IS the element type) — the SAME `elementStride` engine the
            // runtime Gep path scales with, so the folded address equals the
            // address the program would compute.
            auto const stride = elementStride(hir.typeId(n));
            if (!stride.has_value()) return std::nullopt;
            return std::make_pair(
                base->first,
                base->second + idxVal * static_cast<std::int64_t>(*stride));
        }
        if (k == HirKind::MemberAccess) {
            auto kids = hir.children(n);
            if (kids.size() != 1) return std::nullopt;
            TypeId const baseTy = hir.typeId(kids[0]);
            if (!baseTy.valid()) return std::nullopt;
            TypeKind const btk = interner.kind(baseTy);
            if (btk != TypeKind::Struct && btk != TypeKind::Union)
                return std::nullopt;
            auto const base = tryResolveConstLvaluePath(kids[0], env, opts);
            if (!base.has_value()) return std::nullopt;
            // The FC7 field-offset engine (combineMemberAddr's authority) —
            // folded field addresses match runtime member access exactly.
            auto const off = fieldByteOffset(baseTy, hir.payload(n));
            if (!off.has_value()) return std::nullopt;
            return std::make_pair(
                base->first,
                base->second + static_cast<std::int64_t>(*off));
        }
        return std::nullopt;
    }

    // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): recognize a file-scope
    // AGGREGATE global whose initializer carries a LINK-TIME-CONSTANT member —
    // a function address, `&global`, or a string literal nested in a struct /
    // union / array (canonical: sqlite's `static struct unix_syscall aSyscall[]
    // = {{"open",(sqlite3_syscall_ptr)posixOpen,0},…}`). const-eval cannot fold
    // such a member (the address is not known until link), so the whole
    // aggregate would otherwise fall to runtimeInit and trip the
    // bitfield-rvalue fail-loud guard. This is the AGGREGATE generalization of
    // the F5 scalar mechanism (`tryClassifyAsSymbolAddr`, D-CSUBSET-SYMBOL-
    // ADDRESS-GLOBAL): emit STATIC DATA with abs64 relocations at the member
    // offsets (the C-correct, gcc-matching placement) instead of a
    // __module_init__ store-chain.
    //
    // Build a `MirAggregateValue` whose `fields` pair 1:1 with the
    // ConstructAggregate's POSITIONAL children (zero-fills already normalized at
    // HIR lowering — same discipline the const-eval ConstructAggregate arm and
    // `encodeAggregateValue` rely on). Each child resolves by trying, in order:
    //   (a) `tryClassifyAsSymbolAddr` → a reloc-bearing pointer leaf (a string
    //       member mints + pushes its own rodata PendingGlobal, the F5 path);
    //   (b) a NESTED `tryClassifyAggregateConst` (recurse — struct-in-struct /
    //       array-of-struct like aSyscall);
    //   (c) `evaluateConstant` → an ordinary folded leaf (plain `0`, ints, …).
    // If ANY member resolves by none → nullopt (the whole aggregate falls back
    // to runtimeInit — never partially classify). `env`/`opts` are threaded
    // from the classify loop's locals (the SAME const-eval policy globals use).
    //   (d) a NULL POINTER CONSTANT in a pointer member — `(void*)0`, the
    //       trailing `0` of an aSyscall row, `int(*fn)(void) = 0`. const-eval
    //       leaves a cast-to-pointer un-folded ("pointer targets remain
    //       non-foldable"), so peel the pointer-typed Cast, fold its INTEGER
    //       operand, and — iff it is 0 — emit a zero pointer leaf (8 zero bytes,
    //       NO relocation; the encoder's pre-zeroed slot already holds them).
    [[nodiscard]] std::optional<MirLiteralValue>
    tryClassifyAggregateConst(HirNodeId initNode, EvalEnvironment const& env,
                              EvalOptions const& opts) {
        if (hir.kind(initNode) != HirKind::ConstructAggregate)
            return std::nullopt;
        MirAggregateValue agg;
        auto kids = hir.children(initNode);
        agg.fields.reserve(kids.size());
        for (HirNodeId child : kids) {
            if (auto sa = tryClassifyAsSymbolAddr(child, env, opts)) {
                MirLiteralValue leaf;
                leaf.value = MirSymbolAddrValue{sa->first.v, sa->second};
                leaf.core  = TypeKind::Ptr;
                agg.fields.push_back(std::move(leaf));
                continue;
            }
            if (auto nested = tryClassifyAggregateConst(child, env, opts)) {
                agg.fields.push_back(std::move(*nested));
                continue;
            }
            if (auto np = tryClassifyNullPointerConst(child, env, opts)) {
                agg.fields.push_back(std::move(*np));
                continue;
            }
            if (auto ip = tryClassifyNullBaseIndexConst(child, env, opts)) {
                agg.fields.push_back(std::move(*ip));
                continue;
            }
            ConstEvalResult const r =
                evaluateConstant(hir, interner, literals, child, env, opts);
            if (!r.value.has_value()) return std::nullopt;  // one un-foldable → bail
            MirLiteralValue folded = toMirLiteral(*r.value);
            // TLS C1 (★CRIT-1): the fold path can carry a c43 address
            // constant — screen it for TLS targets before it becomes a
            // static-data leaf.
            rejectTlsAddressesInFoldedLiteral(folded, child);
            agg.fields.push_back(std::move(folded));
        }
        MirLiteralValue out;
        out.value = std::move(agg);
        out.core  = interner.kind(hir.typeId(initNode));  // Struct / Union / Array
        return out;
    }

    // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): a NULL POINTER CONSTANT
    // member — an integer constant expression equal to 0 in a POINTER context
    // (`(void*)0`, `int(*fn)(void) = 0`, the `0` member of an aSyscall row).
    // const-eval refuses a cast-to-pointer ("pointer/aggregate targets remain
    // non-foldable", const_eval.cpp), so peel the pointer-typed Cast and fold
    // its INTEGER operand. iff that operand folds to 0, the member is a null
    // pointer → a zero pointer leaf (`uint64_t 0`, core=Ptr); the encoder's
    // scalar-leaf arm writes 8 zero bytes with NO relocation. A non-zero or
    // non-integer operand (`(void*)0x1000`, a runtime expr) yields nullopt so
    // the caller's evaluateConstant fallback / whole-aggregate bail still
    // governs — this arm ONLY recognizes the standard null pointer constant.
    [[nodiscard]] std::optional<MirLiteralValue>
    tryClassifyNullPointerConst(HirNodeId node, EvalEnvironment const& env,
                                EvalOptions const& opts) {
        TypeId const ty = hir.typeId(node);
        if (!ty.valid() || interner.kind(ty) != TypeKind::Ptr)
            return std::nullopt;
        HirNodeId operand = node;
        if (hir.kind(node) == HirKind::Cast) {
            auto kids = hir.children(node);
            if (kids.size() != 1) return std::nullopt;
            operand = kids[0];   // fold the integer behind the pointer cast
        }
        ConstEvalResult const r =
            evaluateConstant(hir, interner, literals, operand, env, opts);
        if (!r.value.has_value()) return std::nullopt;
        bool isZero = false;
        if (std::holds_alternative<std::int64_t>(r.value->value))
            isZero = std::get<std::int64_t>(r.value->value) == 0;
        else if (std::holds_alternative<std::uint64_t>(r.value->value))
            isZero = std::get<std::uint64_t>(r.value->value) == 0;
        if (!isZero) return std::nullopt;
        MirLiteralValue leaf;
        leaf.value = std::uint64_t{0};
        leaf.core  = TypeKind::Ptr;
        return leaf;
    }

    // c68 (D-CSUBSET-AGGREGATE-GLOBAL-NONSYMBOL-PTR-MEMBER): a NULL-BASE ARRAY-
    // ELEMENT address constant — `(T*)&((char*)0)[X]`. This is sqlite's
    // `SQLITE_INT_TO_PTR(X)` idiom (sqlite3.c: `#define SQLITE_INT_TO_PTR(X)
    // ((void*)&((char*)0)[X])`): stash a small integer X in a `void*` (read back
    // via `SQLITE_PTR_TO_INT`), used for the `pUserData` member of the built-in
    // `FuncDef` tables (`aBuiltinFunc[]`, `aJsonFunc[]`). The address of element
    // X of a NULL pointer base is `0 + X*sizeof(elem)` — a pointer-valued INTEGER
    // constant: NO symbol, NO relocation (gcc folds it to the same bytes). This
    // is the array-element sibling of c43's offsetof MEMBER folding
    // ([[D-CSUBSET-ADDRESS-CONSTANT-FOLD]]); the CST const-eval deliberately
    // punted the `&arr[i]` Index form to the global-init lowering ("the HIR
    // engine", cst_const_eval.cpp Index comment) — this is that handler. Shape:
    // peel pointer reinterpret cast(s) → AddressOf → Index(base, X).
    // ★ CONSERVATIVE (no over-fire, the c65 discipline): the base MUST fold to a
    // NULL pointer (address 0). `&realArray[i]` (a SYMBOL base) is NOT this idiom
    // — its value is the symbol's address + i*stride (a reloc), NOT the bare
    // integer — so a non-null base returns nullopt (the member falls through to
    // the whole-aggregate bail → the fail-loud guard) rather than being silently
    // mis-folded to an integer.
    [[nodiscard]] std::optional<MirLiteralValue>
    tryClassifyNullBaseIndexConst(HirNodeId node, EvalEnvironment const& env,
                                  EvalOptions const& opts) {
        // Peel pointer-typed reinterpret cast(s) — the outer `(void*)`.
        while (hir.kind(node) == HirKind::Cast) {
            TypeId const ct = hir.typeId(node);
            if (!ct.valid() || interner.kind(ct) != TypeKind::Ptr)
                return std::nullopt;
            auto cKids = hir.children(node);
            if (cKids.size() != 1) return std::nullopt;
            node = cKids[0];
        }
        if (hir.kind(node) != HirKind::AddressOf) return std::nullopt;
        auto aoKids = hir.children(node);
        if (aoKids.size() != 1) return std::nullopt;
        HirNodeId const idxNode = aoKids[0];
        if (hir.kind(idxNode) != HirKind::Index) return std::nullopt;
        auto ixKids = hir.children(idxNode);
        if (ixKids.size() != 2) return std::nullopt;
        // The base MUST be a null pointer constant (address 0) — only then is
        // the element address the bare integer X*stride with no symbol/reloc.
        if (!tryClassifyNullPointerConst(ixKids[0], env, opts)) return std::nullopt;
        // Fold the index to an integer.
        ConstEvalResult const ir =
            evaluateConstant(hir, interner, literals, ixKids[1], env, opts);
        if (!ir.value.has_value()) return std::nullopt;
        std::int64_t idxVal = 0;
        if (std::holds_alternative<std::int64_t>(ir.value->value))
            idxVal = std::get<std::int64_t>(ir.value->value);
        else if (std::holds_alternative<std::uint64_t>(ir.value->value))
            idxVal = static_cast<std::int64_t>(
                std::get<std::uint64_t>(ir.value->value));
        else
            return std::nullopt;
        // Stride = sizeof(element). The Index node's type IS the element type
        // (Subscript: type is the element type).
        TypeId const elemTy = hir.typeId(idxNode);
        auto const layout = computeLayout(elemTy, interner,
                                          config.aggregateLayout, config.dataModel);
        if (!layout) return std::nullopt;
        std::uint64_t const value = static_cast<std::uint64_t>(idxVal)
                                  * static_cast<std::uint64_t>(layout->size);
        MirLiteralValue leaf;
        leaf.value = value;
        leaf.core  = TypeKind::Ptr;
        return leaf;
    }


    // Classify each module-level global into pendingGlobals. Called after
    // `collectGlobals` (so `globalSymbols` is already populated for any
    // function body that refers to globals during lowering).
    //
    // CE2 wire-up: a Ref to ANOTHER module global resolves transitively
    // via a per-call resolver closure. `int a = 1; int b = a;` folds to
    // `b = 1` (both as constant-init globals). The resolver is keyed on
    // a pre-pass map from `SymbolId.v` to the global's HIR init
    // expression — engine cycle-safety handles `a = b; b = a;` style
    // pathologies.
    void classifyGlobals(HirNodeId moduleNode) {
        // Pre-pass: build symbol → init-expr map for ALL globals first,
        // so a global initializer that forward-references a later global
        // still resolves.
        std::unordered_map<std::uint32_t, HirNodeId> initBySymbol;
        for (HirNodeId decl : hir.moduleDecls(moduleNode)) {
            if (hir.kind(decl) != HirKind::Global) continue;
            SymbolId const sym = hir.globalSymbol(decl);
            if (!sym.valid()) continue;
            if (auto initN = hir.globalInit(decl); initN.has_value()) {
                initBySymbol[sym.v] = *initN;
            }
        }
        EvalEnvironment env;
        env.resolveConstSymbol = [&initBySymbol](SymbolId s)
                -> std::optional<HirNodeId> {
            if (auto it = initBySymbol.find(s.v); it != initBySymbol.end()) {
                return it->second;
            }
            return std::nullopt;
        };
        // FC6 deferral-close: let a global initializer `int g = sizeof(T)` fold
        // through the SAME `computeLayout` engine the dedicated MIR SizeOf case
        // uses. `config` carries the target's layout params; absent block ⇒
        // nullopt ⇒ the engine surfaces `NotAConstantExpression` (fail loud).
        env.resolveTypeSize = [&](TypeId t) -> std::optional<std::uint64_t> {
            if (!config.aggregateLayoutLoaded) return std::nullopt;
            auto const layout = computeLayout(t, interner, config.aggregateLayout,
                                              config.dataModel);
            if (!layout) return std::nullopt;
            return layout->size;
        };
        // C11/C23 6.5.3.4 (parallel to resolveTypeSize): let a global initializer
        // `int g = _Alignof(T)` fold through the SAME `computeLayout` engine the
        // dedicated MIR AlignOf case uses — reading alignment instead of size.
        // Absent block ⇒ nullopt ⇒ the engine surfaces NotAConstantExpression.
        env.resolveTypeAlign = [&](TypeId t) -> std::optional<std::uint64_t> {
            if (!config.aggregateLayoutLoaded) return std::nullopt;
            auto const layout = computeLayout(t, interner, config.aggregateLayout,
                                              config.dataModel);
            if (!layout) return std::nullopt;
            return layout->align.bytes();
        };
        EvalOptions opts;
        // MIR-globals matches runtime behaviour: a narrowing initializer
        // wraps modularly (the runtime path would wrap too). Refusing to
        // fold here would only lose an optimization; the value installed
        // at module load is identical either way. D5.5 enum-bounds will
        // flip this back to `true` to surface the overflow as a verifier
        // diagnostic — same engine, different policy per consumer.
        opts.refuseOnOverflow = false;
        // Plan 12.5 §0.2 D3 closed: float folding is now schema-driven.
        // Every v1 schema is IEEE 754 (default `true`); a non-IEEE-float
        // schema (decimal float, fixed-point, saturating) declares
        // `hirLowering.globalsConstEval.allowFloat: false` and the
        // engine refuses to fold its float arithmetic at module-load.
        opts.allowFloat = config.globalsAllowFloat;
        for (HirNodeId decl : hir.moduleDecls(moduleNode)) {
            if (hir.kind(decl) != HirKind::Global) continue;
            PendingGlobal pg;
            pg.symbol = hir.globalSymbol(decl);
            pg.type   = hir.globalType(decl);
            if (linkageMap != nullptr)
                if (auto const* p = linkageMap->tryGet(decl)) pg.linkage = *p;
            if (mutabilityMap != nullptr)
                if (auto const* p = mutabilityMap->tryGet(decl)) pg.isConst = p->isConst;
            if (threadLocalMap != nullptr)   // TLS C1 — the isConst mirror
                if (auto const* p = threadLocalMap->tryGet(decl))
                    pg.isThreadLocal = p->isThreadLocal;
            if (volatileMap != nullptr)
                if (auto const* p = volatileMap->tryGet(decl)) pg.isVolatile = p->isVolatile;
            if (alignmentMap != nullptr)
                if (auto const* p = alignmentMap->tryGet(decl))
                    pg.explicitAlignment = p->alignmentBytes;
            if (auto initN = hir.globalInit(decl); initN.has_value()) {
                // F5: a symbol-ADDRESS initializer (`int* p = &x;`, `char* g =
                // "...";`) is a LINK-TIME constant const-eval cannot fold —
                // recognize it FIRST (and route to a MirSymbolAddrValue / abs64
                // reloc) before falling back to const-eval / runtime-init.
                if (auto sa = tryClassifyAsSymbolAddr(*initN, env, opts)) {
                    pg.symbolAddrInit   = sa->first;
                    pg.symbolAddrAddend = sa->second;
                } else {
                    // The resolver covers Refs to sibling globals; literal /
                    // arithmetic / Cast paths still fold per CE1.
                    ConstEvalResult const r = evaluateConstant(
                        hir, interner, literals, *initN, env, opts);
                    if (r.value.has_value()) {
                        pg.constInit = toMirLiteral(*r.value);
                        // TLS C1 (★CRIT-1): screen the c43 address-fold arm
                        // — the one MirSymbolAddrValue producer that never
                        // passes through tryClassifyAsSymbolAddr.
                        rejectTlsAddressesInFoldedLiteral(*pg.constInit,
                                                          *initN);
                    } else if (auto aggC =
                                   tryClassifyAggregateConst(*initN, env, opts)) {
                        // c67 (D-CSUBSET-AGGREGATE-GLOBAL-SYMBOL-ADDRESS): an
                        // aggregate whose initializer has a link-time-constant
                        // member (a fn/`&global`/string address) — const-eval
                        // can't fold the address, but the aggregate is still
                        // STATIC DATA (reloc leaves at the member offsets), NOT
                        // a runtime store-chain. Routes to constInit (the
                        // assembler's aggregate arm encodes the relocs) instead
                        // of runtimeInit. A fully-foldable aggregate already
                        // folded above; only the address-bearing case reaches
                        // here.
                        pg.constInit = std::move(*aggC);
                    } else if (auto np = tryClassifyNullPointerConst(*initN,
                                                                    env, opts)) {
                        // c80: a TOP-LEVEL scalar NULL POINTER CONSTANT —
                        // `T* g = 0;` (sqlite's `vfsList`/`unixBigLock`/
                        // `inodeList`/`sqlite3SharedCacheList`/
                        // `sqlite3_temp_directory`/`sqlite3_data_directory`).
                        // const-eval refuses a cast-to-pointer ("pointer
                        // targets remain non-foldable"), and c67 wired the
                        // null-pointer-constant recognizer only into the
                        // AGGREGATE member loop — a bare scalar pointer
                        // global fell to runtimeInit → the asm fail-loud.
                        // Same classifier, same order as the member loop.
                        pg.constInit = std::move(*np);
                    } else if (auto ip = tryClassifyNullBaseIndexConst(*initN,
                                                                      env, opts)) {
                        // c80: the TOP-LEVEL scalar sibling of c68's member
                        // arm — `void* g = SQLITE_INT_TO_PTR(X)` =
                        // `(void*)&((char*)0)[X]` at file scope: a pointer-
                        // valued INTEGER constant (no symbol, no reloc).
                        pg.constInit = std::move(*ip);
                    } else {
                        pg.runtimeInit = *initN;
                    }
                }
            }
            pendingGlobals.push_back(std::move(pg));
        }
    }

    // After all functions are lowered, emit the actual MIR for every global.
    // Discipline: every well-formed global gets its `addGlobal` call FIRST,
    // unconditionally; the init-function body is built second. A failure in
    // ONE runtime-init's expression lowering MUST NOT cause unrelated globals
    // (foldable, zero-init, or other runtime-inits) to be dropped from MIR —
    // the abort-resilience contract requires the partial module remain
    // walkable. On a per-init failure we seal that init's Store-emission
    // with Unreachable and continue with the next; the global itself is
    // still registered.
    //   - constant-init  → literalPool.add + addGlobal(type, sym, litIdx)
    //   - zero-init      → addGlobal(type, sym)
    //   - runtime-init   → synthesize a `__module_init__` MirFunc whose
    //                       body Stores each runtime-init value into its
    //                       global, then `addGlobal(..., initFunc=…)`.
    // The init function is built ONCE; all runtime-init globals share it.
    bool emitGlobals_() {
        if (pendingGlobals.empty()) return true;
        bool anyRuntime = false;
        for (auto const& pg : pendingGlobals) {
            if (pg.runtimeInit.valid()) { anyRuntime = true; break; }
        }
        // Step 1: synthesize the init function (header + entry block) up
        // front so its MirFuncId is known when the addGlobal calls below
        // reference it. The body is filled in step 3.
        MirBlockId initEntry{};
        if (anyRuntime) {
            TypeId const voidTy = interner.primitive(TypeKind::Void);
            std::array<TypeId, 0> noParams{};
            // SysV is the canonical MIR-time placeholder; LIR's calling-
            // convention pass (ML7) maps to the target's real convention.
            TypeId const initSig = interner.fnSig(noParams, voidTy, CallConv::CcSysV);
            moduleInitFunc = mir.addFunction(initSig, SymbolId{});
            initEntry = mir.createBlock(StructCfMarker::EntryBlock);
        }
        // Step 2: addGlobal for every pending entry — unconditional, so
        // an init-failure later doesn't strip unrelated globals from MIR.
        bool ok = true;
        for (auto const& pg : pendingGlobals) {
            if (!pg.type.valid() || !pg.symbol.valid()) {
                unsupported(HirNodeId{}, std::format(
                    "global decl has invalid type/symbol (HIR verifier "
                    "should have flagged this) — symbol {}", pg.symbol.v));
                ok = false;
                continue;
            }
            if (pg.symbolAddrInit.has_value()) {
                // F5: init = link-time-constant symbol address. Emit a
                // MirSymbolAddrValue literal; lowerMirGlobalsToDataItems (asm)
                // emits a pointer slot + an abs64 reloc against the target symbol,
                // NOT a __module_init__ runtime store.
                MirLiteralValue v;
                v.value = MirSymbolAddrValue{pg.symbolAddrInit->v,
                                             pg.symbolAddrAddend};
                v.core  = TypeKind::Ptr;
                std::uint32_t const idx = mir.literalPoolAdd(std::move(v));
                mir.addGlobal(pg.type, pg.symbol, idx, {},
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst, mirThreadStorageOf(pg.isThreadLocal),
                              pg.explicitAlignment);
            } else if (pg.constInit.has_value()) {
                std::uint32_t const idx = mir.literalPoolAdd(*pg.constInit);
                mir.addGlobal(pg.type, pg.symbol, idx, {},
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst, mirThreadStorageOf(pg.isThreadLocal),
                              pg.explicitAlignment);
            } else if (pg.runtimeInit.valid()) {
                mir.addGlobal(pg.type, pg.symbol, UINT32_MAX, moduleInitFunc,
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst, mirThreadStorageOf(pg.isThreadLocal),
                              pg.explicitAlignment);
            } else {
                mir.addGlobal(pg.type, pg.symbol, UINT32_MAX, {},
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst, mirThreadStorageOf(pg.isThreadLocal),
                              pg.explicitAlignment);
            }
        }
        // Step 3: fill the init function's body — Store each runtime
        // initializer into its global. Per-init failures DO NOT seal the
        // block or stop processing; lowerExpr returning InvalidMirInst
        // leaves the open block in a still-Open state (the seal-on-failure
        // discipline is at the STATEMENT tier, not the expression tier),
        // so we skip the failing Store and continue with the next init.
        // Every global was already declared in step 2; this loop only
        // affects whether each global's initial value is actually
        // installed at module load.
        if (anyRuntime) {
            mir.beginBlock(initEntry);
            for (auto const& pg : pendingGlobals) {
                if (!pg.runtimeInit.valid()) continue;
                MirInstId const val = lowerExpr(pg.runtimeInit);
                if (!val.valid()) {
                    // Inner expression lowering already emitted a
                    // diagnostic; the global is declared but its Store
                    // is dropped. Subsequent inits get their chance.
                    ok = false;
                    continue;
                }
                MirInstId const addr = mir.addGlobalAddr(
                    pg.symbol, interner.pointer(pg.type));
                std::array<MirInstId, 2> ops{val, addr};
                // c21 (D-CSUBSET-VOLATILE-QUALIFIER): a `volatile` global's
                // runtime load-time init store carries the flag.
                // FC17.9(d) (D-CSUBSET-ATOMIC): an `_Atomic` global's load-time
                // runtime init is INITIALIZATION (C11 7.17.2.1 — not itself atomic),
                // so it stays a plain Store, marked AtomicInitExempt so the verifier's
                // atomic belt spares it despite the atomic-qualified global pointee.
                mir.addInst(MirOpcode::Store, ops, InvalidType, /*payload=*/0,
                            (pg.isVolatile ? MirInstFlags::Volatile
                                           : MirInstFlags::None)
                                | MirInstFlags::AtomicInitExempt);
            }
            if (!mir.openBlockHasTerminator()) mir.addReturn();
        }
        return ok;
    }

    // Lower the whole module: collect function + global symbols, classify
    // globals into the pending queue, lower each function, then emit globals
    // (constant-init via literal pool, runtime-init via synthesized init
    // function). Non-function / non-global top-level decls emit fail-loud
    // diagnostics — extern declarations are owned by FFI plan 11.
    void lower() {
        HirNodeId const root = hir.root();
        if (!root.valid() || hir.kind(root) != HirKind::Module) {
            unsupported(root, "HIR root is not a Module — cannot lower");
            return;
        }
        // Pre-pass ordering invariant (architect LOW fold, LK6
        // cycle 2d post-fold review): `collectFunctions` MUST run
        // before `collectExterns` because the cross-table guard in
        // `collectExterns` reads `functionSymbols` to detect a
        // SymbolId owned by an intra-module function colliding with
        // an extern declaration. Reordering would silently degrade
        // the guard to "extern wins, prior function shadowed."
        collectFunctions(root);
        collectThreadShimSymbols();   // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER)
        collectGlobals(root);
        collectExterns(root);
        classifyGlobals(root);
        for (HirNodeId decl : hir.moduleDecls(root)) {
            HirKind const dk = hir.kind(decl);
            switch (dk) {
                case HirKind::Function:
                    if (!lowerFunction(decl)) return;
                    break;
                case HirKind::Global:
                    // Already classified into `pendingGlobals` by the
                    // pre-pass; deferred to `emitGlobals_` below so a
                    // synthesized module-init function can be built once
                    // for ALL runtime-init globals.
                    break;
                case HirKind::ExternFunction:
                    // Pre-pass (`collectExterns`) already registered
                    // the SymbolId in `functionSymbols` and pushed
                    // an `ExternImport` row. No MIR instructions
                    // are emitted at the decl site itself — the
                    // declaration is pure metadata that the linker
                    // consumes via `AssembledModule.externImports`.
                    // Call sites referencing this extern emit
                    // `Call GlobalAddr(externSym), args...`, same
                    // as intra-module calls. (LK6 cycle 2d —
                    // D-LK6-6 closure.)
                    break;
                case HirKind::ExternGlobal:
                    // c82 (D-LK-EXTERN-DATA-IMPORT): pre-pass
                    // (`collectExterns`) already registered the SymbolId in
                    // `globalSymbols` and pushed an `ExternImport` row
                    // flagged `isData`. No MIR instructions at the decl
                    // site — a `Ref` lowers through the same
                    // GlobalAddr(+Load) path an intra-module global uses;
                    // the LK11 merge resolves sibling-CU-defined ones, and
                    // the link tier fail-louds on a surviving TRUE library
                    // data import until the binding model lands (§B).
                    break;
                case HirKind::TypeDecl:
                    // TypeDecl is the one structural carrier that genuinely
                    // has no MIR runtime effect — it interns a type into the
                    // lattice but emits no code. Skipping is correct here.
                    break;
                case HirKind::ImportGroup:
                    unsupported(decl, std::format(
                        "HIR ImportGroup (id {}) — import resolution is not "
                        "yet lowered", decl.v));
                    return;
                default:
                    unsupported(decl, std::format(
                        "Top-level HIR kind ordinal {} (id {}) not yet "
                        "supported", static_cast<unsigned>(dk), decl.v));
                    return;
            }
        }
        // Emit deferred globals last: builds the synthesized module-init
        // function (if any runtime-init globals exist) and then `addGlobal`
        // for every pending entry. Done after all real functions so the
        // init function lands at a stable, predictable arena slot.
        (void)emitGlobals_();
    }
};

} // namespace

HirToMirResult lowerToMir(Hir const&               hir,
                          HirLiteralPool const&    literals,
                          TypeInterner&            interner,
                          DiagnosticReporter&      reporter,
                          HirSourceMap const*      sourceMap,
                          MirLoweringConfig const& config,
                          HirFfiMap const*         ffiMap,
                          HirLinkageMap const*     linkageMap,
                          HirMutabilityMap const*  mutabilityMap,
                          HirVolatileMap const*    volatileMap,
                          HirAlignmentMap const*   alignmentMap,
                          HirThreadLocalMap const* threadLocalMap,
                          std::unordered_map<std::uint32_t,
                                             std::vector<HirNodeId>> const*
                                                   vlaSizeMap,
                          std::unordered_map<std::uint32_t, std::uint32_t> const*
                                                   sizeofVlaSymMap,
                          std::unordered_map<std::uint32_t, std::uint32_t> const*
                                                   typedefVlaOriginMap,
                          std::unordered_map<std::uint32_t, std::string> const*
                                                   synthRecipeMap,
                          HirReturnsTwiceMap const* returnsTwiceMap) {
    std::size_t const errorsBefore = reporter.errorCount();
    // Designated initializers (code-simplifier REQUIRED fold, LK6
    // cycle 2d post-fold review): a future field addition or
    // reorder is a compile error rather than a silent position
    // rebind. Trailing default-initialized fields are omitted.
    Lowerer lwr{
        .hir       = hir,
        .literals  = literals,
        .interner  = interner,
        .reporter  = reporter,
        .sourceMap = sourceMap,
        .config    = config,
        .ffiMap    = ffiMap,
        .linkageMap = linkageMap,
        .mutabilityMap = mutabilityMap,
        .volatileMap = volatileMap,
        .returnsTwiceMap = returnsTwiceMap,   // FC17.9(c) (D-CSUBSET-SETJMP)
        .alignmentMap = alignmentMap,
        .threadLocalMap = threadLocalMap,
        .vlaSizeMap = vlaSizeMap,
        .sizeofVlaSymMap = sizeofVlaSymMap,
        .typedefVlaOriginMap = typedefVlaOriginMap,
        .synthRecipeMap = synthRecipeMap,   // FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER)
        .mir       = MirBuilder{},
    };
    // D-OPT-LOAD-ALIAS-ANALYSIS-STRICT-TBAA-WIRING: stamp the module-
    // level alias-analysis polarity from the schema BEFORE any lowering
    // work runs. Unconditional (matches `globalsAllowFloat` discipline)
    // so a future default-flip in `MirBuilder` can't silently diverge
    // from the schema, AND both polarities are grep-discoverable.
    lwr.mir.setAliasingMode(config.strictAliasingOnDistinctTypes
        ? MirAliasingMode::StrictTBAA
        : MirAliasingMode::Permissive);
    lwr.mir.setCharTypesAliasAll(config.charTypesAliasAll);
    lwr.lower();
    HirToMirResult result;
    result.mir = std::move(lwr.mir).finish();
    // Canonical-marker stamping (D-OPT4-1): the creation-time
    // `createBlock(StructCfMarker::X)` stamps above are creation-time
    // DEFAULTS documenting lowering intent; the canonical CFG-derived
    // markers are stamped here as the FINAL step, module-wide. This is
    // what makes degenerate shapes verify: `while(1){break;}` lowers
    // with a LoopHeader stamp, but the break removed the back-edge —
    // the derivation normalizes the header to its actual role instead
    // of the verifier rejecting the program (the pre-derivation
    // behavior). See mir_struct_markers.hpp for the placement principle.
    rederiveStructCfMarkers(result.mir);
    result.externImports = std::move(lwr.externImports);
    result.ok = (reporter.errorCount() == errorsBefore);
    return result;
}

} // namespace dss
