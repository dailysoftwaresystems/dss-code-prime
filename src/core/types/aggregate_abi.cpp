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
    // FC17.9(e): F80 listed for SYMMETRY with F128 (both are floats), NOT
    // because this predicate rejects long-double leaves — the actual reject
    // is `hasLongDoubleClassLeaf` below, which nulls the whole classification
    // BEFORE the eightbyte-merge/HFA arms read this. Keeping the set complete
    // stops a future editor who relaxes that guard from silently letting an
    // F80 leaf classify SSE/INTEGER (an ABI-divergent by-value pass).
    return k == TypeKind::F16 || k == TypeKind::F32
        || k == TypeKind::F64 || k == TypeKind::F80 || k == TypeKind::F128;
}

// D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): a long-double (F80|F128) leaf
// forces the SysV x87 MEMORY class. SysV classes an x87 eightbyte pair
// X87/X87UP → the WHOLE aggregate goes MEMORY regardless of size
// (a naive float-kind join would classify it SSE → XMM pieces; a non-join
// classifies it INTEGER → a silent 2-GPR by-value pass, ABI-divergent at FFI).
// Used by the SysV arm to force MEMORY (or, for a result, X87). (AAPCS64 uses the narrower
// `hasF80Leaf` — its binary128 leaf is a legitimate Q-register HFA.)
[[nodiscard]] bool hasLongDoubleClassLeaf(std::vector<LeafField> const& leaves) noexcept {
    for (LeafField const& f : leaves) {
        if (f.kind == TypeKind::F80 || f.kind == TypeKind::F128) return true;
    }
    return false;
}

// D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): an F80 (x87 80-bit) leaf ONLY.
// On AAPCS64 a binary128 (F128) leaf is a valid HFA member (a Q-register piece,
// elem=16), so it falls through to the HFA math below; but an F80 leaf can only
// arise from a hand-built type on this axis (arm64 `long double` is F128 by
// construction, never x87), and x87 extended has NO AAPCS64 fundamental-FP
// register class — so an F80 leaf refuses classification (fail loud, never a
// guessed 8-byte FPR piece over a 16-byte x87 value).
[[nodiscard]] bool hasF80Leaf(std::vector<LeafField> const& leaves) noexcept {
    for (LeafField const& f : leaves) {
        if (f.kind == TypeKind::F80) return true;
    }
    return false;
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
    // C99 _Complex (D-CSUBSET-COMPLEX / D10): a complex is ABI-passed like a
    // struct{re, im} — emit TWO float leaves, re@base and im@base+elemSize, of the
    // element FLOAT kind. WITHOUT this arm the bare-Complex default below emits ONE
    // zero-size NON-float leaf → the eightbyte mis-classes INTEGER (a silent 2-GPR
    // by-value pass, ABI-divergent at FFI). double _Complex → 2 SSE eightbytes /
    // 2-double HFA; a long-double-complex leaf (F80/F128) then trips
    // hasLongDoubleClassLeaf and the aggregate refuses classification — automatic.
    if (k == TypeKind::Complex) {
        TypeId const elem = in.complexElement(ty);
        auto const elemLay = computeLayout(elem, in, lp, dm);
        if (!elemLay.has_value()) return false;
        TypeKind const ek = in.kind(elem);
        out.push_back(LeafField{base, ek, elemLay->size});
        out.push_back(LeafField{base + elemLay->size, ek, elemLay->size});
        return true;
    }
    // D-CSUBSET-BITINT: size the leaf through the TypeId-aware shim so a
    // `_BitInt(N)` gets its CONTAINER size (not the kind-only nullopt→1 default).
    out.push_back(LeafField{base, k,
                            sizeOfScalarOrBitInt(in, ty, dm).value_or(0)});
    return true;
}

// ★ P69 (lane `cs`, fold f26c, main's ledger 454): psABI 3.2.3's eightbyte classes and its merge, for an
// aggregate holding an x87 `long double` (the only one whose classes go past INTEGER and SSE here).
enum class EightbyteClass : std::uint8_t { NoClass, Integer, Sse, X87, X87Up, Memory };

