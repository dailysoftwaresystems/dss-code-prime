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

// D-CSUBSET-MEMBER-ALIGNAS: fold field `i`'s explicit `alignas` override into its
// natural alignment with MAX semantics — the SAME rule the non-bitfield struct/union
// arm's `effectiveAlign` lambda applies, hoisted here so BOTH bit-field packers honor
// a member `alignas` on their ORDINARY (non-bit-field) fields (a bit-field FIELD itself
// can't carry alignas — the semantic phase rejects it). `alignas` on a non-bit-field
// member of a bit-field-bearing struct is legal C11 6.7.5 and RAISES that field's (and
// thus the struct's) alignment — it must never be silently dropped. Returns the natural
// alignment unchanged when the struct carries no aligns or this field has no override;
// `nullopt` when a stored override is not a power of two in [1, 256] (an upstream
// alignas-semantics bug — fail loud rather than silently mis-pad, mirroring the
// non-bit-field path's `Alignment::fromBytes` reject).
[[nodiscard]] std::optional<Alignment>
bitfieldPackerEffectiveAlign(TypeInterner const& interner, TypeId id,
                             std::size_t i, Alignment natural) {
    if (!interner.hasExplicitAligns(id)) return natural;
    std::uint32_t const ovr = interner.explicitFieldAlign(id, i);
    if (ovr == 0) return natural;   // no override on this field
    auto const a = Alignment::fromBytes(ovr);
    if (!a) return std::nullopt;
    return maxAlign(natural, *a);
}

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
                // A FAM can carry `alignas` (`alignas(16) int fam[];`): raise its
                // effective alignment exactly as the non-bit-field path does.
                auto const ea =
                    bitfieldPackerEffectiveAlign(interner, id, i, elem->align);
                if (!ea) return std::nullopt;
                std::uint64_t const fo = ea->alignUp((bitCursor + 7) / 8);
                out.fieldOffsets.push_back(fo);
                out.align = maxAlign(out.align, *ea);
                out.hasFlexibleArrayMember = true;
                bitCursor = fo * 8;   // no size contribution
                continue;
            }
            auto const fl = computeLayout(f, interner, params, dm);
            if (!fl) return std::nullopt;
            // Fold a member `alignas` override into the ordinary field's alignment
            // (D-CSUBSET-MEMBER-ALIGNAS) — a bit-field-bearing struct's non-bit-field
            // member may be over-aligned; the offset AND the struct align must honor it.
            auto const ea = bitfieldPackerEffectiveAlign(interner, id, i, fl->align);
            if (!ea) return std::nullopt;
            std::uint64_t const fo = ea->alignUp((bitCursor + 7) / 8);
            out.fieldOffsets.push_back(fo);
            out.align = maxAlign(out.align, *ea);
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
                // A FAM can carry `alignas`: raise its effective alignment
                // (D-CSUBSET-MEMBER-ALIGNAS), mirroring the non-bit-field path.
                auto const ea =
                    bitfieldPackerEffectiveAlign(interner, id, i, elem->align);
                if (!ea) return std::nullopt;
                std::uint64_t const fo = ea->alignUp(highWaterByte);
                out.fieldOffsets.push_back(fo);
                out.align = maxAlign(out.align, *ea);
                out.hasFlexibleArrayMember = true;
                highWaterByte = fo;   // no size contribution
                continue;
            }
            auto const fl = computeLayout(f, interner, params, dm);
            if (!fl) return std::nullopt;
            // Fold a member `alignas` override into the ordinary field's alignment
            // (D-CSUBSET-MEMBER-ALIGNAS) — legal on a non-bit-field member of a
            // bit-field-bearing struct; the offset AND struct align must honor it.
            auto const ea = bitfieldPackerEffectiveAlign(interner, id, i, fl->align);
            if (!ea) return std::nullopt;
            std::uint64_t const fo = ea->alignUp(highWaterByte);
            out.fieldOffsets.push_back(fo);
            out.align = maxAlign(out.align, *ea);
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
        // FC17.9(e) (D-CSUBSET-LONG-DOUBLE): F80 (x87 80-bit) STORES as 16/16 —
        // x86_64-SysV and darwin-x86_64 both pad the 10 significant bytes to a
        // 16-byte, 16-aligned slot (the same size/align binary128 uses).
        case TypeKind::I128: case TypeKind::U128: case TypeKind::F80:
        case TypeKind::F128:
            return 16;
        // Pointer-class scalars take the model's pointer width. C23 nullptr_t has
        // the same size/representation as `void*` (§6.2.5), so `sizeof(nullptr)`
        // is the pointer width — even though the `nullptr` value itself lowers to
        // the integer-0 null constant and never materializes as a NullptrT value.
        case TypeKind::Ptr: case TypeKind::Ref: case TypeKind::FnPtr:
        case TypeKind::NullptrT:
            return pointerBytes(dm);
        // Not a sized scalar: aggregates are handled by `computeLayout`; Void and
        // the out-of-scope kinds (FnSig/Slice/Tuple/Vector/Matrix/Nullable/
        // Optional/Param/Bind/Enum/Struct/Union/Array/Extension) return nullopt —
        // the caller's fail-loud signal.
        default:
            return std::nullopt;
    }
}

