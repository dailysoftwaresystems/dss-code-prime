#include "core/types/aggregate_abi.hpp"

#include "core/types/type_lattice/core_type.hpp"     // TypeKind
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout, scalarByteSize

#include <algorithm>

namespace dss {

bool aggregateAbiImplemented(AggregateClassKind strategy) noexcept {
    // SysV AMD64 (C1) + MS x64 (C2) + AAPCS64/Apple (C3) are realized. Only the
    // `None` sentinel stays unimplemented (a CC with no by-value strategy).
    return strategy == AggregateClassKind::SysVEightbyte
        || strategy == AggregateClassKind::Win64BySize
        || strategy == AggregateClassKind::Aapcs64Hfa;
}

namespace {

struct LeafField {
    std::uint64_t offset;    // absolute byte offset within the top aggregate
    TypeKind      kind;      // a scalar/pointer leaf kind
    // D-CSUBSET-BITINT: the leaf's byte size, resolved at collection through
    // `sizeOfScalarOrBitInt` (a `_BitInt(N)`'s size needs its width scalar, which
    // `scalarByteSize(kind,...)` — kind-only — cannot see). 0 = un-sized (the
    // classifier's defensive fsz=1). Carrying it here fixes a BitInt leaf that
    // STRADDLES an eightbyte boundary: sized as 1 byte it would leave the second
    // eightbyte marked SSE (a silent GPR→XMM ABI miscompile of a by-value struct).
    std::uint64_t byteSize;
};

[[nodiscard]] bool isFloatKind(TypeKind k) noexcept {
    return k == TypeKind::F16 || k == TypeKind::F32
        || k == TypeKind::F64 || k == TypeKind::F128;
}

// Append every SCALAR leaf of `ty` at its ABSOLUTE byte offset, recursing through
// nested struct/union/array. Union members all sit at the same parent offset
// (overlapping — their fieldOffsets are all 0). Returns false if any nested
// layout is un-computable (the fail-loud signal). Type-driven only — no target
// identity.
[[nodiscard]] bool collectLeaves(TypeId ty, std::uint64_t base,
                                 TypeInterner const& in,
                                 AggregateLayoutParams lp, DataModel dm,
                                 std::vector<LeafField>& out) {
    TypeKind const k = in.kind(ty);
    if (k == TypeKind::Struct || k == TypeKind::Union) {
        auto const lay = computeLayout(ty, in, lp, dm);
        if (!lay.has_value()) return false;
        auto const ops = in.operands(ty);
        if (ops.size() != lay->fieldOffsets.size()) return false;
        for (std::size_t i = 0; i < ops.size(); ++i)
            if (!collectLeaves(ops[i], base + lay->fieldOffsets[i], in, lp, dm, out))
                return false;
        return true;
    }
    if (k == TypeKind::Array) {
        auto const ops   = in.operands(ty);
        auto const scals = in.scalars(ty);
        if (ops.empty() || scals.empty()) return false;
        TypeId const elem        = ops[0];
        std::uint64_t const count = scals[0];
        auto const elemLay = computeLayout(elem, in, lp, dm);
        if (!elemLay.has_value()) return false;
        for (std::uint64_t i = 0; i < count; ++i)
            if (!collectLeaves(elem, base + i * elemLay->size, in, lp, dm, out))
                return false;
        return true;
    }
    // D-CSUBSET-BITINT: size the leaf through the TypeId-aware shim so a
    // `_BitInt(N)` gets its CONTAINER size (not the kind-only nullopt→1 default).
    out.push_back(LeafField{base, k,
                            sizeOfScalarOrBitInt(in, ty, dm).value_or(0)});
    return true;
}

} // namespace

std::optional<AbiPassing>
classifyAggregate(AggregateClassKind strategy, std::uint16_t maxRegBytes,
                  TypeId aggTy, TypeInterner const& in,
                  AggregateLayoutParams lp, DataModel dm) {
    if (!aggregateAbiImplemented(strategy))
        return std::nullopt;   // unimplemented strategy (AAPCS64 until C3) → fail loud

    auto const lay = computeLayout(aggTy, in, lp, dm);
    if (!lay.has_value()) return std::nullopt;   // un-sizeable → fail loud
    std::uint64_t const size = lay->size;

    if (strategy == AggregateClassKind::Win64BySize) {
        // MS x64: a struct/union is passed/returned in ONE GPR iff its size is a
        // power of two ≤ maxRegBytes (1/2/4/8 bytes); every other size — 3/5/6/7,
        // or > maxRegBytes — goes BY REFERENCE (a caller-allocated copy / sret).
        // Win64 has NO SSE/HFA rule: a small aggregate is treated as an integer of
        // its size regardless of float members (the full register moves; the
        // byte-exact valid bytes are recovered at the temp/slot boundary).
        AbiPassing wout;
        bool const pow2 = size != 0 && (size & (size - 1)) == 0;
        if (pow2 && size <= maxRegBytes) {
            wout.kind = AbiPassing::Kind::InRegisters;
            wout.pieces.push_back(AbiPiece{AbiPieceClass::Gpr, 0,
                                           static_cast<std::uint32_t>(size)});
        } else {
            wout.kind = AbiPassing::Kind::ByReference;
        }
        return wout;
    }

    if (strategy == AggregateClassKind::Aapcs64Hfa) {
        // AAPCS64 §5.4 / Apple ARM64. An HFA (Homogeneous Float Aggregate) — a
        // composite whose every fundamental leaf is the SAME floating-point type,
        // 1..4 of them — is passed/returned in that many SIMD (FPR) registers,
        // each the element's width (NOT size-limited to 16B: a 4-double HFA is 32
        // bytes in v0..v3). A non-HFA aggregate ≤16B goes in 1-2 GPRs; >16B (or
        // empty) by reference. NO per-eightbyte SSE merge (that is SysV's rule).
        std::vector<LeafField> leaves;
        if (!collectLeaves(aggTy, 0, in, lp, dm, leaves))
            return std::nullopt;
        // HFA element homogeneity: every leaf is the SAME float kind. The member
        // COUNT is size/elem (NOT the leaf count) so a union of N floats — whose
        // members overlap to one element — is a 1-member HFA, while a struct/array
        // of N packs to N. count must be 1..4 and divide the size evenly.
        bool allSameFp = !leaves.empty() && isFloatKind(leaves.front().kind);
        if (allSameFp)
            for (LeafField const& f : leaves)
                if (f.kind != leaves.front().kind) { allSameFp = false; break; }
        if (allSameFp) {
            std::uint64_t const elem =
                scalarByteSize(leaves.front().kind, dm).value_or(0);
            if (elem == 0) return std::nullopt;   // un-sized FP leaf → fail loud
            if (size % elem == 0) {
                std::uint64_t const count = size / elem;
                if (count >= 1 && count <= 4) {
                    AbiPassing hout;
                    hout.kind = AbiPassing::Kind::InRegisters;
                    for (std::uint64_t i = 0; i < count; ++i)
                        hout.pieces.push_back(AbiPiece{AbiPieceClass::Fpr,
                            i * elem, static_cast<std::uint32_t>(elem)});
                    return hout;
                }
            }
        }
        // Non-HFA: ≤16B → ceil(size/8) GPR pieces (1 or 2); >16B / empty → by ref.
        AbiPassing aout;
        if (size == 0 || size > maxRegBytes) {
            aout.kind = AbiPassing::Kind::ByReference;
            return aout;
        }
        aout.kind = AbiPassing::Kind::InRegisters;
        std::size_t const n = static_cast<std::size_t>((size + 7) / 8);
        for (std::size_t e = 0; e < n; ++e) {
            std::uint64_t const eoff = e * 8;
            aout.pieces.push_back(AbiPiece{AbiPieceClass::Gpr, eoff,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(8, size - eoff))});
        }
        return aout;
    }

