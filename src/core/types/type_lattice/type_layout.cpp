#include "core/types/type_lattice/type_layout.hpp"

#include "core/types/type_lattice/core_type.hpp"

#include <algorithm>

namespace dss {

namespace {

// Pointer byte width under a data model (the one OS-dependent layout dimension —
// every other scalar's width is already baked into its TypeKind by FC3).
[[nodiscard]] std::uint64_t pointerBytes(DataModel dm) noexcept {
    switch (dm) {
        case DataModel::Lp64:
        case DataModel::Llp64: return 8;
        case DataModel::Ilp32: return 4;
    }
    return 8;  // unreachable for a valid model
}

// The alignment of a scalar/pointer of `size` bytes under the params. The bounded
// natural-alignment rule: align = min(size, maxAlignment). Both operands are
// powers of two (scalar sizes ∈ {1,2,4,8,16}; maxAlignment is loader-validated
// pow2), so the min is a power of two — `ofRuntimePow2` is exact.
[[nodiscard]] Alignment scalarAlign(std::uint64_t size,
                                    AggregateLayoutParams params) noexcept {
    // `ScalarAlignmentRule::Natural` is the only rule today; a future non-natural
    // ABI adds a member here (an `Explicit` per-primitive table), never a
    // target-name branch.
    std::uint64_t const a =
        std::min<std::uint64_t>(size == 0 ? 1 : size, params.maxAlignment);
    return Alignment::ofRuntimePow2(static_cast<std::uint32_t>(a == 0 ? 1 : a));
}

// The stricter (larger) of two alignments.
[[nodiscard]] Alignment maxAlign(Alignment a, Alignment b) noexcept {
    return a.bytes() >= b.bytes() ? a : b;
}

} // namespace

std::optional<std::uint64_t> scalarByteSize(TypeKind kind, DataModel dm) noexcept {
    switch (kind) {
        // 1-byte: C `_Bool`, `char` (signed/unsigned char map to I8/U8), I8/U8, Byte.
        case TypeKind::Bool: case TypeKind::I8: case TypeKind::U8:
        case TypeKind::Char: case TypeKind::Byte:
            return 1;
        case TypeKind::I16: case TypeKind::U16: case TypeKind::F16:
            return 2;
        case TypeKind::I32: case TypeKind::U32: case TypeKind::F32:
            return 4;
        case TypeKind::I64: case TypeKind::U64: case TypeKind::F64:
            return 8;
        case TypeKind::I128: case TypeKind::U128: case TypeKind::F128:
            return 16;
        // Pointer-class scalars take the model's pointer width.
        case TypeKind::Ptr: case TypeKind::Ref: case TypeKind::FnPtr:
            return pointerBytes(dm);
        // Not a sized scalar: aggregates are handled by `computeLayout`; Void and
        // the out-of-scope kinds (FnSig/Slice/Tuple/Vector/Matrix/Nullable/
        // Optional/Param/Bind/Enum/Struct/Union/Array/Extension) return nullopt —
        // the caller's fail-loud signal.
        default:
            return std::nullopt;
    }
}

std::optional<StructLayout>
computeLayout(TypeId id, TypeInterner const& interner,
              AggregateLayoutParams params, DataModel dm) {
    TypeKind const kind = interner.kind(id);

    // Scalars + pointers: degenerate layout (no field offsets).
    if (auto const sz = scalarByteSize(kind, dm)) {
        return StructLayout{*sz, scalarAlign(*sz, params), {}, false};
    }

    switch (kind) {
        case TypeKind::Enum: {
            // size/align = the underlying integer primitive (scalars[0] = kind).
            auto const sc = interner.scalars(id);
            if (sc.empty()) return std::nullopt;
            auto const under = static_cast<TypeKind>(sc[0]);
            auto const sz = scalarByteSize(under, dm);
            if (!sz) return std::nullopt;
            return StructLayout{*sz, scalarAlign(*sz, params), {}, false};
        }
        case TypeKind::Array: {
            // A bare flexible/incomplete array `T[]` has NO standalone size — it is
            // only legal as a struct's last field (handled in the Struct arm).
            if (interner.isIncompleteArray(id)) return std::nullopt;
            auto const ops = interner.operands(id);
            auto const sc  = interner.scalars(id);
            if (ops.empty() || sc.empty() || sc[0] < 0) return std::nullopt;
            auto const elem = computeLayout(ops[0], interner, params, dm);
            if (!elem) return std::nullopt;
            std::uint64_t const stride = elem->align.alignUp(elem->size);
            std::uint64_t const len    = static_cast<std::uint64_t>(sc[0]);
            return StructLayout{stride * len, elem->align, {}, false};
        }
        case TypeKind::Struct: {
            auto const fields = interner.operands(id);
            StructLayout out{};
            out.align = Alignment::of<1>();
            out.fieldOffsets.reserve(fields.size());
            std::uint64_t off = 0;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                TypeId const f = fields[i];
                // A flexible array member takes its element's alignment + an
                // offset, but contributes ZERO to the struct size (the unsized
                // tail). It is legal ONLY as the LAST field — a non-last FAM is
                // malformed (the following fields would silently overlay the
                // unsized tail), so fail loud rather than mislay them.
                if (interner.isIncompleteArray(f)) {
                    if (i + 1 != fields.size()) return std::nullopt;
                    auto const fops = interner.operands(f);
                    if (fops.empty()) return std::nullopt;
                    auto const elem = computeLayout(fops[0], interner, params, dm);
                    if (!elem) return std::nullopt;
                    off = elem->align.alignUp(off);
                    out.fieldOffsets.push_back(off);
                    out.align = maxAlign(out.align, elem->align);
                    out.hasFlexibleArrayMember = true;
                    continue;  // no size contribution
                }
                auto const fl = computeLayout(f, interner, params, dm);
                if (!fl) return std::nullopt;   // out-of-scope field type → fail loud
                off = fl->align.alignUp(off);
                out.fieldOffsets.push_back(off);
                off += fl->size;
                out.align = maxAlign(out.align, fl->align);
            }
            out.size = out.align.alignUp(off);
            return out;
        }
        case TypeKind::Union: {
            auto const fields = interner.operands(id);
            StructLayout out{};
            out.align = Alignment::of<1>();
            out.fieldOffsets.assign(fields.size(), 0);  // every variant at offset 0
            std::uint64_t maxSize = 0;
            for (TypeId const f : fields) {
                auto const fl = computeLayout(f, interner, params, dm);
                if (!fl) return std::nullopt;
                maxSize   = std::max(maxSize, fl->size);
                out.align = maxAlign(out.align, fl->align);
            }
            out.size = out.align.alignUp(maxSize);
            return out;
        }
        default:
            // Void + out-of-scope kinds (FnSig/Slice/Tuple/Vector/Matrix/Nullable/
            // Optional/Param/Bind/Extension): no C-aggregate layout. Fail loud
            // (nullopt) — NEVER a guessed size.
            return std::nullopt;
    }
}

} // namespace dss
