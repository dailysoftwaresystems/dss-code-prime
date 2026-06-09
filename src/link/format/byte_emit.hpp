#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"
#include "link/object_format_schema.hpp"

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared little-endian byte-emit helpers for object-format walkers
// (`elf.cpp`, `pe.cpp`, future `macho.cpp`). Hoisted from the per-
// walker copies — every walker writes little-endian POD records,
// and the byte-by-byte append loop is byte-identical across them.
//
// Header-only `inline` so each walker pays no link cost. Lives in
// `dss::link::format::detail` so the namespace identifies "format
// walker substrate" without escaping to the public surface.

namespace dss::link::format::detail {

inline void appendU8(std::vector<std::uint8_t>& out, std::uint8_t v) {
    out.push_back(v);
}

inline void appendU16LE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

inline void appendU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void appendU64LE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

// Big-endian (network-byte-order) append helpers.
//
// WHY these exist alongside the LE variants: every Mach-O POD record
// (mach_header_64, load commands, nlist_64, the chained-fixups payload)
// is LITTLE-endian — so the LE helpers above serve the whole walker.
// The Apple code-signature SuperBlob is the ONE exception: every field
// in the `CS_SuperBlob` / `CS_BlobIndex` / `CS_CodeDirectory` headers is
// BIG-endian (Apple's `cs_blobs.h` stores them via `htonl` / `OSSwapHostToBigInt*`).
// This is the universal codesign gotcha — emit a magic or length LE and
// the kernel's `cs_validate_csblob` rejects the signature. Hoisted here
// (not buried in `macho_codesign.cpp`) so the BE/LE pair lives in one
// place and a future format that needs BE records can reuse it.
inline void appendU32BE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void appendU64BE(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

inline void appendI16LE(std::vector<std::uint8_t>& out, std::int16_t v) {
    appendU16LE(out, static_cast<std::uint16_t>(v));
}

inline void appendI64LE(std::vector<std::uint8_t>& out, std::int64_t v) {
    appendU64LE(out, static_cast<std::uint64_t>(v));
}

// Positional (read/write-at-offset) helpers — patch sites need
// in-place mutation, not append. Hoisted from `exec_reloc_apply.hpp`
// at D-LK6-1 post-fold #1 (3-consumer trip: 3 ARM64 formula arms read
// + OR + write the 32-bit instruction word, vs Linear's append-style
// overwrite — but Linear can use these too when widthBytes==4).
//
// Bounds-check via `assert` — every caller pre-validates the offset
// against the buffer's expected size (`applyExecRelocations` rules
// 3 + 4), so the assertion is a defense-in-depth that catches caller
// bugs (mis-populated `funcTextStart`, off-by-one in a future patcher)
// while staying zero-cost in release. (silent-failure audit HIGH-2
// post-fold #2.)
inline std::uint32_t
readU32LEAt(std::vector<std::uint8_t> const& buf, std::size_t off) noexcept {
    assert(off + 4 <= buf.size() && "readU32LEAt: offset overruns buffer");
    return  static_cast<std::uint32_t>(buf[off + 0])
         | (static_cast<std::uint32_t>(buf[off + 1]) <<  8)
         | (static_cast<std::uint32_t>(buf[off + 2]) << 16)
         | (static_cast<std::uint32_t>(buf[off + 3]) << 24);
}

inline void
writeU32LEAt(std::vector<std::uint8_t>& buf, std::size_t off, std::uint32_t v) noexcept {
    assert(off + 4 <= buf.size() && "writeU32LEAt: offset overruns buffer");
    buf[off + 0] = static_cast<std::uint8_t>(v        & 0xFFu);
    buf[off + 1] = static_cast<std::uint8_t>((v >>  8) & 0xFFu);
    buf[off + 2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    buf[off + 3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

// Round `v` up to the nearest multiple of `a` (a must be > 0; not
// required to be a power of two — handled with the modulo-cycle
// form to keep the contract simple). Hoisted from per-walker
// lambdas in `elf.cpp` / `pe.cpp` / `macho.cpp` (3+ consumers
// trip the D-LK4-9 / D-LK4-11 hoist threshold; code-simplifier
// REQUIRED fold, LK7 post-fold review).
[[nodiscard]] inline constexpr std::uint64_t
alignUp(std::uint64_t v, std::uint64_t a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

// Shared diagnostic-emit shorthand for format walkers. Identical
// shape to ML6/ML7's pass-side helpers; centralizes `report` so
// every walker speaks the same K_* dialect.
inline void emit(DiagnosticReporter& reporter, DiagnosticCode code,
                 std::string msg) {
    dss::report(reporter, code, DiagnosticSeverity::Error,
                          std::move(msg));
}

// Resolve a SectionKind row in the format schema, fail-loud if
// missing. The walker treats absent declarations as a configuration
// error rather than substituting a default — silent defaults are
// exactly the silent-failure class the substrate discipline rejects.
// Returns nullptr + emits `K_NoMatchingObjectFormat` if not found.
//
// `walkerName` (e.g. "ELF writer" / "PE writer") prefixes the
// diagnostic so a multi-format build pinpoints the failing walker.
[[nodiscard]] inline ObjectFormatSectionInfo const*
requireSection(ObjectFormatSchema const& fmt, SectionKind kind,
               std::string_view walkerName,
               DiagnosticReporter& reporter) {
    auto const* s = fmt.sectionByKind(kind);
    if (s == nullptr) {
        emit(reporter, DiagnosticCode::K_NoMatchingObjectFormat,
             std::string{walkerName}
                 + " requires section kind '"
                 + std::string{sectionKindName(kind)}
                 + "' but object format '"
                 + std::string{fmt.name()}
                 + "' does not declare one");
    }
    return s;
}

} // namespace dss::link::format::detail
