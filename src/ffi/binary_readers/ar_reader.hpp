#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "ffi/binary_reader.hpp"
#include "ffi/import_surface.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

// Plan 11 FF1 -- the `ar` static-archive reader (roadmap B3;
// anchor D-FF1-AR-READER). An `ar` archive (`libfoo.a`, or the
// Windows COFF `.lib` variant) is a CONTAINER: it packages N object
// members (`.o`) plus an archive symbol index (the "armap") that maps
// each exported symbol name to the byte offset of the member that
// defines it. A linker consults the armap to decide -- lazily -- which
// members to pull to satisfy an unresolved extern (plan 11 sec 4 Q5).
//
// Unlike the ELF / PE / Mach-O readers (which read ONE object and yield
// `std::vector<ImportSurface>`), an archive has TWO deliverables:
//   1. the member list (each member's resolved name + header/data offset
//      + size), and
//   2. the armap export surface (symbol name -> defining member).
// So this reader exposes a dedicated `ArArchive` struct via
// `readArArchive`, AND participates in the format-blind `readImports`
// dispatch via `readAr`, which PROJECTS the armap to `ImportSurface`
// rows (the linker-facing export surface -- see readAr's contract).
//
// Variant scope: the GNU / System V armap (a big-endian `u32` count +
// `u32` member-header offsets + a NUL-terminated name blob) is the
// PRIMARY, fully-supported form -- it is what `ar rcs` produces on Linux
// and the first linker member of a Windows COFF `.lib`. GNU long member
// names (a "//" string table + "/N" back-references) are resolved fully.
// The BSD variant (a "__.SYMDEF" armap + "#1/N" inline-length names) and
// the GNU 64-bit "/SYM64/" armap (>4 GiB archives) are DETECTED and
// fail loud cleanly (anchor D-FF1-AR-BSD-VARIANT) rather than silently
// misparsed. Every field is bounds-checked against the untrusted bytes;
// structural corruption fails loud `CorruptedBinary`, mirroring the
// c159 PE / c160 Mach-O readers' discipline.

namespace dss::ffi {

// One object member of an `ar` archive (a `.o` the archive packages).
// Excludes the special "/" armap + "//" long-name-table members --
// those are archive metadata, not linkable objects (they never appear
// in `ar t`).
struct DSS_EXPORT ArMember {
    std::string   name;          // resolved member name (long-name-expanded), e.g. "a.o"
    std::uint64_t headerOffset = 0;  // byte offset of the 60-byte ar_hdr in the archive
    std::uint64_t dataOffset   = 0;  // byte offset of the member payload (== headerOffset + 60)
    std::uint64_t size         = 0;  // payload size in bytes (the ar_hdr size field)
};

// One entry of the archive symbol index (armap): an exported symbol
// and the member that defines it. The armap carries NO symbol-kind
// (function vs data) information -- a consumer that needs the kind
// pulls the member's bytes (`bytes.subspan(dataOffset, size)`) and
// reads it with the matching per-format reader.
struct DSS_EXPORT ArSymbol {
    std::string   name;          // exported symbol name (verbatim, from the armap name blob)
    std::uint64_t memberOffset = 0;  // armap-recorded byte offset of the defining member's ar_hdr
    std::size_t   memberIndex  = 0;  // resolved index into ArArchive::members
};

// The parsed `ar` archive.
struct DSS_EXPORT ArArchive {
    std::vector<ArMember> members;   // real object members (excludes "/" + "//")
    std::vector<ArSymbol> symbols;   // armap entries; EMPTY when the archive has no symbol index
    std::string           archivePath;
};

// Parse a GNU / System V `ar` archive into its member list + armap.
// Fails loud `CorruptedBinary` on any malformed shape (bad magic,
// member header past EOF, non-numeric size field, a "/N" long-name
// offset past the "//" table, an armap count/offset out of range, an
// armap offset that matches no member header). An archive with members
// but no armap parses successfully with an empty `symbols` vector (a
// valid, if not-linker-indexable, archive).
[[nodiscard]] DSS_EXPORT
std::expected<ArArchive, BinaryReadError>
readArArchive(std::span<std::uint8_t const> bytes,
              std::string_view              archivePathLabel,
              DiagnosticReporter&           reporter);

// Dispatch-shaped projection (the `readImports` arm for `ar` archives).
// Reads the archive and returns its armap symbols as `ImportSurface`
// rows -- the honest linker-facing export surface (the UNION of the
// members' exported symbols, which is exactly the set a linker resolves
// against). Each row:
//   * mangledName  = the armap symbol name (verbatim)
//   * libraryPath  = "<archive>(<member>)" -- the standard nm / linker
//                    notation naming the DEFINING member (e.g.
//                    "libtest.a(a.o)"), so the lazy-pull consumer knows
//                    which member to extract
//   * kind         = NoType -- the armap records no function/data kind;
//                    resolving it requires reading the member object
//   * visibility   = Default, linkage = External -- an armap lists only
//                    DEFINED external symbols (that is what a linker
//                    indexes), so every row is an external definition
// An archive with no armap yields an empty surface (success).
[[nodiscard]] DSS_EXPORT
std::expected<std::vector<ImportSurface>, BinaryReadError>
readAr(std::span<std::uint8_t const> bytes,
       std::string_view              archivePathLabel,
       DiagnosticReporter&           reporter);

} // namespace dss::ffi
