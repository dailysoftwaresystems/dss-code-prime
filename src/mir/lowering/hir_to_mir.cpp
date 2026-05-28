#include "mir/lowering/hir_to_mir.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "hir/const_eval.hpp"
#include "hir/hir_op.hpp"

#include <array>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dss {

namespace {

// One transient per `lowerToMir` call. The HirLowering analog is `Lowerer`
// in cst_to_hir.cpp; this is much smaller because MIR is structurally
// simpler than CST (no schema-driven shape dispatch, no per-language
// vocabulary).
struct Lowerer {
    Hir const&            hir;
    HirLiteralPool const& literals;
    TypeInterner&         interner;
    DiagnosticReporter&   reporter;
    HirSourceMap const*   sourceMap;   // optional — diagnostics carry spans when bound
    MirBuilder            mir;
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
            case HirOpKind::Eq:     return isFloat ? MirOpcode::FCmpOeq : MirOpcode::ICmpEq;
            case HirOpKind::Ne:     return isFloat ? MirOpcode::FCmpOne : MirOpcode::ICmpNe;
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
                MirLiteralValue dst;
                dst.value = src.value;
                dst.core  = src.core;
                return mir.addConst(std::move(dst), t);
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
                for (std::size_t i = 1; i < kids.size(); ++i) {
                    MirInstId const arg = lowerExpr(kids[i]);
                    if (!arg.valid()) return InvalidMirInst;
                    operands.push_back(arg);
                }
                return mir.addInst(MirOpcode::Call, operands, t);
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
                MirBlockId const rhsBB   = mir.createBlock(StructCfMarker::Linear);
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
                MirInstId const operand = lowerExpr(kids[0]);
                if (!operand.valid()) return InvalidMirInst;
                TypeId const fromTy = hir.typeId(kids[0]);
                TypeKind const fromK = fromTy.valid()
                    ? interner.kind(fromTy) : TypeKind::Void;
                TypeKind const toK   = t.valid()
                    ? interner.kind(t) : TypeKind::Void;
                MirOpcode const mop = mapCast(fromK, toK);
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
                for (HirNodeId stmt : hir.seqExprStmts(node)) {
                    if (!lowerStmt(stmt)) return InvalidMirInst;
                }
                return lowerExpr(hir.seqExprResult(node));
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
        MirInstId const a = mir.addInst(MirOpcode::Alloca, {}, ptrTy);
        addressableLocal[sym.v] = a;
        return a;
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
            // GEP carries indices as operands. For a struct field, the
            // canonical shape is [basePtr, const-0, const-fieldIndex] —
            // the leading 0 walks "through" the pointer to the struct,
            // the fieldIndex picks the field. The HIR node's typeId is
            // the field's type; the GEP result is `pointer(fieldType)`.
            std::uint32_t const fieldIdx = hir.payload(node);
            TypeId const fieldTy = hir.typeId(node);
            if (!fieldTy.valid()) {
                unsupported(node, "MemberAccess with invalid field type "
                                   "(HIR verifier should have flagged)");
                return InvalidMirInst;
            }
            MirInstId const zero    = constInt(0);
            MirInstId const fieldK  = constInt(fieldIdx);
            if (!zero.valid() || !fieldK.valid()) return InvalidMirInst;
            std::array<MirInstId, 3> ops{basePtr, zero, fieldK};
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
            if (baseKind == TypeKind::Ptr) {
                MirInstId const basePtr = lowerExpr(kids[0]);
                if (!basePtr.valid()) return InvalidMirInst;
                std::array<MirInstId, 2> ops{basePtr, idx};
                return mir.addInst(MirOpcode::Gep, ops, resTy);
            }
            MirInstId const basePtr = lowerLvalueAddress(kids[0]);
            if (!basePtr.valid()) return InvalidMirInst;
            MirInstId const zero = constInt(0);
            if (!zero.valid()) return InvalidMirInst;
            std::array<MirInstId, 3> ops{basePtr, zero, idx};
            return mir.addInst(MirOpcode::Gep, ops, resTy);
        }
        unsupported(node, std::format(
            "lvalue kind ordinal {} not supported by this lowering "
            "(only Ref-to-addressable-local, Deref, MemberAccess, Index)",
            static_cast<unsigned>(k)));
        return InvalidMirInst;
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