std::optional<std::uint64_t>
sizeOfScalarOrBitInt(TypeInterner const& interner, TypeId id, DataModel dm) noexcept {
    if (!id.valid()) return std::nullopt;
    TypeKind const k = interner.kind(id);
    if (k != TypeKind::BitInt) return scalarByteSize(k, dm);
    // C23 _BitInt(N) (D-CSUBSET-BITINT): the CONTAINER size (params-independent, so
    // no AggregateLayoutParams needed) — mirrors the `computeLayout` BitInt arm's
    // size ladder exactly (N≤64 → {1,2,4,8}; N>64 → ceil(N/64) eightbytes).
    std::int64_t const n = interner.bitIntWidth(id);
    if (n <= 0) return std::nullopt;
    std::uint64_t const bits = static_cast<std::uint64_t>(n);
    if (bits <= 8)  return 1;
    if (bits <= 16) return 2;
    if (bits <= 32) return 4;
    if (bits <= 64) return 8;
    return ((bits + 63) / 64) * 8;
}

bool isWideBitInt(TypeInterner const& interner, TypeId id) noexcept {
    return id.valid() && interner.kind(id) == TypeKind::BitInt
        && interner.bitIntWidth(id) > 64;
}

bool isMemoryResidentType(TypeInterner const& interner, TypeId id) noexcept {
    if (!id.valid()) return false;
    switch (interner.kind(id)) {
        case TypeKind::Struct:
        case TypeKind::Union:
        case TypeKind::Array:
            return true;
        case TypeKind::BitInt:
            return interner.bitIntWidth(id) > 64;   // wide _BitInt is multi-limb
        default:
            return false;
    }
}

