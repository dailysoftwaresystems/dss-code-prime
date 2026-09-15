#pragma once

#include "core/export.hpp"

#include <bit>
#include <cstdint>
#include <optional>

// Power-of-two byte-alignment newtype.
// Closes `D-LK4-RODATA-WIDE-ALIGNMENT-NEWTYPE`.
//
// **Why a newtype**: an alignment is a POWER OF TWO, but the natural
// integer types (`uint16_t` / `uint32_t`) admit non-power-of-two
// values that silently produce wrong padding when the walker rounds
// bytes via `(x + a - 1) & ~(a - 1)`. Producers passing `7` instead
// of `8` get bytes laid out at wrong offsets with no diagnostic.
// The newtype wrapper makes the precondition (power-of-two, in the
// representable range) a construction-time check.
//
// **Range = the newtype's own STORAGE domain, and nothing else.**
// `kMaxBytes` below is the largest power of two a `uint32_t` byte
// count can carry; the log2 of it fits the `uint8_t` field with room
// to spare. That is a REPRESENTABILITY bound, not a policy one, and
// the distinction is the whole point of this paragraph.
//
// ⚠⚠ THIS RANGE USED TO BE 256, AND THAT NUMBER WAS A POLICY
// SMUGGLED INTO A STORAGE TYPE — with THREE hand-written copies of
// it elsewhere (`foldAlignmentOperand`'s `> 256u` ladder, the
// interner's `representableCompositeAlign`, and this file's own
// `of<N>` static_assert), each calling itself "the `Alignment`
// newtype cap". ✔MEASURED 2026-09-07 (P63,
// D-CSUBSET-ALIGNMENT-CEILING-REFUSES-WHAT-TWO-REFERENCES-RUN): gcc
// 13.3.0 and clang 18.1.3 BUILD AND RUN `aligned(N)` and
// `_Alignas(N)` at N = 512, 1024, 4096, 8192, 65536 and 2^28, both
// spellings, on ELF and on PE — while DSS refused every one of them
// by name. A page (4096) is the first alignment a program asking for
// one would write, so the wall sat right where real code lives.
// ★ The POLICY ceiling — how large an alignment a program may ASK
// for — is now DECLARED, per target, as
// `AggregateLayoutParams::maxRequestedAlignment` (a `.target.json`
// `aggregateLayout` key), and the semantic ladder refuses above IT.
// This type only says what it can carry. A fact with an owner does
// not get a second owner; this one had three and an absentee.
//
// **Storage**: stores log2 in a `uint8_t`. The `bytes()` accessor
// returns the byte alignment.
//
// Default-construct is alignment 1 (byte-aligned, the natural
// no-op default — every byte is naturally byte-aligned).
namespace dss {

class DSS_EXPORT Alignment {
public:
    // THE ONE OWNER of this type's representable domain. Every other
    // site that needs to know "can an `Alignment` carry this?" asks
    // `fromBytes` or names THIS constant — never a literal of its
    // own. It is the largest power of two a `uint32_t` byte count
    // holds, i.e. a property of the storage, deliberately NOT a
    // judgement about what a program should be allowed to request
    // (see the header docblock).
    static constexpr std::uint32_t kMaxBytes = 1u << 31;

    // Construct an alignment of `bytes` bytes. Returns nullopt if
    // `bytes` is 0 (alignment of 0 is undefined — `(x + 0 - 1) &
    // ~(0 - 1)` is meaningless), not a power of two, or above
    // `kMaxBytes` (unrepresentable in this type).
    [[nodiscard]] static constexpr std::optional<Alignment>
    fromBytes(std::uint32_t bytes) noexcept {
        if (bytes == 0u || bytes > kMaxBytes) return std::nullopt;
        if ((bytes & (bytes - 1u)) != 0u) return std::nullopt;
        return Alignment{static_cast<std::uint8_t>(
            std::countr_zero(bytes))};
    }

    // Compile-time factory — non-power-of-two / 0 / unrepresentable
    // inputs are static-assert violations. Use for literals at known
    // alignments (e.g. `Alignment::of<16>()`).
    template <std::uint32_t Bytes>
    [[nodiscard]] static consteval Alignment of() noexcept {
        static_assert(Bytes != 0u,
                      "Alignment::of<0>() — alignment of 0 is undefined");
        static_assert(Bytes <= kMaxBytes,
                      "Alignment::of<N>() — N exceeds Alignment::kMaxBytes");
        static_assert((Bytes & (Bytes - 1u)) == 0u,
                      "Alignment::of<N>() — N must be a power of two");
        return Alignment{static_cast<std::uint8_t>(
            std::countr_zero(Bytes))};
    }

    // Runtime factory for inputs the caller GUARANTEES are powers
    // of two in [1, kMaxBytes] — e.g. a primitive type's byte size
    // (always ∈ {1,2,4,8,16}). Asserts the precondition; returns
    // a constructed `Alignment` directly with no optional unwrap.
    //
    // Use this in preference to `fromBytes()` when the call site
    // proves the precondition holds and an `optional` unwrap
    // would surface as dead branch + wrong-domain diagnostic on
    // the impossible nullopt arm (type-design audit fold:
    // 2026-06-02). For inputs sourced from user data or
    // arbitrary computation, use `fromBytes()` and handle the
    // nullopt explicitly.
    [[nodiscard]] static constexpr Alignment
    ofRuntimePow2(std::uint32_t bytes) noexcept {
        // Precondition: `bytes` is a power of two in [1, kMaxBytes].
        // We use a constexpr-friendly assert path; in release
        // builds this collapses to the bare cast (UB on misuse).
        // The matching `fromBytes()` exists for arbitrary input.
        return Alignment{static_cast<std::uint8_t>(
            std::countr_zero(bytes))};
    }

    // Default-construct: byte alignment (1).
    constexpr Alignment() noexcept : log2_(0u) {}

    // Bytes value (the alignment quantum). A power of two in
    // [1, kMaxBytes].
    [[nodiscard]] constexpr std::uint32_t bytes() const noexcept {
        return 1u << log2_;
    }

    // log2 form (useful for Mach-O `__align` / PE
    // `IMAGE_SCN_ALIGN_*` shift encoding).
    [[nodiscard]] constexpr std::uint8_t log2() const noexcept {
        return log2_;
    }

    // Round `n` up to the next multiple of `bytes()`. Standard
    // alignment-padding kernel. `bytes()` is power-of-two so the
    // mask form `(n + a - 1) & ~(a - 1)` is exact.
    [[nodiscard]] constexpr std::uint64_t
    alignUp(std::uint64_t n) const noexcept {
        std::uint64_t const a = bytes();
        return (n + a - 1u) & ~(a - 1u);
    }

    constexpr bool operator==(Alignment const&) const noexcept = default;

private:
    explicit constexpr Alignment(std::uint8_t log2) noexcept
        : log2_(log2) {}

    std::uint8_t log2_;
};

} // namespace dss
