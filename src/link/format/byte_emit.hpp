#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "lir/lir_pass_util.hpp"
#include "link/object_format_schema.hpp"

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

inline void appendI16LE(std::vector<std::uint8_t>& out, std::int16_t v) {
    appendU16LE(out, static_cast<std::uint16_t>(v));
}

inline void appendI64LE(std::vector<std::uint8_t>& out, std::int64_t v) {
    appendU64LE(out, static_cast<std::uint64_t>(v));
}

// Shared diagnostic-emit shorthand for format walkers. Identical
// shape to ML6/ML7's pass-side helpers; centralizes `report` so
// every walker speaks the same K_* dialect.
inline void emit(DiagnosticReporter& reporter, DiagnosticCode code,
                 std::string msg) {
    lir_pass_util::report(reporter, code, DiagnosticSeverity::Error,
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