    // Lower a single HIR statement in the currently-open MIR block.
    // Returns true on success, false on a hard error (caller bails).
    bool lowerStmt(HirNodeId node) {
        HirKind const k = hir.kind(node);
        switch (k) {
            case HirKind::Block: {
                for (HirNodeId child : hir.children(node)) {
                    if (!lowerStmt(child)) return false;
                }
                return true;
            }
            case HirKind::ReturnStmt: {
                auto v = hir.returnValue(node);
                if (v.has_value()) {
                    MirInstId const value = lowerExpr(*v);
                    if (!value.valid()) return false;
                    mir.addReturn(value);
                } else {
                    mir.addReturn();
                }
                return true;
            }
            case HirKind::ExprStmt: {
                // Discard the value; emit for side effects.
                HirNodeId const expr = hir.exprStmtExpr(node);
                MirInstId const v = lowerExpr(expr);
                return v.valid();
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
                    MirInstId const initVal = lowerExpr(*initN);
                    if (!initVal.valid()) return false;
                    std::array<MirInstId, 2> ops{initVal, alloca};
                    mir.addInst(MirOpcode::Store, ops);
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
                MirInstId const rhs = lowerExpr(valueN);
                if (!rhs.valid()) return false;
                MirInstId const ptr = lowerLvalueAddress(targetN);
                if (!ptr.valid()) return false;
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
                // so finish() doesn't abort on a created-but-unfilled block.
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
                    if (!lowerStmt(*initN)) {
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
                    // The update is an expression evaluated for its side
                    // effects (the value is discarded), same shape as ExprStmt.
                    MirInstId const u = lowerExpr(*updateN);
                    if (!u.valid()) {
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
        auto paramTypes = interner.fnParams(signature);
        if (params.size() != paramTypes.size()) {
            unsupported(node,
                std::format("Function param count {} mismatches FnSig param "
                            "count {}", params.size(), paramTypes.size()));
            return false;
        }

        // Per-function context reset: each function owns its own SSA/alloca
        // bindings — entries from the previous function are stale.
        symbolToValue.clear();
        addressableLocal.clear();

        // Pre-pass: scan the body to find params (and locals) whose address
        // is taken. Address-taken params must live in memory (alloca-backed),
        // not as pure SSA `Arg` values — otherwise the address would be
        // undefined. Body locals always get a slot at their `VarDecl`, so
        // the pre-pass result is only consulted for the param loop below.
        HirNodeId const body = hir.functionBody(node);
        std::unordered_set<std::uint32_t> addressTaken;
        collectAddressTakenSymbols(body, addressTaken);

        // From here on a block is open — any return-false MUST seal it.
        mir.addFunction(signature, symbol);
        MirBlockId const entry = mir.createBlock(StructCfMarker::EntryBlock);
        mir.beginBlock(entry);

        // Params: one `Arg` instruction per param, in declaration order. If
        // the param's address is ever taken in the body, ALSO allocate a slot
        // and store the arg into it — every read of that symbol then goes
        // through `Load(alloca)`. Otherwise the param stays in the pure-SSA
        // `symbolToValue` map.
        for (std::size_t i = 0; i < params.size(); ++i) {
            HirNodeId const p = params[i];
            // A param is a VarDecl whose typeId carries the param's type;
            // verifier already enforced the kind invariant upstream.
            SymbolId const sym = hir.varDeclSymbol(p);
            TypeId const ty = paramTypes[i];
            MirInstId const arg = mir.addArg(static_cast<std::uint32_t>(i), ty);
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
        EvalOptions opts;
        opts.resolveConstSymbol = [&initBySymbol](SymbolId s)
                -> std::optional<HirNodeId> {
            if (auto it = initBySymbol.find(s.v); it != initBySymbol.end()) {
                return it->second;
            }
            return std::nullopt;
        };
        // MIR-globals matches runtime behaviour: a narrowing initializer
        // wraps modularly (the runtime path would wrap too). Refusing to
        // fold here would only lose an optimization; the value installed
        // at module load is identical either way. D5.5 enum-bounds will
        // flip this back to `true` to surface the overflow as a verifier
        // diagnostic — same engine, different policy per consumer.
        opts.refuseOnOverflow = false;
        // CE5: MIR-globals also opts into float folding. A `float g = 1.5 + 2.0;`
        // initializer folds to constant `3.5` instead of synthesizing a
        // `__module_init__` runtime path. Same runtime-equivalence argument as
        // `refuseOnOverflow=false` — IEEE 754 host arithmetic produces the
        // same bits the runtime would compute; refusing would only lose an
        // optimization.
        opts.allowFloat = true;
        for (HirNodeId decl : hir.moduleDecls(moduleNode)) {
            if (hir.kind(decl) != HirKind::Global) continue;
            PendingGlobal pg;
            pg.symbol = hir.globalSymbol(decl);
            pg.type   = hir.globalType(decl);
            if (auto initN = hir.globalInit(decl); initN.has_value()) {
                // The resolver covers Refs to sibling globals; literal /
                // arithmetic / Cast paths still fold per CE1.
                ConstEvalResult const r = evaluateConstant(
                    hir, interner, literals, *initN, opts);
                if (r.value.has_value()) {
                    MirLiteralValue lit;
                    lit.value = r.value->value;
                    lit.core  = r.value->core;
                    pg.constInit = std::move(lit);
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
                mir.addGlobal(pg.type, pg.symbol, idx);
            } else if (pg.runtimeInit.valid()) {
                mir.addGlobal(pg.type, pg.symbol, UINT32_MAX, moduleInitFunc);
            } else {
                mir.addGlobal(pg.type, pg.symbol);
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
        collectFunctions(root);
        collectGlobals(root);
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
                    unsupported(decl, std::format(
                        "HIR ExternFunction (id {}) — FFI symbol ingestion "
                        "is not yet lowered", decl.v));
                    return;
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

HirToMirResult lowerToMir(Hir const&             hir,
                          HirLiteralPool const&  literals,
                          TypeInterner&          interner,
                          DiagnosticReporter&    reporter,
                          HirSourceMap const*    sourceMap) {
    std::size_t const errorsBefore = reporter.errorCount();
    Lowerer lwr{hir, literals, interner, reporter, sourceMap, MirBuilder{},
                {}, {}, {}, {}, {}, {}};
    lwr.lower();
    HirToMirResult result;
    result.mir = std::move(lwr.mir).finish();
    result.ok = (reporter.errorCount() == errorsBefore);
    return result;
}

} // namespace dss
