#include "core/types/type_lattice/type_layout.hpp"

#include "core/types/type_lattice/core_type.hpp"

#include <algorithm>
#include <span>

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

// Is the declared bit-field strategy one the engine actually realizes? A
// declared-but-unbuilt strategy (e.g. a future ABI value) — and `None` (not
// declared at all) — fail loud at the consumer rather than silently using a
// wrong rule (the `aggregate_abi::aggregateAbiImplemented` precedent). The
// realized set grows with each new arm in `layoutStructBitfields*` below.
[[nodiscard]] constexpr bool
bitFieldStrategyRealized(BitFieldStrategy s) noexcept {
    switch (s) {
        case BitFieldStrategy::GnuPacked:
        case BitFieldStrategy::MsvcStraddle:
            return true;
        case BitFieldStrategy::None:
            return false;
    }
    return false;
}

// ── Per-strategy struct bit-field packers (D-CSUBSET-BITFIELD-ABI-EXACT) ──
//
// Each returns the fully-populated `out` (offsets, bitFields[], align, size) for
// a struct that CONTAINS a bit-field, or nullopt on a fail-loud condition (a
// non-integer bit-field, a malformed FAM, an out-of-scope field type). The
// Struct arm dispatches to one of these by SWITCHING on the strategy enum — the
// only place the per-ABI rule is selected, and NEVER on a target/format name.
//
// Both packers share the front matter the Struct arm prepared (`out.align`
// seeded to 1, `out.fieldOffsets` reserved). They fill `out.bitFields` (one
// `BitFieldPlacement` per field; `unitBytes == 0` marks an ordinary field or a
// zero-width break) + `out.fieldOffsets` (one byte offset per field) + the
// running `out.align` + the final `out.size`.

// GnuPacked: SysV/Itanium/GNU/AAPCS64/Apple little-endian. Bits flow LSB-first
// through a single absolute `bitCursor`; a field's allocation unit is its
// declared-type size; a straddle bumps to the next unit of that type; a
// zero-width unnamed field forces the cursor to its type's unit boundary;
// different-typed adjacent bit-fields may SHARE a unit. Struct size = the bits
// actually consumed, rounded up to the struct alignment.
[[nodiscard]] std::optional<StructLayout>
layoutStructBitfieldsGnuPacked(TypeId id, std::span<TypeId const> fields,
                               TypeInterner const& interner,
                               AggregateLayoutParams params, DataModel dm,
                               StructLayout out) {
    out.bitFields.assign(fields.size(), BitFieldPlacement{});
    std::uint64_t bitCursor = 0;   // absolute bits from the struct start
    for (std::size_t i = 0; i < fields.size(); ++i) {
        TypeId const f  = fields[i];
        auto const    bw = interner.fieldBitWidth(id, i);
        if (!bw.has_value()) {
            // Ordinary field (incl. a FAM): close any open bit-unit by
            // rounding the cursor up to a byte, then to the field's align.
            if (interner.isIncompleteArray(f)) {
                if (i + 1 != fields.size()) return std::nullopt;
                auto const fops = interner.operands(f);
                if (fops.empty()) return std::nullopt;
                auto const elem = computeLayout(fops[0], interner, params, dm);
                if (!elem) return std::nullopt;
                std::uint64_t const fo =
                    elem->align.alignUp((bitCursor + 7) / 8);
                out.fieldOffsets.push_back(fo);
                out.align = maxAlign(out.align, elem->align);
                out.hasFlexibleArrayMember = true;
                bitCursor = fo * 8;   // no size contribution
                continue;
            }
            auto const fl = computeLayout(f, interner, params, dm);
            if (!fl) return std::nullopt;
            std::uint64_t const fo = fl->align.alignUp((bitCursor + 7) / 8);
            out.fieldOffsets.push_back(fo);
            out.align = maxAlign(out.align, fl->align);
            bitCursor = (fo + fl->size) * 8;
            continue;
        }
        // Bit-field: the allocation unit is its declared type's size.
        auto const fl = computeLayout(f, interner, params, dm);
        if (!fl || fl->size == 0) return std::nullopt;   // non-int bitfield → fail loud
        out.align = maxAlign(out.align, fl->align);
        std::uint64_t const unitBits = fl->size * 8;
        std::uint32_t const w        = *bw;
        if (w == 0) {
            // Zero-width unnamed bit-field: force the cursor to the next
            // unit boundary of its type; contributes no addressable field.
            bitCursor = ((bitCursor + unitBits - 1) / unitBits) * unitBits;
            out.fieldOffsets.push_back(bitCursor / 8);   // marker; unitBytes stays 0
            continue;
        }
        if (w > unitBits) return std::nullopt;   // defensive (semantic validates first)
        // If w bits at the cursor would straddle the type's unit
        // boundary, bump to the next aligned unit (the GNU rule).
        if (bitCursor / unitBits != (bitCursor + w - 1) / unitBits) {
            bitCursor = ((bitCursor + unitBits - 1) / unitBits) * unitBits;
        }
        std::uint64_t const unitByteOffset = (bitCursor / unitBits) * fl->size;
        out.fieldOffsets.push_back(unitByteOffset);
        out.bitFields[i] = BitFieldPlacement{
            static_cast<std::uint32_t>(fl->size),
            static_cast<std::uint32_t>(bitCursor % unitBits),
            w};
        bitCursor += w;
    }
    out.size = out.align.alignUp((bitCursor + 7) / 8);
    return out;
}

