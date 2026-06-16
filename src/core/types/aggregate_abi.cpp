#include "core/types/aggregate_abi.hpp"

#include "core/types/type_lattice/core_type.hpp"     // TypeKind
#include "core/types/type_lattice/type_layout.hpp"   // computeLayout, scalarByteSize

#include <algorithm>

namespace dss {

bool aggregateAbiImplemented(AggregateClassKind strategy) noexcept {
    // C1: only the SysV AMD64 eightbyte strategy is realized. Win64 / AAPCS64
    // are declared in config but not yet built — they stay fail-loud (C2/C3).
    return strategy == AggregateClassKind::SysVEightbyte;
}

namespace {

struct LeafField {
    std::uint64_t offset;   // absolute byte offset within the top aggregate
    TypeKind      kind;     // a scalar/pointer leaf kind
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
    out.push_back(LeafField{base, k});   // scalar/pointer leaf
    return true;
}

} // namespace

std::optional<AbiPassing>
classifyAggregate(AggregateClassKind strategy, std::uint16_t maxRegBytes,
                  TypeId aggTy, TypeInterner const& in,
                  AggregateLayoutParams lp, DataModel dm) {
    if (strategy != AggregateClassKind::SysVEightbyte)
        return std::nullopt;   // C1: only SysV; the guard fails the others loud

    auto const lay = computeLayout(aggTy, in, lp, dm);
    if (!lay.has_value()) return std::nullopt;   // un-sizeable → fail loud
    std::uint64_t const size = lay->size;

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
        std::uint64_t fsz = scalarByteSize(f.kind, dm).value_or(0);
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