// psABI 3.2.3, "the class of each eightbyte": (a) equal classes are that class; (b) NO_CLASS yields the other;
// (c) MEMORY wins; (d) then INTEGER; (e) then an X87, X87UP or COMPLEX_X87 makes MEMORY; (f) else SSE.
[[nodiscard]] EightbyteClass mergeEightbyte(EightbyteClass acc, EightbyteClass field) noexcept {
    using C = EightbyteClass;
    if (acc == field || field == C::NoClass) return acc;
    if (acc == C::NoClass) return field;
    if (acc == C::Memory || field == C::Memory) return C::Memory;
    if (acc == C::Integer || field == C::Integer) return C::Integer;
    if (acc == C::X87 || acc == C::X87Up || field == C::X87 || field == C::X87Up) return C::Memory;
    return C::Sse;
}

} // namespace

std::optional<AbiPassing>
classifyAggregate(AggregateConventionRules const& rules,
                  TypeId aggTy, TypeInterner const& in,
                  AggregateLayoutParams lp, DataModel dm,
                  AggregatePosition position) {
    AggregateClassKind const strategy    = rules.classification;
    std::uint16_t const      maxRegBytes = rules.maxRegBytes;
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
            wout.kind = AbiPassing::Kind::Memory;
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
        // D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): an F80 leaf still fails
        // loud (x87 extended has no AAPCS64 register class); an F128 (binary128)
        // leaf now FALLS THROUGH to the HFA math below — `isFloatKind`/
        // `scalarByteSize` already handle F128 (elem=16, count=size/16), so a
        // binary128 member is a 16-byte Q-register HFA piece (v0..v7) and a
        // 2-binary128 aggregate is a 2-piece HFA.
        if (hasF80Leaf(leaves)) return std::nullopt;
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
            aout.kind = AbiPassing::Kind::Memory;
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
    // empty — goes in MEMORY (an argument by the convention's carrier — its bytes on
    // the stack under SysV; a result through sret).
    if (size == 0 || size > maxRegBytes) {
        out.kind = AbiPassing::Kind::Memory;
        return out;
    }

    std::vector<LeafField> leaves;
    if (!collectLeaves(aggTy, 0, in, lp, dm, leaves))
        return std::nullopt;
    // D-CSUBSET-LONG-DOUBLE-AGGREGATE-ABI (LD-4): a long-double (F80|F128) leaf
    // forces the SysV x87 MEMORY class → the WHOLE aggregate goes to MEMORY
    // (X87/X87UP → MEMORY regardless of size, except a RESULT of class X87, below).
    // This is the same memory path a >16B aggregate takes (an argument's bytes on
    // the stack, a result through sret), NOT the SSE/INTEGER
    // eightbyte merge below (which would silently 2-GPR-pass or XMM-pass a
    // 16-byte x87 value). Win64BySize needs NO arm: its formats declare the f64
    // axis (long double ≡ F64, so no F80/F128 leaf can form) and its by-size
    // rule sends any 16-byte aggregate to memory anyway.
    // ★ P69 (lane `cs`, fold f26c — D-TARGET-SYSV-AMD64-MEMORY-CLASS-ARGUMENTS-GO-BY-POINTER-AND-X87-RESULTS-BY-SRET):
    // an aggregate whose ONLY content is one x87 value — every leaf an
    // F80 at offset 0 (`struct { long double x; }`, a one-element array of it, a union of two),
    // so its two eightbytes are X87 and X87UP — is RETURNED in the x87 register (psABI 3.2.3,
    // "If the class is X87, the value is returned on the X87 stack in %st0"), where a merge with
    // any other leaf is MEMORY; it is PASSED in memory either way. ✔MEASURED 2026-10-07 (lane
    // `lm`'s two-unit witness, linux-x86_64-debug run 20261008-015420-b7a36825): gcc 13.3.0 and
    // clang 18.1.3 leave `struct { long double x; }` on the x87 stack, and a DSS caller of theirs
    // read a wrong value through the hidden pointer this arm used to answer.
    // ★ P69 (lane `cs`, fold f26c, main's ledger 454): AND EVERY OTHER x87-BEARING AGGREGATE IS CLASSED BY psABI's
    // MERGE, eightbyte by eightbyte, where this arm answered MEMORY whenever such a leaf was present. The x87
    // value's eightbytes are X87 and X87UP; another leaf's are INTEGER or SSE; the merge (`mergeEightbyte`) takes
    // INTEGER over the x87 halves and MEMORY for an x87 half beside SSE; then the post-merger cleanup: any MEMORY
    // makes the whole MEMORY; X87 then X87UP is the x87 value alone (returned in st0, passed in memory); an X87UP
    // not preceded by X87 is the convention's (`AggregateConventionRules::x87UpWithoutX87`: MEMORY by psABI 0.98,
    // SSE on Apple's x86_64). ✔MEASURED 2026-11-03 (probe su, -O1 asm, each union as a result and as an argument
    // followed by a long): gcc 13.3.0 and clang 18.1.3 on Linux pass and return `union { long double; int / long /
    // void * }` in memory (the hidden pointer; the stack, the long in %rdi), Apple clang 21.0.0 for x86_64 in %rax
    // or %rdi and %xmm0 (the long in %rsi); the double and float unions and `double[2]` in memory on all three;
    // `long[2]` in two integer registers on all three (%rax:%rdx; %rdi:%rsi) -- where this arm answered MEMORY.
    // A binary128 (F128) leaf keeps the MEMORY answer it had (no x86_64 shape of it is measured here).
    if (hasLongDoubleClassLeaf(leaves)) {
        bool const binary128 = std::any_of(leaves.begin(), leaves.end(), [](LeafField const& f) {
            return f.kind == TypeKind::F128;
        });
        std::size_t const n = static_cast<std::size_t>((size + 7) / 8);   // 1 or 2
        std::vector<EightbyteClass> cls(n, EightbyteClass::NoClass);
        for (LeafField const& f : leaves) {
            std::size_t const e = static_cast<std::size_t>(f.offset / 8);
            if (f.kind == TypeKind::F80) {
                if (e < n) cls[e] = mergeEightbyte(cls[e], EightbyteClass::X87);
                if (e + 1 < n) cls[e + 1] = mergeEightbyte(cls[e + 1], EightbyteClass::X87Up);
                continue;
            }
            std::uint64_t fsz = f.byteSize;
            if (fsz == 0) fsz = 1;   // as the INTEGER/SSE loop below
            for (std::size_t k = e; k <= static_cast<std::size_t>((f.offset + fsz - 1) / 8) && k < n; ++k)
                cls[k] = mergeEightbyte(cls[k], isFloatKind(f.kind) ? EightbyteClass::Sse
                                                                    : EightbyteClass::Integer);
        }
        bool memory = binary128
            || std::find(cls.begin(), cls.end(), EightbyteClass::Memory) != cls.end();
        if (!memory && n == 2 && cls[0] == EightbyteClass::X87 && cls[1] == EightbyteClass::X87Up) {
            out.kind = position == AggregatePosition::Result ? AbiPassing::Kind::X87
                                                             : AbiPassing::Kind::Memory;
            return out;
        }
        for (std::size_t e = 0; e < n && !memory; ++e) {
            if (cls[e] == EightbyteClass::X87Up && (e == 0 || cls[e - 1] != EightbyteClass::X87)) {
                if (rules.x87UpWithoutX87 == X87UpWithoutX87::Memory) memory = true;
                else cls[e] = EightbyteClass::Sse;
            } else if (cls[e] == EightbyteClass::X87 || cls[e] == EightbyteClass::X87Up) {
                memory = true;   // an x87 half the pair above did not account for
            }
        }
        if (memory) {
            out.kind = AbiPassing::Kind::Memory;
            return out;
        }
        out.kind = AbiPassing::Kind::InRegisters;
        for (std::size_t e = 0; e < n; ++e) {
            std::uint64_t const eoff = e * 8;
            out.pieces.push_back(AbiPiece{
                cls[e] == EightbyteClass::Integer ? AbiPieceClass::Gpr : AbiPieceClass::Fpr, eoff,
                static_cast<std::uint32_t>(std::min<std::uint64_t>(8, size - eoff))});
        }
        return out;
    }

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
