#include "mir/lowering/hir_to_mir.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "hir/hir_op.hpp"

#include <format>
#include <string>
#include <unordered_map>
#include <utility>

namespace dss {

namespace {

// One transient per `lowerToMir` call. The HirLowering analog is `Lowerer`
// in cst_to_hir.cpp; this is much smaller because MIR is structurally
// simpler than CST (no schema-driven shape dispatch, no per-language
// vocabulary).
struct Lowerer {
    Hir const&            hir;
    HirLiteralPool const& literals;
    TypeInterner const&   interner;
    DiagnosticReporter&   reporter;
    MirBuilder            mir;
    // Within one function: HIR VarDecl / param `SymbolId.v` → MirInstId that
    // produces its value. For ML2 cycle 1 this covers params + var-decl-with-
    // initializer; locals + assignments come in cycle 2.
    std::unordered_map<std::uint32_t, MirInstId> symbolToValue;

    // Emit an unsupported-construct diagnostic anchored at no specific span
    // (no HirSourceMap injected yet in ML2 cycle 1; the next cycle wires it).
    void unsupported(std::string what) {
        ParseDiagnostic d;
        d.code     = DiagnosticCode::H_UnsupportedLoweringForKind;
        d.severity = DiagnosticSeverity::Error;
        d.actual   = std::move(what);
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
                // The HIR Ref's payload is the resolved SymbolId.v; the
                // lowering maintains a map from SymbolId.v to its
                // MirInstId producer (the Arg, or a VarDecl init value
                // — locals/assignments arrive in ML2 cycle 2).
                std::uint32_t const sym = hir.payload(node);
                auto it = symbolToValue.find(sym);
                if (it == symbolToValue.end()) {
                    unsupported(std::format("HIR Ref to symbol {} not yet "
                                            "supported (likely a global / "
                                            "function — ML2 cycle 2+)", sym));
                    return InvalidMirInst;
                }
                return it->second;
            }
            case HirKind::BinaryOp: {
                std::uint32_t const payload = hir.payload(node);
                if (!isCoreOp(payload)) {
                    unsupported("extension BinaryOp (post-v1)");
                    return InvalidMirInst;
                }
                HirOpKind const op = decodeCoreOp(payload);
                auto kids = hir.children(node);
                if (kids.size() != 2) {
                    unsupported("malformed BinaryOp (verifier should have "
                                "flagged this)");
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
                    unsupported(std::format("BinaryOp '{}' on TypeKind {} not "
                                            "yet supported", opName(op),
                                            static_cast<unsigned>(tk)));
                    return InvalidMirInst;
                }
                std::array<MirInstId, 2> operands{lhs, rhs};
                return mir.addInst(mop, operands, t);
            }
            default: break;
        }
        unsupported(std::format("HIR expression kind ordinal {} not yet "
                                "supported (HIR id {})",
                                static_cast<unsigned>(k), node.v));
        return InvalidMirInst;
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
            default: break;
        }
        unsupported(std::format("HIR statement kind ordinal {} not yet "
                                "supported (HIR id {})",
                                static_cast<unsigned>(k), node.v));
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
            unsupported("Function with InvalidType signature (HIR verifier "
                        "should have flagged this)");
            return false;
        }
        auto params = hir.functionParams(node);
        auto paramTypes = interner.fnParams(signature);
        if (params.size() != paramTypes.size()) {
            unsupported(std::format("Function param count {} mismatches FnSig "
                                    "param count {}", params.size(),
                                    paramTypes.size()));
            return false;
        }

        // From here on a block is open — any return-false MUST seal it.
        mir.addFunction(signature, symbol);
        MirBlockId const entry = mir.createBlock(StructCfMarker::EntryBlock);
        mir.beginBlock(entry);

        // Params: one `Arg` instruction per param, in declaration order.
        for (std::size_t i = 0; i < params.size(); ++i) {
            HirNodeId const p = params[i];
            // A param is a VarDecl whose typeId carries the param's type.
            // ML2 cycle 1 trusts the verifier; later cycles add a kind check.
            SymbolId const sym = hir.varDeclSymbol(p);
            TypeId const ty = paramTypes[i];
            MirInstId const arg = mir.addArg(static_cast<std::uint32_t>(i), ty);
            symbolToValue[sym.v] = arg;
        }
        // Body: a single Block of statements.
        HirNodeId const body = hir.functionBody(node);
        if (!lowerStmt(body)) {
            // Failed mid-body — seal the block so finish() can complete and
            // the caller sees the actual diagnostic instead of an abort.
            mir.addUnreachable();
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

    // Lower the whole module. Walks moduleDecls and dispatches per kind.
    // Anything other than Function is fail-loud-deferred to later ML2 cycles
    // (Global, ExternFunction, ExternGlobal, TypeDecl, ImportGroup).
    void lower() {
        HirNodeId const root = hir.root();
        if (!root.valid() || hir.kind(root) != HirKind::Module) {
            unsupported("HIR root is not a Module — cannot lower");
            return;
        }
        for (HirNodeId decl : hir.moduleDecls(root)) {
            HirKind const dk = hir.kind(decl);
            switch (dk) {
                case HirKind::Function:
                    if (!lowerFunction(decl)) return;
                    break;
                // Fail-loud-deferred (review-fix): the file header doc and the
                // project discipline both require unsupported HirKinds to emit
                // `H_UnsupportedLoweringForKind`, not be silently skipped. A
                // global initializer DOES have runtime effect; an extern's
                // signature DOES gate codegen later. Silent skip would mask
                // real gaps in tests of richer corpora. Each kind reopens with
                // proper handling in subsequent ML2 cycles.
                case HirKind::Global:
                    unsupported(std::format("HIR Global (id {}) not yet "
                                            "supported (ML2 cycle 2+)", decl.v));
                    return;
                case HirKind::ExternFunction:
                    unsupported(std::format("HIR ExternFunction (id {}) not yet "
                                            "supported (ML2 cycle 2+ via FFI plan 11)",
                                            decl.v));
                    return;
                case HirKind::ExternGlobal:
                    unsupported(std::format("HIR ExternGlobal (id {}) not yet "
                                            "supported (ML2 cycle 2+ via FFI plan 11)",
                                            decl.v));
                    return;
                case HirKind::TypeDecl:
                    // TypeDecl is the one structural carrier that genuinely
                    // has no MIR runtime effect — it interns a type into the
                    // lattice but emits no code. Skipping is correct here.
                    break;
                case HirKind::ImportGroup:
                    unsupported(std::format("HIR ImportGroup (id {}) not yet "
                                            "supported (ML2 cycle 2+ via plan 11)",
                                            decl.v));
                    return;
                default:
                    unsupported(std::format("Top-level HIR kind ordinal {} "
                                            "(id {}) not yet supported",
                                            static_cast<unsigned>(dk), decl.v));
                    return;
            }
        }
    }
};

} // namespace

HirToMirResult lowerToMir(Hir const&             hir,
                          HirLiteralPool const&  literals,
                          TypeInterner const&    interner,
                          DiagnosticReporter&    reporter) {
    std::size_t const errorsBefore = reporter.errorCount();
    Lowerer lwr{hir, literals, interner, reporter, MirBuilder{}, {}};
    lwr.lower();
    HirToMirResult result;
    result.mir = std::move(lwr.mir).finish();
    result.ok = (reporter.errorCount() == errorsBefore);
    return result;
}

} // namespace dss