// MsvcStraddle: Microsoft x64 (PE). Each bit-field allocation unit is aligned to
// its declared-type natural alignment; a unit is REUSED by a following bit-field
// ONLY when the declared-type SIZE matches AND the bits fit. Any type-size
// change, intervening ordinary field, zero-width field, or straddle CLOSES the
// unit and opens a FRESH type-aligned unit at the high-water mark. Struct size
// covers the LAST unit's FULL declared-type width (so `{char a:1;}`→1,
// `{int a:1;}`→4, `{int a:1; char b:1;}`→8 with b@byte4). The rule + every
// golden below was derived empirically from cl.exe 14.51
// (D-CSUBSET-BITFIELD-ABI-EXACT conformance witness).
[[nodiscard]] std::optional<StructLayout>
layoutStructBitfieldsMsvcStraddle(TypeId id, std::span<TypeId const> fields,
                                  TypeInterner const& interner,
                                  AggregateLayoutParams params, DataModel dm,
                                  StructLayout out) {
    out.bitFields.assign(fields.size(), BitFieldPlacement{});
    std::uint64_t highWaterByte = 0;   // one past all placed content
    std::uint64_t unitTypeSize  = 0;   // bytes of the open bit-field unit (0 = none)
    std::uint64_t unitStartByte = 0;   // byte offset where the open unit begins
    std::uint64_t unitBitsUsed  = 0;   // bits consumed in the open unit
    for (std::size_t i = 0; i < fields.size(); ++i) {
        TypeId const f  = fields[i];
        auto const    bw = interner.fieldBitWidth(id, i);
        if (!bw.has_value()) {
            // Ordinary field (incl. a FAM): closes any open bit-unit; lands at
            // the next byte aligned to its own alignment.
            unitTypeSize = 0;   // close the unit (MSVC never reopens it)
            if (interner.isIncompleteArray(f)) {
                if (i + 1 != fields.size()) return std::nullopt;
                auto const fops = interner.operands(f);
                if (fops.empty()) return std::nullopt;
                auto const elem = computeLayout(fops[0], interner, params, dm);
                if (!elem) return std::nullopt;
                std::uint64_t const fo = elem->align.alignUp(highWaterByte);
                out.fieldOffsets.push_back(fo);
                out.align = maxAlign(out.align, elem->align);
                out.hasFlexibleArrayMember = true;
                highWaterByte = fo;   // no size contribution
                continue;
            }
            auto const fl = computeLayout(f, interner, params, dm);
            if (!fl) return std::nullopt;
            std::uint64_t const fo = fl->align.alignUp(highWaterByte);
            out.fieldOffsets.push_back(fo);
            out.align = maxAlign(out.align, fl->align);
            highWaterByte = fo + fl->size;
            continue;
        }
        // Bit-field: the allocation unit is its declared type's size, aligned to
        // its declared type's natural alignment.
        auto const fl = computeLayout(f, interner, params, dm);
        if (!fl || fl->size == 0) return std::nullopt;   // non-int bitfield → fail loud
        out.align = maxAlign(out.align, fl->align);
        std::uint64_t const t        = fl->size;          // unit type size (bytes)
        std::uint64_t const unitBits = t * 8;
        std::uint32_t const w        = *bw;
        if (w == 0) {
            // Zero-width unnamed bit-field: break the run AND force the high-water
            // to this type's natural-unit boundary; no addressable field. (Next
            // real field then opens fresh, re-aligned to ITS own type.)
            unitTypeSize  = 0;
            highWaterByte = fl->align.alignUp(highWaterByte);
            out.fieldOffsets.push_back(highWaterByte);   // marker; unitBytes stays 0
            continue;
        }
        if (w > unitBits) return std::nullopt;   // defensive (semantic validates first)
        bool const canContinue =
            (unitTypeSize == t) && (unitBitsUsed + w <= unitBits);
        if (!canContinue) {
            // Open a fresh unit of this type at the next type-aligned byte.
            unitStartByte = fl->align.alignUp(highWaterByte);
            unitTypeSize  = t;
            unitBitsUsed  = 0;
        }
        out.fieldOffsets.push_back(unitStartByte);
        out.bitFields[i] = BitFieldPlacement{
            static_cast<std::uint32_t>(t),
            static_cast<std::uint32_t>(unitBitsUsed),
            w};
        unitBitsUsed += w;
        // The struct must cover the FULL declared-type width of every opened unit.
        highWaterByte = std::max(highWaterByte, unitStartByte + t);
    }
    out.size = out.align.alignUp(highWaterByte);
    return out;
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
    // c27 (D-CSUBSET-VOLATILE-POINTEE): a `volatile T` has the SAME layout as T
    // (C 6.7.3 — a qualifier never changes size/alignment). Strip the VolatileQual
    // skin ONCE here so the whole engine — incl. the raw-kind incomplete checks
    // below and the recursive field/element layouts — operates on the material
    // type. This single strip makes `sizeof(volatile T) == sizeof(T)` hold by
    // construction and routes a volatile-qualified struct/array/scalar down its
    // normal arm. (The transparent `kind()`/`operands()` would mostly suffice, but
    // `isIncompleteComposite`/`isIncompleteArray` read the RAW record kind.)
    id = interner.stripVolatile(id);
    TypeKind const kind = interner.kind(id);

    // D-CSUBSET-SELF-REFERENTIAL-STRUCT: an INCOMPLETE composite (a forward-declared
    // struct/union whose body has not been seen) has NO size — `sizeof` of it, or a
    // by-value member of it, is ill-formed (C 6.5.3.4 / 6.7.2.1). Fail loud (nullopt
    // → positioned diagnostic), never a guessed/zero size. This is also the backstop
    // that keeps layout from recursing on a self-by-value cycle (the semantic phase
    // leaves such a composite incomplete after emitting S_IncompleteTypeMember).
    if (interner.isIncompleteComposite(id)) return std::nullopt;

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
            // FC8 bitfields (D-CSUBSET-BITFIELD): a bitfield-free struct interns
            // with EMPTY scalars (see TypeInterner::structType), so this O(1) test
            // routes every existing struct down the unchanged byte path below.
            bool const anyBitfield = !interner.scalars(id).empty();
            if (!anyBitfield) {
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
            // ── bit-field packing path (D-CSUBSET-BITFIELD-ABI-EXACT) ──
            // The per-ABI rule is config-SELECTED by switching ONLY on the
            // strategy enum — NEVER on a target/format name (the per-ABI value
            // is resolved upstream from the active object FORMAT). A struct that
            // contains a bit-field but whose strategy is unrealized (`None` = not
            // declared, or a future un-built value) FAILS LOUD here (nullopt →
            // positioned diag) — no silent fallback can bake a wrong placement.
            switch (params.bitFieldStrategy) {
                case BitFieldStrategy::GnuPacked:
                    return layoutStructBitfieldsGnuPacked(
                        id, fields, interner, params, dm, std::move(out));
                case BitFieldStrategy::MsvcStraddle:
                    return layoutStructBitfieldsMsvcStraddle(
                        id, fields, interner, params, dm, std::move(out));
                case BitFieldStrategy::None:
                    return std::nullopt;   // not declared → fail loud
            }
            return std::nullopt;   // unrealized strategy → fail loud
        }
        case TypeKind::Union: {
            auto const fields = interner.operands(id);
            StructLayout out{};
            out.align = Alignment::of<1>();
            out.fieldOffsets.assign(fields.size(), 0);  // every variant at offset 0
            // FC8 bitfields: a union bit-field member occupies bits [0, W) of its
            // OWN allocation unit at offset 0 (members are independent). This
            // placement is identical under EVERY realized strategy — a lone
            // member never straddles, never has a type-transition neighbour, and
            // gnu_packed/msvc_straddle agree on a single field's unit — so the
            // arm needs only the fail-loud gate (an unrealized/undeclared
            // strategy → nullopt), not a per-strategy dispatch. Empty scalars ⇒
            // no bitfield ⇒ the unchanged byte path.
            bool const anyBitfield = !interner.scalars(id).empty();
            if (anyBitfield) {
                if (!bitFieldStrategyRealized(params.bitFieldStrategy))
                    return std::nullopt;
                out.bitFields.assign(fields.size(), BitFieldPlacement{});
            }
            std::uint64_t maxSize = 0;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                auto const fl = computeLayout(fields[i], interner, params, dm);
                if (!fl) return std::nullopt;
                out.align = maxAlign(out.align, fl->align);
                if (anyBitfield) {
                    auto const bw = interner.fieldBitWidth(id, i);
                    if (bw.has_value() && *bw > 0) {
                        if (fl->size == 0) return std::nullopt;
                        out.bitFields[i] = BitFieldPlacement{
                            static_cast<std::uint32_t>(fl->size), 0, *bw};
                    }
                }
                maxSize = std::max(maxSize, fl->size);
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