    // SysVEightbyte (the only other implemented strategy).
    AbiPassing out;
    // SysV §3.2.3: an aggregate larger than two eightbytes (> maxRegBytes) — or
    // empty — goes in MEMORY (by reference for args; sret for returns).
    if (size == 0 || size > maxRegBytes) {
        out.kind = AbiPassing::Kind::ByReference;
        return out;
    }

    std::vector<LeafField> leaves;
    if (!collectLeaves(aggTy, 0, in, lp, dm, leaves))
        return std::nullopt;

    std::size_t const n = static_cast<std::size_t>((size + 7) / 8);   // 1 or 2
    // Each eightbyte is SSE iff EVERY scalar field overlapping it is float; any
    // integer/pointer field in the eightbyte ⇒ INTEGER (INTEGER wins the merge).
    std::vector<bool> isInteger(n, false);
    for (LeafField const& f : leaves) {
        std::uint64_t fsz = f.byteSize;   // BitInt-aware (D-CSUBSET-BITINT)
        if (fsz == 0) fsz = 1;   // defensive: a non-sized leaf can't be SSE-clean
        std::uint64_t const lo = f.offset;
        std::uint64_t const hi = f.offset + fsz;   // [lo, hi)
        for (std::size_t e = static_cast<std::size_t>(lo / 8);
             e <= static_cast<std::size_t>((hi - 1) / 8) && e < n; ++e)
            if (!isFloatKind(f.kind)) isInteger[e] = true;
    }

    out.kind = AbiPassing::Kind::InRegisters;
    for (std::size_t e = 0; e < n; ++e) {
        std::uint64_t const eoff = e * 8;
        std::uint32_t const w =
            static_cast<std::uint32_t>(std::min<std::uint64_t>(8, size - eoff));
        out.pieces.push_back(AbiPiece{
            isInteger[e] ? AbiPieceClass::Gpr : AbiPieceClass::Fpr, eoff, w});
    }
    return out;
}

} // namespace dss
