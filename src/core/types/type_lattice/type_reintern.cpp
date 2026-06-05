#include "core/types/type_lattice/type_reintern.hpp"

#include "core/types/type_lattice/core_type.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace dss {

namespace {

// Human-readable name for a TypeKind, for the fail-loud abort message. Covers
// every enumerator so an abort always names the offending kind precisely.
[[nodiscard]] char const* typeKindName(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:      return "Bool";
        case TypeKind::I8:        return "I8";
        case TypeKind::I16:       return "I16";
        case TypeKind::I32:       return "I32";
        case TypeKind::I64:       return "I64";
        case TypeKind::I128:      return "I128";
        case TypeKind::U8:        return "U8";
        case TypeKind::U16:       return "U16";
        case TypeKind::U32:       return "U32";
        case TypeKind::U64:       return "U64";
        case TypeKind::U128:      return "U128";
        case TypeKind::F16:       return "F16";
        case TypeKind::F32:       return "F32";
        case TypeKind::F64:       return "F64";
        case TypeKind::F128:      return "F128";
        case TypeKind::Char:      return "Char";
        case TypeKind::Byte:      return "Byte";
        case TypeKind::Void:      return "Void";
        case TypeKind::Struct:    return "Struct";
        case TypeKind::Union:     return "Union";
        case TypeKind::Tuple:     return "Tuple";
        case TypeKind::Array:     return "Array";
        case TypeKind::Slice:     return "Slice";
        case TypeKind::Enum:      return "Enum";
        case TypeKind::Vector:    return "Vector";
        case TypeKind::Matrix:    return "Matrix";
        case TypeKind::Ptr:       return "Ptr";
        case TypeKind::Ref:       return "Ref";
        case TypeKind::FnPtr:     return "FnPtr";
        case TypeKind::Nullable:  return "Nullable";
        case TypeKind::Optional:  return "Optional";
        case TypeKind::FnSig:     return "FnSig";
        case TypeKind::Param:     return "Param";
        case TypeKind::Bind:      return "Bind";
        case TypeKind::Extension: return "Extension";
        case TypeKind::Count_:    return "Count_";
    }
    return "<unknown>";
}

[[noreturn]] void reinternFatal(TypeKind k, char const* why) {
    std::fputs("dss::reinternType fatal: TypeKind ", stderr);
    std::fputs(typeKindName(k), stderr);
    std::fputs(" ", stderr);
    std::fputs(why, stderr);
    std::fputc('\n', stderr);
    std::abort();
}

// Is this primitive a leaf rebuildable purely from its kind (no operands /
// scalars / name)? Every primitive in the [Bool, Void] range qualifies.
[[nodiscard]] bool isPrimitiveKind(TypeKind k) noexcept {
    switch (k) {
        case TypeKind::Bool:
        case TypeKind::I8:  case TypeKind::I16: case TypeKind::I32:
        case TypeKind::I64: case TypeKind::I128:
        case TypeKind::U8:  case TypeKind::U16: case TypeKind::U32:
        case TypeKind::U64: case TypeKind::U128:
        case TypeKind::F16: case TypeKind::F32: case TypeKind::F64:
        case TypeKind::F128:
        case TypeKind::Char: case TypeKind::Byte: case TypeKind::Void:
            return true;
        default:
            return false;
    }
}

} // namespace

