#include "mir/lowering/hir_to_mir.hpp"

#include "core/types/aggregate_abi.hpp"
#include "core/types/call_payload.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir_op.hpp"
#include "mir/mir_struct_markers.hpp"

#include <array>
#include <format>
#include <limits>
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
    // FC7 (D-FC7-MEMBER-ACCESS): per-CU memoized struct/union layouts keyed
    // by TypeId.v. `computeLayout` is PURE and a TypeId's layout is
    // invariant within the CU (one interner + one target config), so this
    // caches across functions and is never cleared. Read by
    // `fieldByteOffset` (member-access offset resolution) and
    // `aggregateByteSize` (aggregate-local Alloca sizing).
    std::unordered_map<std::uint32_t, StructLayout> layoutCache_;
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
    };
    std::vector<BranchFrame> branchStack;

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
            // UINT32_MAX seed: refuse to seed at the saturated
            // edge so the immediate `*nextSyntheticGlobalSym_`
            // read below never wraps. The caller fail-louds.
            if (maxV == std::numeric_limits<std::uint32_t>::max()) {
                return SymbolId{};
            }
            nextSyntheticGlobalSym_ = maxV + 1u;
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
        return MirOpcode::Invalid;
    }

    // Lower a single HIR expression in the currently-open MIR block.
    // Returns the MirInstId that produces the value (`InvalidMirInst` on
    // error — caller decides whether to keep emitting). Recursive.
    [[nodiscard]] MirInstId lowerExpr(HirNodeId node) {
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
                    (void)mir.addGlobal(t, sym, mirLitIdx);
                    TypeId const ptrTy = interner.pointer(t);
                    MirInstId const addr = mir.addGlobalAddr(sym, ptrTy);
                    std::array<MirInstId, 1> ops{addr};
                    return mir.addInst(MirOpcode::Load, ops, t);
                }
                return mir.addConst(toMirLiteral(src), t);
            }
            case HirKind::SizeOf: {
                // FC6: fold sizeof(T) to T's byte size (result type size_t = U64,
                // = `t`) via the type_layout engine. The TypeRef child carries the
                // type being sized (its typeId). Fail loud — never a guessed size —
                // if the target declared no layout params or the type is
                // incomplete/un-sizeable.
                if (!config.aggregateLayoutLoaded) {
                    unsupported(node, "sizeof requires the target to declare its "
                                      "'aggregateLayout' params");
                    return InvalidMirInst;
                }
                auto kids = hir.children(node);
                if (kids.empty()) {
                    unsupported(node, "SizeOf has no type-ref child");
                    return InvalidMirInst;
                }
                TypeId const sized = hir.typeId(kids.front());
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
                    std::array<MirInstId, 1> ops{it->second};
                    return mir.addInst(MirOpcode::Load, ops, t);
                }
                if (auto it = symbolToValue.find(sym); it != symbolToValue.end()) {
                    return it->second;
                }
                if (globalSymbols.contains(sym)) {
                    // Globals: GlobalAddr's result type is pointer(t); a
                    // following Load reads the value. The HIR Ref's typeId
                    // is the global's declared type, not pointer(type).
                    TypeId const ptrTy = interner.pointer(t);
                    MirInstId const addr = mir.addGlobalAddr(SymbolId{sym}, ptrTy);
                    std::array<MirInstId, 1> ops{addr};
                    return mir.addInst(MirOpcode::Load, ops, t);
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
                HirOpKind const op = decodeCoreOp(payload);
                auto kids = hir.children(node);
                if (kids.size() != 1) {
                    unsupported(node, "malformed UnaryOp (verifier should have flagged)");
                    return InvalidMirInst;
                }
                MirInstId const operand = lowerExpr(kids[0]);
                if (!operand.valid()) return InvalidMirInst;
                TypeId const operandType = hir.typeId(kids[0]);
                TypeKind const tk = operandType.valid()
                    ? interner.kind(operandType) : TypeKind::Void;
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
            case HirKind::Call: {
                // children: [callee, args...]. Lower the callee (a Ref-to-
                // function becomes a `GlobalAddr`; a function-pointer
                // expression becomes whatever MirInstId it lowers to) and
                // every arg, then emit a MIR Call. The result type comes
                // from the HIR node's typeId (the call's result type — HIR
                // already pulled it from the callee's FnSig at lowering
                // time). A void-returning callee has typeId == InvalidType,
                // which Call's MirResultRule::Optional accepts.
                auto kids = hir.children(node);
                if (kids.empty()) {
                    unsupported(node, "malformed Call (no callee child)");
                    return InvalidMirInst;
                }
                MirInstId const callee = lowerExpr(kids[0]);
                if (!callee.valid()) return InvalidMirInst;
                std::vector<MirInstId> operands;
                operands.reserve(kids.size());
                operands.push_back(callee);
                // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a struct/union
                // RETURN by value is classified + synthesized below, AFTER the
                // callee-variadic resolution (the sret hidden pointer prepends the
                // first arg, so it must be in place before the arg loop runs).
                // FC7 (D-FC7-STRUCT-BY-VALUE-ARG-RETURN): resolve whether the
                // callee is variadic (through one Ptr wrapper) BEFORE lowering
                // args, so a by-value struct ARG to a variadic function is
                // fail-loud this phase (its register-piece expansion would
                // desync the prefix-sum Arg-ordinal accounting from variadic
                // marshalling — FC12). The callPayload block below recomputes
                // this for the variadic-count stamp; kept separate to avoid
                // perturbing that audited path.
                TypeId byvalCalleeSig = hir.typeId(kids[0]);
                if (byvalCalleeSig.valid()
                    && interner.kind(byvalCalleeSig) == TypeKind::Ptr) {
                    auto const w = interner.operands(byvalCalleeSig);
                    if (!w.empty()) byvalCalleeSig = w[0];
                }
                bool const calleeVariadic =
                    byvalCalleeSig.valid()
                    && interner.kind(byvalCalleeSig) == TypeKind::FnSig
                    && interner.fnIsVariadic(byvalCalleeSig);
                // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value
                // struct/union RETURN. Classify it; allocate the result slot `R`;
                // for sret (class MEMORY, >16B) prepend `R` as the hidden first
                // arg so the arg loop below appends the real args AFTER it (the
                // callee receives the pointer as arg 0 and writes the result
                // through it). The `abi` + `R` are consumed at the Call-emit site.
                std::optional<AbiPassing> structRetAbi;
                MirInstId structRetSlot = InvalidMirInst;
                if (t.valid()
                    && (interner.kind(t) == TypeKind::Struct
                        || interner.kind(t) == TypeKind::Union)) {
                    structRetAbi = byValueClassify(t);
                    if (!structRetAbi.has_value()) {
                        unsupported(node,
                            "returning a by-value struct/union is not supported "
                            "by this target's calling convention "
                            "(D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                        return InvalidMirInst;
                    }
                    structRetSlot = freshAggregateTemp(t);
                    if (!structRetSlot.valid()) {
                        unsupported(node,
                            "by-value struct/union return requires a sizeable "
                            "layout (complete type)");
                        return InvalidMirInst;
                    }
                    if (structRetAbi->kind == AbiPassing::Kind::ByReference)
                        operands.push_back(structRetSlot);  // hidden sret pointer
                }
                for (std::size_t i = 1; i < kids.size(); ++i) {
                    TypeId const argTy = hir.typeId(kids[i]);
                    if (argTy.valid()) {
                        TypeKind const ak = interner.kind(argTy);
                        if (ak == TypeKind::Struct || ak == TypeKind::Union) {
                            // Classify + synthesize the by-value struct arg into
                            // register pieces / a by-ref pointer. Variadic or an
                            // unimplemented CC strategy ⇒ fail loud (never a
                            // silent wrong-ABI pass).
                            if (calleeVariadic) {
                                unsupported(kids[i],
                                    "passing a struct/union BY VALUE to a "
                                    "variadic function is not yet supported "
                                    "(D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                                return InvalidMirInst;
                            }
                            auto const abi = byValueClassify(argTy);
                            if (!abi.has_value()) {
                                unsupported(kids[i],
                                    "passing a struct/union BY VALUE is not "
                                    "supported by this target's calling "
                                    "convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                                return InvalidMirInst;
                            }
                            // FC7 C1b: the caller pushes each eightbyte piece as
                            // a scalar Call operand IN ORDER; lir_callconv assigns
                            // them to consecutive per-class arg registers. No
                            // multi-register guard — the callee's per-class Arg
                            // counter + the verifier's physical-arg-count bound
                            // (D-FC7-SYSV-STRUCT-ARG-MULTIREG) now handle >1 piece.
                            if (!appendByValueArg(operands, kids[i], argTy, *abi))
                                return InvalidMirInst;
                            continue;
                        }
                    }
                    MirInstId const arg = lowerExpr(kids[i]);
                    if (!arg.valid()) return InvalidMirInst;
                    operands.push_back(arg);
                }
                // D-LANG-VARIADIC (step 13.4): stamp the MIR Call's
                // payload with the callee's variadic-shape bits. The
                // callee HIR node's TypeId is its FnSig (for direct
                // calls — `Ref` to a function symbol carries the
                // symbol's FnSig type) OR a pointer-to-FnSig (for
                // indirect calls). We read it through one optional
                // pointer-deref to support both. A non-FnSig callee
                // (lowering bug, or fn-pointer-to-non-fn — already
                // caught at semantic) emits payload=0 (non-variadic).
                // Post-13.4 audit-fold (HIGH-5): a Ptr with empty
                // operands is structurally malformed (Ptr<*> always
                // carries the pointee at operands[0]) — fail-loud
                // rather than silently leave calleeTy pointing at
                // the Ptr wrapper which would silently degrade the
                // call to non-variadic.
                std::uint32_t callPayload = 0;
                TypeId calleeTy = hir.typeId(kids[0]);
                if (calleeTy.valid()
                    && interner.kind(calleeTy) == TypeKind::Ptr) {
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
                    auto const fixedSz = interner.fnParams(calleeTy).size();
                    // Post-13.4 audit-fold (HIGH-3): the call_payload
                    // u32 encodes fixedArgCount in bits 0..30 (mask
                    // 0x7FFF_FFFF). A FnSig with >= 2^31 params
                    // would silently truncate AND collide with the
                    // isVariadic bit. Practically unreachable (no
                    // function has 2-billion fixed params) but pin
                    // the contract so a future builder bypass can't
                    // silently corrupt the payload.
                    if (fixedSz > ::dss::call_payload::kFixedArgMask) {
                        unsupported(node,
                            "Call payload: fixed-arg count exceeds 31-bit "
                            "encoding limit");
                        return InvalidMirInst;
                    }
                    callPayload = ::dss::call_payload::encode(
                        true, static_cast<std::uint32_t>(fixedSz));
                }
                // FC7 C3 (AAPCS64/Apple x8 sret): when the by-value aggregate
                // RETURN is class-MEMORY (ByReference) AND this CC carries the
                // result pointer in a dedicated indirect-result register (x8, not
                // a hidden arg), flag the call so lir_callconv ROUTES the prepended
                // sret-pointer operand (operands[1]) to the IRR instead of arg0,
                // and shifts the real-arg index past it. Bit-30; independent of the
                // variadic bits already encoded above.
                if (structRetAbi.has_value()
                    && structRetAbi->kind == AbiPassing::Kind::ByReference
                    && !config.aggregateSretViaHiddenArg)
                    callPayload |= ::dss::call_payload::kIndirectResultBit;
                // FC7 C1c: a by-value struct/union return materializes the call
                // into its result slot `R` (sret pointer or eightbyte pieces) and
                // yields `R`'s address — the aggregate-by-address value the
                // consumers (assign/arg/member/return) expect.
                if (structRetAbi.has_value())
                    return emitStructReturningCall(node, operands, callPayload,
                                                   *structRetAbi, structRetSlot);
                return mir.addInst(MirOpcode::Call, operands, t, callPayload);
            }
            case HirKind::IntrinsicCall: {
                // children: [args...]; the intrinsic id lives in payload.
                // MirOpcode::IntrinsicCall has the same Optional result rule.
                auto kids = hir.children(node);
                std::vector<MirInstId> operands;
                operands.reserve(kids.size());
                for (HirNodeId argN : kids) {
                    MirInstId const arg = lowerExpr(argN);
                    if (!arg.valid()) return InvalidMirInst;
                    operands.push_back(arg);
                }
                std::uint32_t const intrinsicId = hir.payload(node);
                return mir.addInst(MirOpcode::IntrinsicCall, operands, t,
                                   intrinsicId);
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
                if (t.valid()) {
                    TypeKind const tk = interner.kind(t);
                    if (tk == TypeKind::Struct || tk == TypeKind::Union
                        || tk == TypeKind::Array) {
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
                HirOpKind const op = decodeCoreOp(payload);
                auto kids = hir.children(node);
                if (kids.size() != 2) {
                    unsupported(node, "malformed BinaryOp (verifier should "
                                       "have flagged this)");
                    return InvalidMirInst;
                }
                MirInstId const lhs = lowerExpr(kids[0]);
                MirInstId const rhs = lowerExpr(kids[1]);
                if (!lhs.valid() || !rhs.valid()) return InvalidMirInst;
                // Operand type drives signed/unsigned/float opcode choice.
                TypeId const operandType = hir.typeId(kids[0]);
                TypeKind const tk = operandType.valid()
                    ? interner.kind(operandType) : TypeKind::Void;
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
                std::array<MirInstId, 1> ops{ptr};
                return mir.addInst(MirOpcode::Load, ops, t);
            }
            case HirKind::MemberAccess:
            case HirKind::Index: {
                // Rvalue read of an lvalue: compute the field/element
                // address via the shared lvalue path, then emit `Load`.
                MirInstId const ptr = lowerLvalueAddress(node);
                if (!ptr.valid()) return InvalidMirInst;
                // FC8 D-CSUBSET-BITFIELD: a bit-field read loads the whole
                // allocation unit (the Gep already targets the unit), then
                // extracts the field's bits (shift + mask / sign-extend).
                if (BitFieldPlacement const* bf = bitfieldPlacementOf(node)) {
                    std::array<MirInstId, 1> lo{ptr};
                    MirInstId const unit = mir.addInst(MirOpcode::Load, lo, t);
                    if (!unit.valid()) return InvalidMirInst;
                    return emitBitfieldExtract(unit, *bf, t);
                }
                std::array<MirInstId, 1> ops{ptr};
                return mir.addInst(MirOpcode::Load, ops, t);
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
                    std::uint32_t const litIdx0 = hir.payload(kids[0]);
                    HirLiteralValue const& src = literals.at(litIdx0);
                    if (!std::holds_alternative<std::string>(src.value)) {
                        unsupported(node,
                            "Array→Pointer decay operand is a Literal "
                            "but its pool entry is not a string arm "
                            "(non-string array literals anchored "
                            "D-LK4-RODATA-PRODUCER-NONSTRING-ARRAY-"
                            "LITERAL-DECAY)");
                        return InvalidMirInst;
                    }
                    // Mint a fresh synthetic global for the literal,
                    // register it in the MIR globals arena with the
                    // string bytes as its constant-init, and emit a
                    // `GlobalAddr` that returns the Ptr<T> the Cast
                    // expression yields. The lowerMirGlobalsToDataItems
                    // pass (asm/asm.cpp) materializes the rodata bytes
                    // from this MirGlobal at assembly time.
                    //
                    // SymbolId-space-exhaustion guard (silent-failure
                    // HIGH-1 audit fold, 2026-06-02): the minter
                    // returns invalid SymbolId{} when the next mint
                    // would wrap UINT32_MAX. Fail loud rather than
                    // silently collide with SymbolId{0} (the
                    // invalid sentinel) or user-declared symbols.
                    SymbolId const sym = mintSyntheticGlobalSymbol();
                    if (!sym.valid()) {
                        unsupported(node,
                            "string-literal promotion failed: "
                            "synthetic SymbolId space exhausted "
                            "(UINT32_MAX wraparound). Source has "
                            "too many string literals OR the user "
                            "SymbolId range already saturates the "
                            "u32 space. Anchor: D-LK4-RODATA-"
                            "PRODUCER-STRING space-exhaustion pin.");
                        return InvalidMirInst;
                    }
                    std::uint32_t const mirLitIdx =
                        mir.literalPoolAdd(toMirLiteral(src));
                    (void)mir.addGlobal(fromTy, sym, mirLitIdx);
                    return mir.addGlobalAddr(sym, t);
                }
                MirInstId const operand = lowerExpr(kids[0]);
                if (!operand.valid()) return InvalidMirInst;
                // C 6.7.2.2: an enum casts AS its underlying integer (the kind in
                // the enum TypeId's `scalars[0]`). Resolve here so `mapCast`
                // (TypeKind-only, can't read the interner) sees the real width —
                // NO I32 assumption: a non-I32-underlying enum lowers via its
                // declared kind. The Cast's RESULT type stays `t` (enum-typed for
                // an int→enum cast). D-CSUBSET-ENUM-INT-CONVERSION.
                auto const enumUnderlying =
                    [&](TypeId ty, TypeKind k) noexcept -> TypeKind {
                        if (k != TypeKind::Enum || !ty.valid()) return k;
                        auto const sc = interner.scalars(ty);
                        return sc.empty() ? k : static_cast<TypeKind>(sc[0]);
                    };
                MirOpcode const mop = mapCast(enumUnderlying(fromTy, fromK),
                                              enumUnderlying(t, toK));
                if (mop == MirOpcode::Invalid) {
                    unsupported(node, std::format(
                        "Cast from TypeKind {} to {} has no MIR opcode",
                        static_cast<unsigned>(fromK),
                        static_cast<unsigned>(toK)));
                    return InvalidMirInst;
                }
                std::array<MirInstId, 1> ops{operand};
                return mir.addInst(mop, ops, t);
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
                if (t.valid()) {
                    TypeKind const tk = interner.kind(t);
                    if (tk == TypeKind::Struct || tk == TypeKind::Union
                        || tk == TypeKind::Array) {
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
        TypeKind const tk = interner.kind(ty);
        if (tk == TypeKind::Struct || tk == TypeKind::Union
            || tk == TypeKind::Array) {
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
        MirInstId const a =
            mir.addInst(MirOpcode::Alloca, {}, ptrTy, allocaPayload);
        addressableLocal[sym.v] = a;
        return a;
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

    // FC8 D-CSUBSET-BITFIELD: extract a bit-field value from a loaded allocation
    // unit. Unsigned: `(unit >> bitOffset) & ((1<<W)-1)`. Signed: sign-extend via
    // `(unit << (B - bitOffset - W)) >>arith (B - W)` (B = unit bits). All ops are
    // computed at the field's (unit) type — reuses the existing MIR shift/and ops.
    [[nodiscard]] MirInstId
    emitBitfieldExtract(MirInstId unitVal, BitFieldPlacement const& p, TypeId fieldTy) {
        fieldTy = enumReprType(fieldTy);   // enum bit-field → underlying integer
        std::uint32_t const B = p.unitBytes * 8;
        // Shift/mask constants MUST be the field/unit type, NOT I32 — a 64-bit-
        // unit bit-field (`unsigned long long x:40`) with an I32 const operand is
        // a mixed-width op the verifier doesn't catch (silent miscompile); see
        // constIntOfType. (Unit widths >64 are rejected at semantic, so the mask
        // fits in u64.)
        auto shiftBy = [&](MirInstId v, MirOpcode op, std::uint32_t amt) -> MirInstId {
            if (amt == 0) return v;
            MirInstId const sh = constIntOfType(static_cast<std::int64_t>(amt), fieldTy);
            std::array<MirInstId, 2> ops{v, sh};
            return mir.addInst(op, ops, fieldTy);
        };
        if (bitfieldIsSigned(interner.kind(fieldTy))) {
            MirInstId v = shiftBy(unitVal, MirOpcode::Shl, B - p.bitOffset - p.bitWidth);
            return shiftBy(v, MirOpcode::AShr, B - p.bitWidth);
        }
        MirInstId v = shiftBy(unitVal, MirOpcode::LShr, p.bitOffset);
        if (p.bitWidth < B) {
            std::uint64_t const mask =
                p.bitWidth >= 64 ? ~0ull : ((1ull << p.bitWidth) - 1);
            MirInstId const m = constIntOfType(static_cast<std::int64_t>(mask), fieldTy);
            std::array<MirInstId, 2> ops{v, m};
            v = mir.addInst(MirOpcode::And, ops, fieldTy);
        }
        return v;
    }

    // FC8 D-CSUBSET-BITFIELD: read-modify-write a bit-field at unit address
    // `unitPtr` with `rhsVal`: `unit = (unit & ~(mask<<off)) | ((rhs & mask)<<off)`
    // then store back. The RMW preserves every OTHER bit in the unit (incl. a
    // neighbour field packed into the same unit). Computed at the field/unit type.
    [[nodiscard]] bool
    emitBitfieldInsert(MirInstId unitPtr, MirInstId rhsVal,
                       BitFieldPlacement const& p, TypeId fieldTy) {
        fieldTy = enumReprType(fieldTy);   // enum bit-field → underlying integer
        std::uint64_t const mask =
            p.bitWidth >= 64 ? ~0ull : ((1ull << p.bitWidth) - 1);
        std::uint64_t const fieldMask = mask << p.bitOffset;
        std::array<MirInstId, 1> lo{unitPtr};
        MirInstId const unit = mir.addInst(MirOpcode::Load, lo, fieldTy);
        if (!unit.valid()) return false;
        MirInstId const clrK = constIntOfType(static_cast<std::int64_t>(~fieldMask), fieldTy);
        std::array<MirInstId, 2> ca{unit, clrK};
        MirInstId const cleared = mir.addInst(MirOpcode::And, ca, fieldTy);
        MirInstId const mK = constIntOfType(static_cast<std::int64_t>(mask), fieldTy);
        std::array<MirInstId, 2> ma{rhsVal, mK};
        MirInstId masked = mir.addInst(MirOpcode::And, ma, fieldTy);
        if (p.bitOffset != 0) {
            MirInstId const sh = constIntOfType(static_cast<std::int64_t>(p.bitOffset), fieldTy);
            std::array<MirInstId, 2> sa{masked, sh};
            masked = mir.addInst(MirOpcode::Shl, sa, fieldTy);
        }
        std::array<MirInstId, 2> oa{cleared, masked};
        MirInstId const merged = mir.addInst(MirOpcode::Or, oa, fieldTy);
        std::array<MirInstId, 2> st{merged, unitPtr};
        mir.addInst(MirOpcode::Store, st);
        return true;
    }

    // Resolve the ADDRESS of an lvalue expression — the pointer value a
    // `Store` should write into (or an `AddressOf` should yield directly).
    // Distinct from `lowerExpr` which produces the RVALUE of the lvalue
    // (`Load(ptr)`). Supported lvalue shapes:
    //   - `Ref(sym)` where sym is an addressable local → the local's alloca.
    //   - `Deref(ptr)` → the lowered pointer (no double-load).
    //   - `MemberAccess(base, .field)` → `GEP(addressOf(base), const-field)`.
    //   - `Index(base, idxExpr)` → `GEP(addressOf(base), idxValue)`.
    // Returns `InvalidMirInst` on failure.
    [[nodiscard]] MirInstId lowerLvalueAddress(HirNodeId node) {
        HirKind const k = hir.kind(node);
        // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value struct/union-
        // returning CALL is an aggregate rvalue materialized into a result slot;
        // its lvalue address IS that slot. `lowerExpr(Call)` does the
        // materialization and yields the slot address, so every aggregate
        // consumer (`a = f()`, `g(f())`, `f().x`, `return f();`) reaches the
        // result by address (the call is emitted exactly once — see the
        // call-once pin). A scalar call is not an lvalue → falls through.
        if (k == HirKind::Call) {
            TypeId const ct = hir.typeId(node);
            if (ct.valid()
                && (interner.kind(ct) == TypeKind::Struct
                    || interner.kind(ct) == TypeKind::Union))
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
            // FC7 (D-FC7-MEMBER-ACCESS): resolve the field's BYTE OFFSET via
            // the FC6 `computeLayout` engine and emit a 2-op base+disp GEP
            // `[basePtr, Const(byteOffset)]` — which MIR→LIR lowers to the
            // base+disp `lea` BOTH targets ship (x86 lea [base+disp32] /
            // arm64 ADD Xd,Xn,#imm12). The aggregate TypeId is `basePtr`'s
            // pointee (basePtr : pointer(aggTy)); the HIR node's typeId is
            // the FIELD type, so the GEP result is `pointer(fieldType)`.
            // (Replaces the prior field-INDEX GEP `[basePtr, 0, fieldIdx]`,
            // whose 3-op shape MIR→LIR never realized.)
            std::uint32_t const fieldIdx = hir.payload(node);
            TypeId const fieldTy = hir.typeId(node);
            if (!fieldTy.valid()) {
                unsupported(node, "MemberAccess with invalid field type "
                                   "(HIR verifier should have flagged)");
                return InvalidMirInst;
            }
            // The aggregate TypeId is the BASE child's HIR type (`s` →
            // struct S; `*p` → struct S; `a.b` → struct B; `arr[i]` →
            // struct Elem) — robust across every nested/indexed base shape.
            TypeId const aggTy = hir.typeId(kids[0]);
            if (!aggTy.valid()) {
                unsupported(node, "MemberAccess base has no type "
                                   "(HIR verifier should have flagged)");
                return InvalidMirInst;
            }
            auto const byteOffset = fieldByteOffset(aggTy, fieldIdx);
            if (!byteOffset.has_value()) {
                unsupported(node, "MemberAccess field-offset resolution failed "
                                   "(target lacks 'aggregateLayout', the "
                                   "aggregate is un-sizeable, or the field "
                                   "index is out of range)");
                return InvalidMirInst;
            }
            MirInstId const offK =
                constInt(static_cast<std::int64_t>(*byteOffset));
            if (!offK.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> ops{basePtr, offK};
            return mir.addInst(MirOpcode::Gep, ops, interner.pointer(fieldTy));
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
                std::array<MirInstId, 2> ops{basePtr, byteIdx};
                return mir.addInst(MirOpcode::Gep, ops, resTy);
            }
            // Storage (array/struct) base: the lvalue address IS the base; the
            // vestigial leading `0` of the old 3-op form (stepping through the
            // pointer-to-array, contributing 0 bytes) is dropped — base+byteIdx
            // is exact. Both arms now emit the same 2-op byte-offset Gep; the
            // 3-op form is no longer produced (its LIR arm stays defensively
            // fail-loud).
            MirInstId const basePtr = lowerLvalueAddress(kids[0]);
            if (!basePtr.valid()) return InvalidMirInst;
            std::array<MirInstId, 2> ops{basePtr, byteIdx};
            return mir.addInst(MirOpcode::Gep, ops, resTy);
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
            // ONLY on HirKind + the type's kind (Struct/Union/Array) + the
            // config-driven layout — no target/arch/format identity.
            TypeId const at = hir.typeId(node);
            TypeKind const ak = at.valid() ? interner.kind(at) : TypeKind::Void;
            if (ak == TypeKind::Struct || ak == TypeKind::Union
                || ak == TypeKind::Array) {
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
        TypeKind const k = ty.valid() ? interner.kind(ty) : TypeKind::I32;
        MirLiteralValue lit;
        lit.value = v;
        lit.core  = k;
        return mir.addConst(std::move(lit), interner.primitive(k));
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
                                                          TypeId aggTy) {
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
            mir.addInst(MirOpcode::Store, zst);
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
                if (fk == TypeKind::Struct || fk == TypeKind::Union) {
                    if (hir.kind(child) == HirKind::ConstructAggregate) {
                        if (!lowerAggregateInitIntoSlot(child, fieldPtr, fieldTy))
                            return false;
                    } else {
                        MirInstId const srcPtr = lowerLvalueAddress(child);
                        if (!srcPtr.valid()) return false;
                        if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy))
                            return false;
                    }
                    continue;
                }
                if (fk == TypeKind::Array) {
                    if (hir.kind(child) == HirKind::ConstructAggregate) {
                        if (!lowerArrayInitIntoSlot(child, fieldPtr, fieldTy))
                            return false;
                    } else {
                        MirInstId const srcPtr = lowerLvalueAddress(child);
                        if (!srcPtr.valid()) return false;
                        if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy))
                            return false;
                    }
                    continue;
                }
                MirInstId const v = lowerExpr(child);
                if (!v.valid()) return false;
                std::array<MirInstId, 2> stOps{v, fieldPtr};
                mir.addInst(MirOpcode::Store, stOps);
                continue;
            }
            // Bit-field: Gep to the UNIT (already zeroed in pass 1), then RMW the
            // value in. The Gep width follows the field type = the unit width,
            // matching emitBitfieldInsert's load/store width.
            std::array<MirInstId, 2> gepOps{allocaPtr, offK};
            MirInstId const unitPtr = mir.addInst(
                MirOpcode::Gep, gepOps, interner.pointer(fieldTy));
            if (!unitPtr.valid()) return false;
            MirInstId const v = lowerExpr(child);
            if (!v.valid()) return false;
            if (!emitBitfieldInsert(unitPtr, v, layout->bitFields[f], fieldTy))
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
                                                  TypeId aggTy) {
        // FC8 D-CSUBSET-BITFIELD-INIT: a struct/union initializer whose type has
        // bit-fields packs per allocation unit — a plain field-wise Store would
        // write each bit-field full-width into the shared unit, clobbering its
        // co-resident neighbours. Routed to the unit-aware initializer.
        if (hasBitfieldMember(aggTy)) {
            return lowerBitfieldAggregateInitIntoSlot(aggNode, allocaPtr, aggTy);
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
            if (fk == TypeKind::Struct || fk == TypeKind::Union) {
                if (hir.kind(kids[i]) == HirKind::ConstructAggregate) {
                    if (!lowerAggregateInitIntoSlot(kids[i], fieldPtr, fieldTy))
                        return false;
                    continue;
                }
                // FC7 (D-FC7-AGGREGATE-COPY-MEMCPY): an aggregate field
                // initialized from an aggregate VALUE (`{ existingStruct, … }`,
                // not a nested brace) is a copy from that value's lvalue
                // address into the field's sub-slot.
                MirInstId const srcPtr = lowerLvalueAddress(kids[i]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy))
                    return false;
                continue;
            }

            // D-MIR-ARRAY-FIELD-AGGREGATE-INIT: an ARRAY-typed field. Array
            // element offsets are `j·stride` (not struct fieldOffsets), so it
            // routes to lowerArrayInitIntoSlot — a brace-init recurses element-
            // wise; an array VALUE copies byte-wise (D-FC7-AGGREGATE-COPY).
            if (fk == TypeKind::Array) {
                if (hir.kind(kids[i]) == HirKind::ConstructAggregate) {
                    if (!lowerArrayInitIntoSlot(kids[i], fieldPtr, fieldTy))
                        return false;
                    continue;
                }
                MirInstId const srcPtr = lowerLvalueAddress(kids[i]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(aggNode, srcPtr, fieldPtr, fieldTy))
                    return false;
                continue;
            }

            MirInstId const v = lowerExpr(kids[i]);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> stOps{v, fieldPtr};
            mir.addInst(MirOpcode::Store, stOps);
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
                                              TypeId /*arrTy*/) {
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
            if (ek == TypeKind::Struct || ek == TypeKind::Union) {
                if (hir.kind(kids[j]) == HirKind::ConstructAggregate) {
                    if (!lowerAggregateInitIntoSlot(kids[j], elemPtr, elemTy))
                        return false;
                    continue;
                }
                MirInstId const srcPtr = lowerLvalueAddress(kids[j]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(arrNode, srcPtr, elemPtr, elemTy))
                    return false;
                continue;
            }
            if (ek == TypeKind::Array) {
                if (hir.kind(kids[j]) == HirKind::ConstructAggregate) {
                    if (!lowerArrayInitIntoSlot(kids[j], elemPtr, elemTy))
                        return false;
                    continue;
                }
                MirInstId const srcPtr = lowerLvalueAddress(kids[j]);
                if (!srcPtr.valid()) return false;
                if (!lowerAggregateCopy(arrNode, srcPtr, elemPtr, elemTy))
                    return false;
                continue;
            }

            MirInstId const v = lowerExpr(kids[j]);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> stOps{v, elemPtr};
            mir.addInst(MirOpcode::Store, stOps);
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
                                         std::uint64_t size) {
        auto emit = [&](TypeKind chunkKind, std::uint64_t off) -> bool {
            TypeId const chunkTy = interner.primitive(chunkKind);
            TypeId const ptrTy   = interner.pointer(chunkTy);
            MirInstId const offK = constInt(static_cast<std::int64_t>(off));
            if (!offK.valid()) return false;
            std::array<MirInstId, 2> sg{srcPtr, offK};
            MirInstId const sp = mir.addInst(MirOpcode::Gep, sg, ptrTy);
            if (!sp.valid()) return false;
            std::array<MirInstId, 1> ld{sp};
            MirInstId const v = mir.addInst(MirOpcode::Load, ld, chunkTy);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> dg{dstPtr, offK};
            MirInstId const dp = mir.addInst(MirOpcode::Gep, dg, ptrTy);
            if (!dp.valid()) return false;
            std::array<MirInstId, 2> st{v, dp};
            mir.addInst(MirOpcode::Store, st);
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
                                          MirInstId dstPtr, TypeId aggTy) {
        StructLayout const* layout = cachedLayout(aggTy);
        if (layout == nullptr) {
            unsupported(atNode, "aggregate copy requires a sizeable layout "
                                "(target 'aggregateLayout' / complete type)");
            return false;
        }
        TypeKind const aggKind = interner.kind(aggTy);
        if (aggKind == TypeKind::Union)
            return lowerByteWiseCopy(srcPtr, dstPtr, layout->size);
        if (aggKind != TypeKind::Struct) {
            unsupported(atNode, "aggregate copy of a non-struct/union value "
                                "is not supported");
            return false;
        }
        auto const fieldTypes = interner.operands(aggTy);
        if (fieldTypes.size() != layout->fieldOffsets.size()) {
            unsupported(atNode, "struct copy: field-type count mismatches the "
                                "layout field-offset count (internal invariant)");
            return false;
        }
        bool anyAggregateField = false;
        for (TypeId ft : fieldTypes) {
            TypeKind const fk = interner.kind(ft);
            if (fk == TypeKind::Struct || fk == TypeKind::Union
                || fk == TypeKind::Array) {
                anyAggregateField = true;
                break;
            }
        }
        if (anyAggregateField)
            return lowerByteWiseCopy(srcPtr, dstPtr, layout->size);

        // Flat scalar struct — field-wise, width-exact.
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
                mir.addInst(MirOpcode::Load, loadOps, fieldTypes[i]);
            if (!v.valid()) return false;
            std::array<MirInstId, 2> dstGep{dstPtr, offK};
            MirInstId const dstField =
                mir.addInst(MirOpcode::Gep, dstGep, fptrTy);
            if (!dstField.valid()) return false;
            std::array<MirInstId, 2> stOps{v, dstField};
            mir.addInst(MirOpcode::Store, stOps);
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
        [[nodiscard]] std::uint32_t next(AbiPieceClass cls) {
            if (slotAligned) return flat++;
            return (cls == AbiPieceClass::Fpr) ? fpr++ : gpr++;
        }
    };

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
        if (!lowerByteWiseCopy(srcAddr, temp, layout->size)) return false;
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
            // The hidden pointer is GPR-class.
            MirInstId const ptr =
                mir.addArg(ctr.next(AbiPieceClass::Gpr), interner.pointer(aggTy));
            if (!ptr.valid()) return false;
            addressableLocal[sym.v] = ptr;   // the caller's copy is the param
            return true;
        }
        MirInstId const slot = allocaForLocal(sym, aggTy, anchor);
        if (!slot.valid()) return false;
        for (AbiPiece const& p : abi.pieces) {
            TypeId const pty = pieceType(p);
            MirInstId const a = mir.addArg(ctr.next(p.cls), pty);
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
            std::uint32_t callPayload, AbiPassing const& abi, MirInstId slot) {
        if (abi.kind == AbiPassing::Kind::ByReference) {
            MirInstId const call =
                mir.addInst(MirOpcode::Call, operands, InvalidType, callPayload);
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
            mir.addInst(MirOpcode::Call, operands, p0ty, callPayload);
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
        if (abi->kind == AbiPassing::Kind::ByReference) {
            if (!sretPtr_.valid()) {
                unsupported(valNode, "sret return reached without a hidden result "
                                     "pointer (lowerFunction setup invariant)");
                return false;
            }
            if (!lowerByteWiseCopy(srcAddr, sretPtr_, layout->size)) return false;
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
            MirInstId const v = mir.addInst(MirOpcode::Load, l, pty);
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
        }
        for (HirNodeId child : hir.children(node)) {
            collectAddressTakenSymbols(child, out);
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
            TypeKind const ek = interner.kind(et);
            if (ek == TypeKind::Struct || ek == TypeKind::Union
                || ek == TypeKind::Array) {
                return lowerLvalueAddress(expr).valid();
            }
        }
        return lowerExpr(expr).valid();
    }

    // Lower a single HIR statement in the currently-open MIR block.
    // Returns true on success, false on a hard error (caller bails).
    bool lowerStmt(HirNodeId node) {
        HirKind const k = hir.kind(node);
        switch (k) {
            case HirKind::Block: {
                auto kids = hir.children(node);
                for (std::size_t i = 0; i < kids.size(); ++i) {
                    if (!lowerStmt(kids[i])) return false;
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
                if (currentFnResult_.valid()) {
                    TypeKind const rk = interner.kind(currentFnResult_);
                    if (rk == TypeKind::Struct || rk == TypeKind::Union)
                        return lowerStructReturn(*v);
                }
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
                MirInstId const alloca = allocaForLocal(sym, ty, node);
                if (!alloca.valid()) return false;
                if (auto initN = hir.varDeclInit(node); initN.has_value()) {
                    // FC7 (D-FC7-MEMBER-ACCESS): a struct/union initializer
                    // (`P p = {3,4}` / `{.y=7}`) lowers ELEMENT-WISE — one
                    // Gep+Store per field into the slot — never as an
                    // aggregate-SSA Store (no LIR aggregate-width Store).
                    TypeKind const initKind = interner.kind(ty);
                    if (hir.kind(*initN) == HirKind::ConstructAggregate
                        && (initKind == TypeKind::Struct
                            || initKind == TypeKind::Union)) {
                        if (!lowerAggregateInitIntoSlot(*initN, alloca, ty))
                            return false;
                    } else if (hir.kind(*initN) == HirKind::ConstructAggregate
                               && initKind == TypeKind::Array) {
                        // D-MIR-ARRAY-FIELD-AGGREGATE-INIT (array-LOCAL form):
                        // `int a[3] = {1,2,3}` — element-wise into the slot via
                        // the same helper the array-field recurse-guard uses.
                        if (!lowerArrayInitIntoSlot(*initN, alloca, ty))
                            return false;
                    } else if (initKind == TypeKind::Struct
                               || initKind == TypeKind::Union) {
                        // FC7 (D-FC7-MEMBER-ACCESS): struct/union COPY-init
                        // from another aggregate VALUE (`T a = b;`) — copy
                        // field-wise from the source lvalue's address. An
                        // aggregate-width Load+Store would truncate to one
                        // register.
                        MirInstId const srcPtr = lowerLvalueAddress(*initN);
                        if (!srcPtr.valid()) return false;
                        if (!lowerAggregateCopy(*initN, srcPtr, alloca, ty))
                            return false;
                    } else {
                        MirInstId const initVal = lowerExpr(*initN);
                        if (!initVal.valid()) return false;
                        std::array<MirInstId, 2> ops{initVal, alloca};
                        mir.addInst(MirOpcode::Store, ops);
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
                // bytes (a silent miscompile).
                TypeId const valTy = hir.typeId(valueN);
                if (valTy.valid()
                    && (interner.kind(valTy) == TypeKind::Struct
                        || interner.kind(valTy) == TypeKind::Union)) {
                    MirInstId const dstPtr = lowerLvalueAddress(targetN);
                    if (!dstPtr.valid()) return false;
                    MirInstId const srcPtr = lowerLvalueAddress(valueN);
                    if (!srcPtr.valid()) return false;
                    return lowerAggregateCopy(node, srcPtr, dstPtr, valTy);
                }
                MirInstId const rhs = lowerExpr(valueN);
                if (!rhs.valid()) return false;
                MirInstId const ptr = lowerLvalueAddress(targetN);
                if (!ptr.valid()) return false;
                // FC8 D-CSUBSET-BITFIELD: a bit-field write is a READ-MODIFY-WRITE
                // of the allocation unit (the Gep targets the unit) — clear the
                // field's bits, OR in the new value, store back. A plain Store
                // would overwrite the neighbour bits packed in the same unit.
                if (BitFieldPlacement const* bf = bitfieldPlacementOf(targetN)) {
                    return emitBitfieldInsert(ptr, rhs, *bf, hir.typeId(targetN));
                }
                std::array<MirInstId, 2> ops{rhs, ptr};
                mir.addInst(MirOpcode::Store, ops);
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
                MirBlockId const target =
                    branchStack[branchStack.size() - 1 - depth].breakBB;
                mir.addBr(target);
                return true;
            }
            case HirKind::ContinueStmt: {
                std::uint32_t const depth = hir.branchDepth(node);
                if (depth >= branchStack.size()) {
                    unsupported(node, std::format(
                        "ContinueStmt depth {} exceeds enclosing-frame count "
                        "{} (HIR verifier should have flagged this)",
                        depth, branchStack.size()));
                    return false;
                }
                BranchFrame& frame =
                    branchStack[branchStack.size() - 1 - depth];
                if (!frame.continueBB.valid()) {
                    unsupported(node, std::format(
                        "ContinueStmt depth {} resolves to a switch frame "
                        "which has no continue target (HIR verifier should "
                        "have flagged this)", depth));
                    return false;
                }
                frame.continueReferenced = true;
                mir.addBr(frame.continueBB);
                return true;
            }
            case HirKind::GotoStmt: {
                // Unconditional jump to the target label's block (created lazily so
                // forward + backward gotos resolve identically). The open block is
                // now terminated; a following sibling opens a fresh dead block (the
                // Block-lowering does this), which the unreachable-prune drops.
                mir.addBr(getOrCreateLabelBlock(hir.labelOrdinal(node)));
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
                // C-style switch: each `CaseArm` has an optional match value
                // and a body span; arms execute in declaration order with
                // FALL-THROUGH when a body doesn't terminate (no break +
                // no return). `default:` is the fall-back target. Lower as:
                //   - lower discriminant
                //   - createBlock per arm (1+ body blocks) + one exit block
                //   - emit `Switch(disc, cases…, defaultBB)` where
                //     defaultBB is the default arm's first block (or `exit`
                //     if no default arm exists)
                //   - lower each arm's body in order; fall-through arms
                //     branch to the NEXT arm's first block; the last arm
                //     falls through to exit.
                //   - push `{invalid-continue, exit}` so `break;` targets
                //     exit (continue inside switch is a HIR verifier error).
                HirNodeId const discN = hir.switchDiscriminant(node);
                auto       const arms  = hir.switchArms(node);

                MirInstId const disc = lowerExpr(discN);
                if (!disc.valid()) return false;

                MirBlockId const exitBB = mir.createBlock(StructCfMarker::SwitchJoin);
                // One block per arm, in declaration order.
                std::vector<MirBlockId> armBlocks;
                armBlocks.reserve(arms.size());
                for (std::size_t i = 0; i < arms.size(); ++i) {
                    armBlocks.push_back(mir.createBlock(StructCfMarker::SwitchCase));
                }

                // Build (caseValue, target) list and resolve defaultBB.
                std::vector<std::pair<MirInstId, MirBlockId>> cases;
                cases.reserve(arms.size());
                MirBlockId defaultBB{};
                for (std::size_t i = 0; i < arms.size(); ++i) {
                    HirNodeId const arm = arms[i];
                    if (hir.caseArmIsDefault(arm)) {
                        if (defaultBB.valid()) {
                            unsupported(arm, "switch has more than one "
                                              "default arm (HIR verifier "
                                              "should have flagged this)");
                            sealCreatedAsUnreachable(exitBB);
                            for (MirBlockId b : armBlocks) sealCreatedAsUnreachable(b);
                            return false;
                        }
                        defaultBB = armBlocks[i];
                        continue;
                    }
                    auto const valN = hir.caseArmValue(arm);
                    if (!valN.has_value()) {
                        unsupported(arm, "non-default CaseArm without "
                                          "match value (HIR verifier "
                                          "should have flagged this)");
                        sealCreatedAsUnreachable(exitBB);
                        for (MirBlockId b : armBlocks) sealCreatedAsUnreachable(b);
                        return false;
                    }
                    MirInstId const caseVal = lowerExpr(*valN);
                    if (!caseVal.valid()) {
                        sealCreatedAsUnreachable(exitBB);
                        for (MirBlockId b : armBlocks) sealCreatedAsUnreachable(b);
                        return false;
                    }
                    cases.emplace_back(caseVal, armBlocks[i]);
                }
                if (!defaultBB.valid()) defaultBB = exitBB;

                mir.addSwitch(disc, cases, defaultBB);

                // Lower each arm's body, falling through to the next arm
                // when not self-terminated. Push the break-frame ONCE for
                // the whole switch (all arms share it).
                branchStack.push_back({MirBlockId{}, exitBB});
                for (std::size_t i = 0; i < arms.size(); ++i) {
                    mir.beginBlock(armBlocks[i]);
                    HirNodeId const arm = arms[i];
                    bool armOk = true;
                    for (HirNodeId stmt : hir.caseArmBody(arm)) {
                        if (!lowerStmt(stmt)) { armOk = false; break; }
                    }
                    if (!armOk) {
                        if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                        // Seal the remaining arm blocks + exit so finish()
                        // doesn't abort on a created-but-unfilled block.
                        for (std::size_t j = i + 1; j < arms.size(); ++j) {
                            sealCreatedAsUnreachable(armBlocks[j]);
                        }
                        sealCreatedAsUnreachable(exitBB);
                        branchStack.pop_back();
                        return false;
                    }
                    if (!mir.openBlockHasTerminator()) {
                        // Fall through: branch to the next arm's first
                        // block, or to exit if this is the last arm.
                        MirBlockId const fall = (i + 1 < arms.size())
                            ? armBlocks[i + 1] : exitBB;
                        mir.addBr(fall);
                    }
                }
                branchStack.pop_back();

                mir.beginBlock(exitBB);
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
        labelBlocks_.clear();   // FC5: labels are function-scoped
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

        // FC7 C1c (D-FC7-SYSV-STRUCT-RETURN-IN-REGS): a by-value struct/union
        // RETURN. Classify under the active CC's strategy. ByReference (class
        // MEMORY, >16B) ⇒ sret: prepend a hidden result-pointer `Arg` (consuming
        // the first INTEGER ordinal); the `ReturnStmt` copies the result through
        // it and returns it. InRegisters (≤16B) ⇒ no hidden arg; the `ReturnStmt`
        // loads the eightbyte pieces. A strategy that can't classify the aggregate
        // (AAPCS64 until C3) fails loud (sealing the open block first).
        if (currentFnResult_.valid()) {
            TypeKind const rk = interner.kind(currentFnResult_);
            if (rk == TypeKind::Struct || rk == TypeKind::Union) {
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
                    sretPtr_ = config.aggregateSretViaHiddenArg
                        ? mir.addArg(argCtr.next(AbiPieceClass::Gpr),
                                     interner.pointer(currentFnResult_))
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
        bool const fnVariadic = interner.fnIsVariadic(signature);
        for (std::size_t i = 0; i < params.size(); ++i) {
            HirNodeId const p = params[i];
            // A param is a VarDecl whose typeId carries the param's type;
            // verifier already enforced the kind invariant upstream.
            SymbolId const sym = hir.varDeclSymbol(p);
            TypeId const ty = paramTypes[i];
            TypeKind const pk = interner.kind(ty);
            if (pk == TypeKind::Struct || pk == TypeKind::Union) {
                if (fnVariadic) {
                    unsupported(p, "a by-value struct/union parameter in a "
                                   "variadic function is not yet supported "
                                   "(D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                auto const abi = byValueClassify(ty);
                if (!abi.has_value()) {
                    unsupported(p, "a by-value struct/union parameter is not "
                                   "supported by this target's calling "
                                   "convention (D-FC7-STRUCT-BY-VALUE-ARG-RETURN)");
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                if (!receiveByValueParam(sym, ty, p, *abi, argCtr)) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                continue;
            }
            // FC7 (D-FC7-SYSV-STRUCT-ARG-MULTIREG / fixes D-ML7-2.10): a scalar
            // param's `Arg` payload is its PER-CLASS register ordinal (GPR/FPR
            // counted separately for an independent CC), not the param index.
            MirInstId const arg = mir.addArg(argCtr.next(scalarArgClass(ty)), ty);
            if (addressTaken.contains(sym.v)) {
                MirInstId const slot = allocaForLocal(sym, ty, p);
                if (!slot.valid()) {
                    if (!mir.openBlockHasTerminator()) mir.addUnreachable();
                    return false;
                }
                std::array<MirInstId, 2> ops{arg, slot};
                mir.addInst(MirOpcode::Store, ops);
            } else {
                symbolToValue[sym.v] = arg;
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
            if (meta->importLibrary.empty()) {
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
    };
    std::vector<PendingGlobal> pendingGlobals;

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
            if (auto initN = hir.globalInit(decl); initN.has_value()) {
                // The resolver covers Refs to sibling globals; literal /
                // arithmetic / Cast paths still fold per CE1.
                ConstEvalResult const r = evaluateConstant(
                    hir, interner, literals, *initN, env, opts);
                if (r.value.has_value()) {
                    pg.constInit = toMirLiteral(*r.value);
                } else {
                    pg.runtimeInit = *initN;
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
            if (pg.constInit.has_value()) {
                std::uint32_t const idx = mir.literalPoolAdd(*pg.constInit);
                mir.addGlobal(pg.type, pg.symbol, idx, {},
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst);
            } else if (pg.runtimeInit.valid()) {
                mir.addGlobal(pg.type, pg.symbol, UINT32_MAX, moduleInitFunc,
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst);
            } else {
                mir.addGlobal(pg.type, pg.symbol, UINT32_MAX, {},
                              pg.linkage.binding, pg.linkage.visibility,
                              pg.isConst);
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
                mir.addInst(MirOpcode::Store, ops);
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
                    unsupported(decl, std::format(
                        "HIR ExternGlobal (id {}) — FFI symbol ingestion is "
                        "not yet lowered", decl.v));
                    return;
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
                          HirMutabilityMap const*  mutabilityMap) {
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
