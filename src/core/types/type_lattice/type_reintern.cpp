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
        case TypeKind::F80:       return "F80";
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
        case TypeKind::VolatileQual: return "VolatileQual";
        case TypeKind::NullptrT:  return "NullptrT";
        case TypeKind::BitInt:    return "BitInt";
        case TypeKind::Complex:   return "Complex";
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
        case TypeKind::F80: case TypeKind::F128:
        case TypeKind::Char: case TypeKind::Byte: case TypeKind::Void:
        // C23 nullptr_t: an operand-less scalar kind — reinterns via the
        // `dst.primitive(kind)` arm (mirrored in the rebuild switch below).
        case TypeKind::NullptrT:
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

    TypeInterner& dst        = dstHost.interner();
    // c27 (D-CSUBSET-VOLATILE-POINTEE): read the RAW record kind, NOT the
    // transparent `kind()` — `kind()` sees THROUGH a VolatileQual skin to the
    // material kind, so using it here would reintern a `volatile T` AS a plain `T`,
    // silently DROPPING the qualifier (a miscompile in the cross-CU merge / text
    // round-trip). The raw kind preserves VolatileQual so the wrapper round-trips.
    TypeKind const kind      = src.get(srcId).kind;

    // ── type qualifiers (D-CSUBSET-VOLATILE-POINTEE / D-CSUBSET-QUAL-BITSET) ──
    // A VolatileQual wraps exactly ONE inner type + a QualBit mask. Re-intern the
    // inner into the host, then re-wrap with the SAME mask. Handled HERE (before the
    // transparent operand read below) because `src.operands(VolatileQual(T))`
    // redirects to T's operands (NOT [T]); `stripVolatile` recovers the material
    // inner (the skin never nests). Re-wrap via `qualified(inner, bits)`, NOT
    // `volatileQualified` — the latter sets only the Volatile bit and would DROP an
    // `_Atomic` (or `_Atomic volatile`) qualifier on this cross-CU merge / text
    // round-trip, a silent loss-of-atomicity miscompile.
    if (kind == TypeKind::VolatileQual) {
        TypeId const inner  = reinternType(src, src.stripVolatile(srcId),
                                           dstHost, remap);
        TypeId const result = dst.qualified(inner, src.qualifierBits(srcId));
        remap.emplace(srcId.v, result);
        return result;
    }

    // ── NOMINAL composites (D-CSUBSET-SELF-REFERENTIAL-STRUCT) ──
    // A composite's field list may CONTAIN A CYCLE — a self-referential
    // `struct N { struct N *next; }` is a Ptr whose pointee is N's OWN TypeId.
    // Memoizing only AFTER recursing the fields (the DAG path below) would loop
    // forever on that edge. So FORWARD-MINT the host composite, INSERT it into the
    // memo BEFORE recursing the fields, THEN re-intern the fields and complete it —
    // the self-ref field's recursion hits the memo and resolves to the placeholder.
    // Host identity is keyed per SOURCE composite (its CU tag + index) so distinct
    // source composites stay distinct and the same source composite is stable.
    if (kind == TypeKind::Struct || kind == TypeKind::Union) {
        std::uint64_t const declSiteKey =
            (static_cast<std::uint64_t>(srcId.arenaTag) << 32)
            | static_cast<std::uint64_t>(srcId.v);
        TypeId const fwd =
            dst.forwardComposite(kind, src.name(srcId), declSiteKey);
        remap.emplace(srcId.v, fwd);   // BEFORE recursion → breaks the cycle
        // An INCOMPLETE source composite (forward-declared, never defined) stays
        // incomplete in the host: re-intern nothing, leave the placeholder.
        if (src.isIncompleteComposite(srcId)) return fwd;
        std::span<TypeId const>       srcFields = src.operands(srcId);
        std::span<std::int64_t const> srcWidths = src.scalars(srcId);
        std::vector<TypeId> fields;
        fields.reserve(srcFields.size());
        for (TypeId f : srcFields)
            fields.push_back(reinternType(src, f, dstHost, remap));
        // Decode the (width+1)/0 scalar form back to the kNotBitfield/width form
        // completeComposite re-encodes (round-trip identity for a bitfield-free
        // composite, whose scalars are empty → no per-field widths).
        std::vector<std::int64_t> widths;
        if (!srcWidths.empty()) {
            widths.reserve(srcWidths.size());
            for (std::int64_t enc : srcWidths)
                widths.push_back(enc <= 0 ? kNotBitfield : enc - 1);
        }
        // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): carry EXPLICIT field offsets across
        // reintern — without this the reinterned composite loses its overlapping
        // layout (HighPart falls back to natural offset 8) and forks the TypeId that
        // `.member` scope keys on. Empty when the source lays out naturally.
        std::vector<std::uint64_t> offsets;
        if (src.hasExplicitOffsets(srcId)) {
            offsets.reserve(srcFields.size());
            for (std::size_t i = 0; i < srcFields.size(); ++i)
                offsets.push_back(src.explicitFieldOffset(srcId, i).value_or(0));
        }
        // D-CSUBSET-MEMBER-ALIGNAS: carry member-alignas overrides across reintern —
        // without this the reinterned composite loses its declared field alignment
        // (falls back to natural) and forks the TypeId. Empty when the source aligns
        // naturally. A source struct can carry offsets OR aligns but not both
        // (completeComposite rejects the pair), so exactly one span is non-empty.
        std::vector<std::uint32_t> aligns;
        if (src.hasExplicitAligns(srcId)) {
            aligns.reserve(srcFields.size());
            for (std::size_t i = 0; i < srcFields.size(); ++i)
                aligns.push_back(src.explicitFieldAlign(srcId, i));
        }
        // D-CSUBSET-PACKED: carry the whole-composite packed flag across reintern.
        // Without it a packed struct crossing a CU/round-trip boundary silently
        // reinterns as UNPACKED — a silent ABI miscompile (the exact reason the
        // `completeComposite` packed parameter is non-defaulted: this call FAILS TO
        // COMPILE if packed is forgotten). packed + explicit offsets never coexist
        // (completeComposite rejects the pair), so `offsets` is empty when packed.
        dst.completeComposite(fwd, fields, src.isPacked(srcId), widths, offsets, aligns);
        return fwd;
    }

    // ── Termination precondition for the NON-composite DAG: ACYCLIC. ──
    // Every non-composite type is interned bottom-up (children before parents), so
    // an operand TypeId always refers to an ALREADY-interned (lower) id; there are
    // no forward / mutable operand edges among them, hence no cycles. (Composites —
    // the only types that CAN cycle — are handled above with memo-before-recursion.)
    // The GuardedSpan results decay to raw spans here (D-TYPEINTERNER-OPERAND-
    // SPAN-LIFETIME-GUARD): SAFE — `src` is `const` and every intern below targets
    // `dst`, a DISTINCT interner, so `src`'s pools are never mutated while these
    // views are live (the dangling hazard the guard exists for cannot arise).
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
        case TypeKind::F80: case TypeKind::F128:
        case TypeKind::Char: case TypeKind::Byte: case TypeKind::Void:
        case TypeKind::NullptrT:   // C23 nullptr_t: operand-less primitive scalar
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

        // ── C23 _BitInt(N): NO operands; scalars=[N, signed] (D-CSUBSET-BITINT).
        //    Rebuild via the width+signedness builder so a `_BitInt(N)` crossing a
        //    CU / text round-trip keeps its EXACT width and signedness (a dropped
        //    signedness would silently flip the wrap/compare semantics). ──
        case TypeKind::BitInt:
            result = dst.bitInt(srcScalar[0], srcScalar.size() > 1 && srcScalar[1] != 0);
            break;

        // ── C99 _Complex: operands=[element]; NO scalars (D-CSUBSET-COMPLEX).
        //    Rebuild via the element builder so a `_Complex` crossing a CU / text
        //    round-trip keeps its exact element float type. ──
        case TypeKind::Complex:
            result = dst.complex(ops[0]);
            break;

        // ── tuple: operands=[elements...] ──
        case TypeKind::Tuple:
            result = dst.tuple(ops);
            break;

        // ── nominal aggregates: handled ABOVE (forward-mint + complete, the
        //    cycle-safe path). Reaching here means the early composite return
        //    was bypassed — interner/control-flow corruption. Fail loud. ──
        case TypeKind::Struct:
        case TypeKind::Union:
            reinternFatal(kind, "is a nominal composite and must be re-interned via "
                                "the forward-mint path, not the operand-DAG switch");

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