TypeId reinternType(TypeInterner const& src, TypeId srcId, TypeLattice& dstHost,
                    std::unordered_map<std::uint32_t, TypeId>& remap) {
    // Sentinel / invalid → identity. InvalidType carries no CU provenance, so it
    // is meaningful (as "no type") against any lattice.
    if (!srcId.valid()) return InvalidType;

    // Memo hit: the same source TypeId always maps to the same host TypeId.
    if (auto it = remap.find(srcId.v); it != remap.end()) return it->second;

    // ── Termination precondition: the type operand graph is an ACYCLIC DAG. ──
    // The TypeInterner has no back-patching builder — every type is interned
    // bottom-up (children before parents), so an operand TypeId always refers to
    // an ALREADY-interned (lower) id; there are no forward / mutable operand
    // edges, hence no cycles. Recursive C structs (`struct N { struct N* next; }`)
    // do NOT introduce a TypeId cycle: the field is a Ptr whose pointee is the
    // struct resolved NOMINALLY by name (TypeKind::Struct + the struct's name),
    // not a TypeId edge back to the parent. That is why memoizing AFTER the
    // recursion below (the `remap.emplace` near the end) terminates: the recursion
    // bottoms out at primitives. A FUTURE truly-recursive-type feature (forward-
    // declared / mutable operand edges) would need memo-insert-BEFORE-recursion
    // with a placeholder TypeId, then back-patch — do NOT add such a type without
    // revisiting this memo ordering.

    TypeInterner& dst        = dstHost.interner();
    TypeKind const kind      = src.kind(srcId);
    std::span<TypeId const>       srcOps    = src.operands(srcId);
    std::span<std::int64_t const> srcScalar = src.scalars(srcId);

    // Bottom-up: re-intern every operand TypeId into the host FIRST, so the
    // host already holds the children when we build the parent.
    std::vector<TypeId> ops;
    ops.reserve(srcOps.size());
    for (TypeId op : srcOps) ops.push_back(reinternType(src, op, dstHost, remap));

    TypeId result{};
    switch (kind) {
        // ── primitives: rebuilt from the kind alone ──
        case TypeKind::Bool:
        case TypeKind::I8:  case TypeKind::I16: case TypeKind::I32:
        case TypeKind::I64: case TypeKind::I128:
        case TypeKind::U8:  case TypeKind::U16: case TypeKind::U32:
        case TypeKind::U64: case TypeKind::U128:
        case TypeKind::F16: case TypeKind::F32: case TypeKind::F64:
        case TypeKind::F128:
        case TypeKind::Char: case TypeKind::Byte: case TypeKind::Void:
            result = dst.primitive(kind);
            break;

        // ── single-operand indirections: operands=[inner] ──
        case TypeKind::Ptr:       result = dst.pointer(ops[0]);   break;
        case TypeKind::Ref:       result = dst.reference(ops[0]); break;
        case TypeKind::Nullable:  result = dst.nullable(ops[0]);  break;
        case TypeKind::Optional:  result = dst.optional(ops[0]);  break;
        case TypeKind::Slice:     result = dst.slice(ops[0]);     break;

        // ── SIMD: operands=[element], scalars=[lanes] / [rows, cols] ──
        case TypeKind::Vector:
            result = dst.vector(ops[0], srcScalar[0]);
            break;
        case TypeKind::Matrix:
            result = dst.matrix(ops[0], srcScalar[0], srcScalar[1]);
            break;

        // ── array: operands=[element], scalars=[length] ──
        case TypeKind::Array:
            result = dst.array(ops[0], srcScalar[0]);
            break;

        // ── tuple: operands=[elements...] ──
        case TypeKind::Tuple:
            result = dst.tuple(ops);
            break;

        // ── nominal aggregates: name + operands=[fields/variants...] ──
        case TypeKind::Struct:
            result = dst.structType(src.name(srcId), ops);
            break;
        case TypeKind::Union:
            result = dst.unionType(src.name(srcId), ops);
            break;

        // ── enum: NO operands; name + scalars=[(int)underlyingTypeKind] ──
        case TypeKind::Enum:
            result = dst.enumType(src.name(srcId),
                                  static_cast<TypeKind>(srcScalar[0]));
            break;

        // ── fnSig: operands=[result, params...]; scalars=[(int)cc] or
        //    [(int)cc, isVariadic]. Split result vs params (the encoding the
        //    typed decoders expose) and honor the variadic flag exactly. ──
        case TypeKind::FnSig: {
            TypeId const resultTy = ops[0];                  // operands[0]
            std::span<TypeId const> params{ops.data() + 1, ops.size() - 1};
            auto const cc = static_cast<CallConv>(srcScalar[0]);
            result = dst.fnSig(params, resultTy, cc, src.fnIsVariadic(srcId));
            break;
        }

        // ── extension: extensionKind + name + operands=[typeArgs] +
        //    scalars=[scalarArgs] ──
        case TypeKind::Extension:
            result = dst.extension(src.get(srcId).extensionKind, src.name(srcId),
                                   ops, srcScalar);
            break;

        // ── never-interned kinds: no public builder exists, so these cannot
        //    legitimately appear in a TypeInterner arena. Reaching here is
        //    interner corruption — fail loud rather than silently drop them. ──
        case TypeKind::FnPtr:
        case TypeKind::Param:
        case TypeKind::Bind:
            reinternFatal(kind, "has no interner builder and cannot be "
                                "re-interned (never legitimately interned)");

        // Count_ is the enum's cardinality sentinel, not a real type.
        case TypeKind::Count_:
            reinternFatal(kind, "is the TypeKind cardinality sentinel, not a "
                                "real type");
    }

    // Defensive: any future TypeKind added without a case above would leave
    // `result` invalid here — fail loud rather than memoize a bad mapping. (The
    // switch is exhaustive today; this guards a later enum extension.)
    if (!result.valid()) {
        reinternFatal(kind, "produced no host TypeId (unhandled kind — add a "
                            "case to reinternType)");
    }
    if (isPrimitiveKind(kind) && !ops.empty()) {
        // A primitive must have no operands; if one ever does, the encoding
        // assumption above is wrong — fail loud rather than silently ignore it.
        reinternFatal(kind, "is a primitive but carried operands");
    }

    remap.emplace(srcId.v, result);
    return result;
}

} // namespace dss
