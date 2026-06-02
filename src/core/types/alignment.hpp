#pragma once

#include "core/export.hpp"

#include <bit>
#include <cstdint>
#include <optional>

// Power-of-two byte-alignment newtype.
// Closes `D-LK4-RODATA-WIDE-ALIGNMENT-NEWTYPE`.
//
// **Why a newtype**: alignment values are a small bounded quantity
// (1..256 covers everything up to AVX-512 vectors) but the natural
// integer types (`uint16_t` / `uint32_t`) admit non-power-of-two
// values that silently produce wrong padding when the walker rounds
// bytes via `(x + a - 1) & ~(a - 1)`. Producers passing `7` instead
// of `8` get bytes laid out at wrong offsets with no diagnostic.
// The newtype wrapper makes the precondition (power-of-two, in the
// representable range) a construction-time check.
//
// **Range**: 1..256 bytes covers all current ABIs:
//   * Byte (1) — C-string concatenation, plain byte arrays.
//   * 2/4/8 — i16/i32/i64 alignment.
//   * 16 — SSE / x86_64 stackAlignment / Win64-MS-x64 frame.
//   * 32 — AVX vector spills.
//   * 64 — AVX-512 / cache-line.
//   * 128 / 256 — over-aligned types (`alignas(256) struct`).
// Higher alignments are rejected: PE `IMAGE_SCN_ALIGN_*` caps at
// 8192 but no producer in the current pipeline emits >256.
//
// **Storage**: stores log2 in a `uint8_t` (8 bits = 256 max power
// of two = 2^255 which is way beyond any meaningful alignment, but
// the static factory bounds the input to powers of two ≤ 256). The
// `value()` accessor returns the byte alignment.
//
// Default-construct is alignment 1 (byte-aligned, the natural
// no-op default — every byte is naturally byte-aligned).
namespace dss {

class DSS_EXPORT Alignment {
public:
    // Construct an alignment of `value` bytes. Returns nullopt if
    // `value` is 0 (alignment of 0 is undefined — `(x + 0 - 1) &
    // ~(0 - 1)` is meaningless), not a power of two, or > 256.
    [[nodiscard]] static constexpr std::optional<Alignment>
    fromBytes(std::uint32_t bytes) noexcept {
        if (bytes == 0u || bytes > 256u) return std::nullopt;
        if ((bytes & (bytes - 1u)) != 0u) return std::nullopt;
        return Alignment{static_cast<std::uint8_t>(
            std::countr_zero(bytes))};
    }

    // Compile-time factory — non-power-of-two / 0 / >256 inputs
    // are static-assert violations. Use for literals at known
    // alignments (e.g. `Alignment::of<16>()`).
    template <std::uint32_t Bytes>
    [[nodiscard]] static consteval Alignment of() noexcept {
        static_assert(Bytes != 0u,
                      "Alignment::of<0>() — alignment of 0 is undefined");
        static_assert(Bytes <= 256u,
                      "Alignment::of<N>() — N must be ≤ 256 bytes");
        static_assert((Bytes & (Bytes - 1u)) == 0u,
                      "Alignment::of<N>() — N must be a power of two");
        return Alignment{static_cast<std::uint8_t>(
            std::countr_zero(Bytes))};
    }

    // Runtime factory for inputs the caller GUARANTEES are powers
    // of two in [1, 256] — e.g. a primitive type's byte size
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
        // Precondition: bytes ∈ {1,2,4,8,16,32,64,128,256}.
        // We use a constexpr-friendly assert path; in release
        // builds this collapses to the bare cast (UB on misuse).
        // The matching `fromBytes()` exists for arbitrary input.
        return Alignment{static_cast<std::uint8_t>(
            std::countr_zero(bytes))};
    }

    // Default-construct: byte alignment (1).
    constexpr Alignment() noexcept : log2_(0u) {}

    // Bytes value (the alignment quantum). Power of two in [1, 256].
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
