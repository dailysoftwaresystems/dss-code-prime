#pragma once

#include "core/export.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// ── Header-name matching (D-PP-HEADER-CASE-INSENSITIVE-PE): how an
//    `#include` header NAME is matched against the filesystem ──────────────
//
// ★ THE DEFECT THIS AXIS REMOVES. Before this axis existed, DSS handed the
// header name straight to `std::filesystem::exists` and let the HOST
// FILESYSTEM decide whether `<Windows.h>` matches `windows.json`. MEASURED
// (one Linux host, one binary, one commit, only the filesystem varied):
//   * config tree on ext4 (case-sensitive):  `<windows.h>` rc=0,
//     `<Windows.h>` rc=1 error[F001A];
//   * the SAME tree via /mnt/c DrvFs (case-insensitive): BOTH rc=0.
// Two failure directions came out of that, and the second is the worse:
//   1. WRONG REJECT — `sqlite3.c:67322`'s `#include <Windows.h>` (capital W,
//      live whenever `SQLITE_USE_SEH` is on, which DSS's pe64 `_MSC_VER`
//      predefine turns on) cannot be compiled for a WINDOWS target from a
//      case-sensitive host.
//   2. SILENT WRONG ACCEPT — `#include <Stdio.h>` for an ELF target on a
//      Windows host compiled rc=0 and emitted a real object. A conforming
//      POSIX toolchain must REJECT that. The pe successes were correct BY
//      ACCIDENT (NTFS folding), never by design.
//
// Case-matching of a header name is a property of the TARGET PLATFORM's
// convention, so it is declared on the OBJECT FORMAT schema (the per-OS
// artifact — the same x86_64 CPU target serves both pe64-windows and
// elf64-linux) as the REQUIRED root key `"headerNameMatching"`. It is
// REQUIRED and not optional-with-default on purpose: an optional key lets a
// future pe/macho format file silently regress to case-sensitive matching,
// which is precisely the class of silent regression this axis exists to end.
//
// The engine NEVER branches on the format KIND to obtain the policy — it
// reads this enum out of the loaded schema and consumes it generically
// (`core/types/include_path_resolve.hpp`). There is no `if (kind == Pe)`
// anywhere on the path, and a reviewer grepping for one will not find it.
//
// **Cross-tier vocabulary**: lives under `core/types/` (not `src/link/`)
// because the include-path resolver, the preprocessor and the import
// resolver all speak it without pulling the link substrate header — the
// `object_format_kind.hpp` / `data_model.hpp` extraction precedent.

namespace dss {

// Closed vocabulary. JSON spellings are the enum's own names, hyphenated.
enum class HeaderNameMatching : std::uint8_t {
    // Zero is the INVALID sentinel: a hand-built `ObjectFormatData` that never
    // set the field is rejected by `ObjectFormatSchema::validate()`, exactly as
    // a zero `DataModel` is. The loader path always sets it or fails.
    Invalid         = 0,
    // Byte-exact. `<Stdio.h>` does NOT resolve `stdio.json`. POSIX/ELF, and the
    // conservative default wherever no format is known (LSP, direct-API, tests,
    // non-C languages) — a conforming-POSIX answer is the safe one to guess.
    CaseSensitive   = 1,
    // ASCII-case-insensitive, per path component. `<Windows.h>`, `<WiNdOwS.h>`
    // and `<windows.h>` all resolve the one `windows.json`. Windows (PE) and
    // Darwin (Mach-O, whose APFS/HFS+ default is case-insensitive).
    CaseInsensitive = 2,
};

// Hand-rolled name pair rather than the house `EnumNameTable<E,N>`: that
// template lives in `target_schema.hpp`, which this header must not pull (the
// same include-cycle constraint `data_model.hpp` documents). Two spellable
// values; the switch stays trivially in sync with the enum (a new member
// without an arm fails the -Wswitch build).
[[nodiscard]] constexpr std::string_view
headerNameMatchingName(HeaderNameMatching m) noexcept {
    switch (m) {
        case HeaderNameMatching::Invalid:         return {};
        case HeaderNameMatching::CaseSensitive:   return "case-sensitive";
        case HeaderNameMatching::CaseInsensitive: return "case-insensitive";
    }
    return {};
}

// `Invalid` deliberately has NO JSON spelling — a spellable "invalid" would
// let a typo look deliberate.
[[nodiscard]] constexpr std::optional<HeaderNameMatching>
headerNameMatchingFromName(std::string_view s) noexcept {
    if (s == "case-sensitive")   return HeaderNameMatching::CaseSensitive;
    if (s == "case-insensitive") return HeaderNameMatching::CaseInsensitive;
    return std::nullopt;
}

// The policy every caller that has NO active object format must use: LSP,
// direct-API callers, unit tests, non-C languages. Case-SENSITIVE is the
// conservative choice — it is what a conforming POSIX toolchain does, and
// guessing it can only ever REJECT a spelling some target would have accepted
// (loud), never ACCEPT one a target would have rejected (silent).
inline constexpr HeaderNameMatching kDefaultHeaderNameMatching =
    HeaderNameMatching::CaseSensitive;

} // namespace dss