bool isByValueClass(TypeInterner const& interner, TypeId id) noexcept {
    if (!id.valid()) return false;
    switch (interner.kind(id)) {
        case TypeKind::Struct:
        case TypeKind::Union:
            return true;
        case TypeKind::BitInt:
            return interner.bitIntWidth(id) > 64;   // ARRAY excluded (it decays)
        default:
            return false;
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
        case TypeKind::BitInt: {
            // C23 _BitInt(N) (D-CSUBSET-BITINT): the ABI layout. scalars[0] = N bits.
            // N≤64 → the smallest native container {1,2,4,8}B, align == size (matches
            // gcc/clang: sizeof(_BitInt(4))==1, _BitInt(17)==4, _BitInt(40)==8). N>64
            // (C2 multi-limb; C1 rejects it at semantic, but layout is ready) →
            // ceil(N/64) eightbytes, align 8 per the x86-64 psABI — sizeof(_BitInt(128))
            // ==16 but _Alignof==8 (NOT 16). A non-positive N is malformed (the
            // semantic gate rejects it first) → fail loud (nullopt).
            auto const sc = interner.scalars(id);
            if (sc.empty() || sc[0] <= 0) return std::nullopt;
            std::uint64_t const n = static_cast<std::uint64_t>(sc[0]);
            std::uint64_t size;
            std::uint64_t alignBytes;
            if (n <= 8)       { size = 1; alignBytes = 1; }
            else if (n <= 16) { size = 2; alignBytes = 2; }
            else if (n <= 32) { size = 4; alignBytes = 4; }
            else if (n <= 64) { size = 8; alignBytes = 8; }
            else {
                size       = ((n + 63) / 64) * 8;   // ceil(N/64) eightbytes
                alignBytes = 8;                       // psABI: align 8 even at N>64
            }
            return StructLayout{size, scalarAlign(alignBytes, params), {}, false};
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
            // c107 (D-FFI-DESCRIPTOR-UNION-OVERLAY): a struct carrying EXPLICIT
            // per-field byte offsets (an FFI overlapping-union modeled as a struct)
            // uses those offsets verbatim instead of natural-alignment derivation.
            // Offsets may OVERLAP (ULARGE_INTEGER {QuadPart@0, LowPart@0, HighPart@4}):
            // size = the max field extent, align = the max field alignment. This is
            // a SEPARATE channel from bitfields (offsets are not in scalars — F1), so
            // an explicit-offset struct never carries bit-fields; a config that pairs
            // them is rejected here (fail loud) rather than silently mis-laid.
            if (interner.hasExplicitOffsets(id)) {
                if (!interner.scalars(id).empty()) return std::nullopt;  // bitfields + offsets: unsupported
                std::uint64_t extent = 0;
                for (std::size_t i = 0; i < fields.size(); ++i) {
                    auto const off = interner.explicitFieldOffset(id, i);
                    if (!off) return std::nullopt;                 // partial offsets: malformed
                    auto const fl = computeLayout(fields[i], interner, params, dm);
                    if (!fl) return std::nullopt;
                    out.fieldOffsets.push_back(*off);
                    out.align = maxAlign(out.align, fl->align);
                    extent = std::max(extent, *off + fl->size);
                }
                out.size = out.align.alignUp(extent);
                return out;
            }
            // D-CSUBSET-PACKED: the whole-composite packed flag (C/C23
            // `__attribute__((packed))`) removes ALL derived inter-field padding —
            // the per-field baseline alignment becomes 1 — and the aggregate's own
            // alignment stays 1 (out.align already seeded to 1). Read once; fed into
            // `effectiveAlign`'s baseline below. An UNPACKED struct leaves `packed`
            // false and the baseline is the field's natural align (the unchanged path).
            bool const packed = interner.isPacked(id);
            // FC8 bitfields (D-CSUBSET-BITFIELD): a bitfield-free struct interns
            // with EMPTY scalars (see TypeInterner::structType), so this O(1) test
            // routes every existing struct down the unchanged byte path below.
            bool const anyBitfield = !interner.scalars(id).empty();
            // D-CSUBSET-PACKED F5 belt (D-CSUBSET-PACKED-BITFIELD-INTERACTION): the
            // packed baseline is applied ONLY on the non-bit-field path (the
            // `effectiveAlign` lambda below). A packed struct reaching the bit-field
            // packer would silently get a NON-packed layout, so fail loud here
            // (nullopt → positioned diag). The semantic `S_PackedBitfieldUnsupported`
            // is the good-UX front door; this nullopt is the reliable backstop.
            if (packed && anyBitfield) return std::nullopt;
            if (!anyBitfield) {
                // D-CSUBSET-MEMBER-ALIGNAS: a struct may carry per-field `alignas`
                // overrides. Read them once here; `effectiveAlign` folds field i's
                // override (when non-zero) into its natural alignment with MAX
                // semantics — a member alignas RAISES a field's alignment, never
                // lowers it. An align-free struct (the common case) leaves
                // `hasAligns` false and this collapses to the unchanged path below.
                bool const hasAligns = interner.hasExplicitAligns(id);
                auto effectiveAlign =
                    [&](std::size_t i, Alignment natural) -> std::optional<Alignment> {
                    // D-CSUBSET-PACKED: a packed composite removes all derived padding
                    // — the per-field BASELINE alignment becomes 1. A member `alignas`
                    // still RAISES it via the MAX-fold below (alignas wins per-field
                    // even under packed — `alignas(8) int x;` in a packed struct keeps
                    // 8-byte alignment).
                    Alignment const baseline = packed ? Alignment::of<1>() : natural;
                    if (!hasAligns) return baseline;
                    std::uint32_t const ovr = interner.explicitFieldAlign(id, i);
                    if (ovr == 0) return baseline;   // no override on this field
                    // A stored override must be a power of two in [1, 256]; a value
                    // outside that is an upstream (alignas-semantics) bug — fail loud
                    // rather than silently mis-pad.
                    auto const a = Alignment::fromBytes(ovr);
                    if (!a) return std::nullopt;
                    return maxAlign(baseline, *a);
                };
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
                        // A FAM can carry alignas (`alignas(16) int fam[];`): raise
                        // its effective alignment the same way as an ordinary field.
                        auto const ea = effectiveAlign(i, elem->align);
                        if (!ea) return std::nullopt;
                        off = ea->alignUp(off);
                        out.fieldOffsets.push_back(off);
                        out.align = maxAlign(out.align, *ea);
                        out.hasFlexibleArrayMember = true;
                        continue;  // no size contribution
                    }
                    auto const fl = computeLayout(f, interner, params, dm);
                    if (!fl) return std::nullopt;   // out-of-scope field type → fail loud
                    auto const ea = effectiveAlign(i, fl->align);
                    if (!ea) return std::nullopt;
                    off = ea->alignUp(off);
                    out.fieldOffsets.push_back(off);
                    off += fl->size;
                    out.align = maxAlign(out.align, *ea);
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
            // D-CSUBSET-PACKED: a packed union (`union {…} __attribute__((packed))`)
            // has natural alignment 1 — the members already sit at offset 0, so packed
            // only lowers the union's OWN alignment (and thus how it aligns when
            // embedded). Read once; fed into effectiveAlign's baseline below.
            bool const packed = interner.isPacked(id);
            bool const anyBitfield = !interner.scalars(id).empty();
            // D-CSUBSET-PACKED F5 belt (D-CSUBSET-PACKED-BITFIELD-INTERACTION): the
            // packed baseline is applied ONLY on the non-bit-field path below. A
            // packed union carrying a bit-field member would silently get a NON-packed
            // alignment, so fail loud here (the semantic S_PackedBitfieldUnsupported is
            // the front door; this nullopt is the backstop).
            if (packed && anyBitfield) return std::nullopt;
            if (anyBitfield) {
                if (!bitFieldStrategyRealized(params.bitFieldStrategy))
                    return std::nullopt;
                out.bitFields.assign(fields.size(), BitFieldPlacement{});
            }
            // D-CSUBSET-MEMBER-ALIGNAS: a union member may carry an `alignas`
            // override too (`union { alignas(16) char c; int i; }`). Fold it into
            // the member's natural alignment with MAX semantics — identical to the
            // struct arm; an align-free union leaves `hasAligns` false and this
            // collapses to the unchanged `fl->align` path. (A union places every
            // member at offset 0, so a member alignas only ever RAISES the union's
            // overall alignment — and thus its size, rounded up — never a field's
            // offset.)
            bool const hasAligns = interner.hasExplicitAligns(id);
            auto effectiveAlign =
                [&](std::size_t i, Alignment natural) -> std::optional<Alignment> {
                // D-CSUBSET-PACKED: a packed union removes the per-member baseline
                // alignment (→ 1); a member `alignas` still RAISES it via the MAX-fold.
                Alignment const baseline = packed ? Alignment::of<1>() : natural;
                if (!hasAligns) return baseline;
                std::uint32_t const ovr = interner.explicitFieldAlign(id, i);
                if (ovr == 0) return baseline;   // no override on this member
                auto const a = Alignment::fromBytes(ovr);
                if (!a) return std::nullopt;    // stored non-pow2/>256 = upstream bug
                return maxAlign(baseline, *a);
            };
            std::uint64_t maxSize = 0;
            for (std::size_t i = 0; i < fields.size(); ++i) {
                auto const fl = computeLayout(fields[i], interner, params, dm);
                if (!fl) return std::nullopt;
                auto const ea = effectiveAlign(i, fl->align);
                if (!ea) return std::nullopt;
                out.align = maxAlign(out.align, *ea);
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
