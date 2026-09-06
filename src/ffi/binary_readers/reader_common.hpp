#pragma once

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_reader.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Per-format readers (`elf_reader.cpp`, `pe_reader.cpp`,
// `macho_reader.cpp`) all consume these byte-decode primitives +
// reporter wiring; centralising prevents three-way duplication and
// keeps each per-format TU at one architectural concern. Internal
// header (none of these symbols are `DSS_EXPORT`d — they live one
// scope above the per-format anonymous namespaces).
//
// Source/target/linker agnostic: the helpers operate on
// `std::span<uint8_t const>` byte buffers and emit through the
// codebase's existing `DiagnosticReporter`. No platform-specific
// headers, no target-arch references, no linker concepts.

namespace dss::ffi {

// ── Reporter wiring ─────────────────────────────────────────────

// Map `BinaryReadErrorKind` → structured `DiagnosticCode::F_*`. The
// kind enum is the function-return shape (compact); the F_* code is
// the in-reporter shape that `--suppress` / `--warnings-as-errors`
// consume. Closed-table dispatch — adding a new variant requires
// updating the switch AND the static_assert in this header.
[[nodiscard]] constexpr DiagnosticCode
toDiagnosticCode(BinaryReadErrorKind k) noexcept {
    switch (k) {
        case BinaryReadErrorKind::FileOpenFailed:      return DiagnosticCode::F_FileOpenFailed;
        case BinaryReadErrorKind::FileEmpty:           return DiagnosticCode::F_FileEmpty;
        case BinaryReadErrorKind::UnknownFormat:       return DiagnosticCode::F_UnknownBinaryFormat;
        case BinaryReadErrorKind::UnsupportedFormat:   return DiagnosticCode::F_UnsupportedBinaryFormat;
        case BinaryReadErrorKind::CorruptedBinary:     return DiagnosticCode::F_CorruptedBinary;
        case BinaryReadErrorKind::UnsupportedElfClass: return DiagnosticCode::F_UnsupportedElfClass;
        case BinaryReadErrorKind::SectionNotFound:     return DiagnosticCode::F_SectionNotFound;
    }
    // Unreachable per the closed enum; if a new variant lands without
    // updating this switch, emit `F_CorruptedBinary` as a fail-loud
    // (rather than `None` which would silently produce an uncoded
    // diagnostic that `--suppress` cannot target).
    return DiagnosticCode::F_CorruptedBinary;
}

static_assert(static_cast<std::uint8_t>(BinaryReadErrorKind::SectionNotFound) == 6u,
              "BinaryReadErrorKind grew without updating "
              "toDiagnosticCode — add a switch arm for the new variant.");

// Emit a binary-reader failure through the run-wide DiagnosticReporter
// AND return the structured BinaryReadError. Centralises the kind →
// F_* code mapping so every failure path produces a remediation-
// distinct diagnostic that downstream policy consumes.
[[nodiscard]] inline BinaryReadError
emitAndReturn(BinaryReadErrorKind kind, std::string detail,
              DiagnosticReporter& reporter) {
    dss::report(reporter, toDiagnosticCode(kind),
                DiagnosticSeverity::Error, detail);
    return BinaryReadError{kind, std::move(detail)};
}

// ── Little-endian byte readers ──────────────────────────────────
//
// All three on-disk binary formats (ELF / PE / Mach-O 64-bit) store
// scalars in little-endian. ELF technically supports big-endian
// (EI_DATA=2/MSB), but v1 enforces ELFDATA2LSB; Mach-O's mach_header
// has cputype + magic that disambiguate endianness via the magic
// value itself (`0xFEEDFACF` LE vs `0xCFFAEDFE` BE).
//
// TWO structures escape that rule and MUST go through `readU32BE`
// below: the System V / GNU `ar` armap (the "/" member), and the Mach-O
// `fat_header` — see the kMachOFatMagic block near `guessFormat`.
// Reaching for `readU32` on either reads a byte-swapped value that
// silently never matches.
// ⚠ The BSD `ar` armap (the "__.SYMDEF" member) is the MIRROR trap: it
// is LITTLE-endian and belongs on `readU32` above. The two archive
// symbol tables therefore disagree about byte order inside one container
// format, so "the ar armap" is not by itself enough to pick a reader —
// the SPECIAL MEMBER'S NAME is what decides (see ar_reader.cpp).

[[nodiscard]] inline std::uint16_t
readU16(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  static_cast<std::uint16_t>(b[off + 0])
         | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}
[[nodiscard]] inline std::uint32_t
readU32(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  static_cast<std::uint32_t>(b[off + 0])
         | (static_cast<std::uint32_t>(b[off + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[off + 2]) << 16)
         | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}
[[nodiscard]] inline std::uint64_t
readU64(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(b[off + i]) << (i * 8);
    }
    return v;
}

// Big-endian u32. Two structures in these formats are stored
// big-endian and both decode through here:
//   * the `ar` armap (System V / GNU archive symbol index) -- its count
//     + member-header offsets; everything else in `ar` is ASCII-decimal.
//   * the Mach-O `fat_header` + `fat_arch` / `fat_arch_64` entries --
//     big-endian ON DISK by definition, independent of the slices they
//     wrap (see the kMachOFatMagic block near `guessFormat`).
// Kept beside the LE readers so every reader shares one byte-decode
// home with its siblings.
[[nodiscard]] inline std::uint32_t
readU32BE(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return  (static_cast<std::uint32_t>(b[off + 0]) << 24)
         | (static_cast<std::uint32_t>(b[off + 1]) << 16)
         | (static_cast<std::uint32_t>(b[off + 2]) <<  8)
         |  static_cast<std::uint32_t>(b[off + 3]);
}

// Big-endian u16. ONE structure needs it: the ELF header of a file that
// declares `EI_DATA == ELFDATA2MSB`, whose half-words (`e_machine` among them)
// are stored in the FILE's order rather than the host's -- see
// `architectureFieldOf`'s ELF arm and `elf_backend`'s
// `looksLikeRelocatableObject`, which already reads `e_type` through the same
// lens for the same reason.
[[nodiscard]] inline std::uint16_t
readU16BE(std::span<std::uint8_t const> b, std::size_t off) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(b[off + 0]) << 8)
        | static_cast<std::uint16_t>(b[off + 1]));
}

// Read a NUL-terminated C string from a string table at `index`,
// bounded by the table's size. Returns empty string on out-of-range
// (caller-side check; we never read past `tableEnd`).
[[nodiscard]] inline std::string
readNulTerminated(std::span<std::uint8_t const> bytes,
                  std::size_t                   tableStart,
                  std::size_t                   tableEnd,
                  std::uint32_t                 index) {
    std::size_t const start = tableStart + index;
    if (start >= tableEnd) return {};
    std::size_t end = start;
    while (end < tableEnd && bytes[end] != 0u) ++end;
    return std::string{
        reinterpret_cast<char const*>(&bytes[start]),
        static_cast<std::size_t>(end - start)};
}

// ── Format detection ────────────────────────────────────────────

enum class FormatGuess : std::uint8_t {
    Unknown    = 0,
    Elf        = 1,
    Pe         = 2,
    MachO64    = 3,  // 0xFEEDFACF (stored LE) — 64-bit Mach-O (mach_header_64)
    MachOFat   = 4,  // 0xCAFEBABE / 0xCAFEBABF (stored BE) — universal/FAT — UnsupportedFormat v1
    MachO32    = 5,  // 0xFEEDFACE (stored LE) — 32-bit Mach-O — UnsupportedFormat v1
    Ar         = 6,  // "!<arch>\n" -- Unix `ar` static archive (.a / COFF .lib)
};

// The 8-byte global magic that opens every `ar` archive, GNU / BSD /
// COFF alike: the ASCII "!<arch>\n" (0x0A newline terminator).
constexpr std::uint8_t kArMagic[8] = {
    '!', '<', 'a', 'r', 'c', 'h', '>', 0x0Au};

// ── Mach-O universal ("fat") archive magics — BIG-ENDIAN ON DISK ────
//
// READ THIS BEFORE TOUCHING THE MACH-O ARMS OF `guessFormat`. The two
// Mach-O header families do NOT agree on byte order, and the asymmetry
// is not a quirk of any one file — it is in the format definition:
//
//   * A THIN header (`mach_header` / `mach_header_64`) stores its magic
//     in the SLICE's own byte order. Every target DSS emits is
//     little-endian, so `0xFEEDFACF` lands on disk as `CF FA ED FE` and
//     the little-endian `readU32` is the right lens for it.
//   * A FAT header (`struct fat_header`, Apple `<mach-o/fat.h>`) is
//     defined BIG-ENDIAN ON DISK — ALWAYS, whatever the slices inside
//     it are. Both dyld and `lipo` read it through
//     `OSSwapBigToHostInt32`. So `0xCAFEBABE` lands on disk as
//     `CA FE BA BE` and MUST be matched with `readU32BE`.
//
// Matching a fat magic through `readU32` is therefore not a style
// choice, it is a live bug: the little-endian read of `CA FE BA BE`
// yields `0xBEBAFECA`, so a `readU32(b,0) == 0xCAFEBABE` test never
// fires on any real universal binary. It fires only on the byte-SWAPPED
// spelling `BE BA FE CA` (`FAT_CIGAM` — the value a little-endian HOST
// computes after an in-host-order load, never a byte sequence a
// producer writes). That inversion made `FormatGuess::MachOFat`
// unreachable, which in turn made the D-FF1-MACHO-FAT "run `lipo -thin`
// first" remediation in `binary_reader.cpp` dead code — operators
// feeding a universal `.dylib` got `F_UnknownBinaryFormat` ("no
// recognised magic") from a message that listed `0xCAFEBABE` among the
// magics it recognised.
//
//   FAT_MAGIC    0xCAFEBABE -> CA FE BA BE (20-byte `fat_arch` entries)
//   FAT_MAGIC_64 0xCAFEBABF -> CA FE BA BF (32-byte `fat_arch_64`
//                              entries; what `lipo` emits once a slice
//                              offset/size exceeds 4 GiB)
//
// Classifying these does NOT mean DSS reads universal binaries — it
// still routes to `UnsupportedFormat`. It makes the ALREADY-INTENDED,
// already-anchored failure path REACHABLE so the operator is told to
// slice instead of being told the file is gibberish.
constexpr std::uint32_t kMachOFatMagic   = 0xCAFEBABEu;
constexpr std::uint32_t kMachOFatMagic64 = 0xCAFEBABFu;

[[nodiscard]] inline FormatGuess
guessFormat(std::span<std::uint8_t const> b) noexcept {
    if (b.size() >= 8
     && b[0] == kArMagic[0] && b[1] == kArMagic[1] && b[2] == kArMagic[2]
     && b[3] == kArMagic[3] && b[4] == kArMagic[4] && b[5] == kArMagic[5]
     && b[6] == kArMagic[6] && b[7] == kArMagic[7]) {
        return FormatGuess::Ar;
    }
    if (b.size() >= 4
     && b[0] == 0x7Fu && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') {
        return FormatGuess::Elf;
    }
    if (b.size() >= 2 && b[0] == 'M' && b[1] == 'Z') {
        return FormatGuess::Pe;
    }
    if (b.size() >= 4) {
        // THIN Mach-O: magic stored in the slice's own (little-endian)
        // order.
        std::uint32_t const thinMagic = readU32(b, 0);
        if (thinMagic == 0xFEEDFACFu) return FormatGuess::MachO64;
        if (thinMagic == 0xFEEDFACEu) return FormatGuess::MachO32;
        // FAT/universal: `fat_header` is big-endian ON DISK by
        // definition — see the kMachOFatMagic block above for why this
        // read must NOT reuse `thinMagic`. Deliberately not also
        // matching the byte-swapped spelling: `BE BA FE CA`
        // (`FAT_CIGAM`) is a host-order artifact, not a file, and
        // accepting it would hand the operator a `lipo -thin`
        // instruction that cannot work.
        std::uint32_t const fatMagic = readU32BE(b, 0);
        if (fatMagic == kMachOFatMagic || fatMagic == kMachOFatMagic64)
            return FormatGuess::MachOFat;
    }
    return FormatGuess::Unknown;
}

// ── WHERE EACH FORMAT KEEPS THE ARCHITECTURE IT WAS BUILT FOR ───────────────
//
// D-FFI-RESOLVE-LIBRARY-DOES-NOT-CHECK-THE-LIBRARY-ARCH. `guessFormat` above
// answers WHAT KIND of object a file is; this answers WHICH CPU that object
// declares itself to be for. Both are properties of the FILE, decoded from its
// own header, so both belong to FF1 and neither knows anything about a target —
// the COMPARISON against the build's architecture happens one tier up, in
// `ffi::checkLibraryMatchesTargetFormat`, which is the tier that has a target.
//
// ★ THIS IS A LOCATION TABLE, NOT A POLICY BRANCH — the same species as
// `guessFormat`'s magic table and as `toDiagnosticCode` above. Every arm
// returns a `{offset, width, fieldName}` triple and nothing else; no arm does
// anything a different arm does not, no caller behaves differently per arm, and
// the value returned is DECODED, never dispatched on. Adding a fourth object
// format adds one row.
//
// The three fields, from each format's own specification:
//   * ELF   `Elf64_Ehdr.e_machine`   — u16 at 18 (gABI; EM_X86_64=62,
//                                       EM_AARCH64=183), IN THE FILE'S OWN
//                                       BYTE ORDER — see the EI_DATA block
//                                       below, which is why this struct
//                                       carries `bigEndian` at all.
//   * PE    COFF `FileHeader.Machine` — u16 at `e_lfanew` + 4, where `e_lfanew`
//                                       is itself a u32 at 0x3C of the DOS stub
//                                       (PE/COFF §3.3; IMAGE_FILE_MACHINE_AMD64
//                                       =0x8664, ARM64=0xAA64). This is the one
//                                       format whose field is not at a fixed
//                                       offset, which is why this function
//                                       returns a LOCATION rather than a value:
//                                       the lead bytes can locate the field on
//                                       every format, but only the file can
//                                       supply it.
//   * Mach-O `mach_header_64.cputype` — u32 at 4 (<mach-o/loader.h>;
//                                       CPU_TYPE_X86_64=0x01000007,
//                                       CPU_TYPE_ARM64=0x0100000C).
//
// ── THE ONE FORMAT THAT DECLARES ITS OWN BYTE ORDER ─────────────────────────
//
// PE is little-endian by definition and a Mach-O slice announces its order in
// the MAGIC itself (`0xFEEDFACF` little-endian on disk; a big-endian slice
// spells `FE ED FA CF`, which `guessFormat` does not recognise and therefore
// never reaches this table). ELF is the exception: `EI_DATA` at offset 5 says
// how EVERY multi-byte field in the header is spelled, so `e_machine` must be
// decoded through it and cannot be read little-endian on faith.
//
// ⚠ THIS IS NOT A HYPOTHETICAL. ✔MEASURED at the cycle base, before this arm
// carried the byte order: a synthetic ELFDATA2MSB `.so` whose `e_machine`
// bytes are `00 16` (EM_S390, 22) made the refusal print
// "declares e_machine 0x1600 (5632)" — a number that appears NOWHERE in the
// file. A guard that fabricates the value it refuses on is worse than one that
// does not fire, because the operator is sent to look at a field that does not
// say what the message claims.
//
// ⓘ AND WHY IT IS DECODED RATHER THAN DELEGATED. Returning `nullopt` for a
// big-endian ELF would also stop the fabrication, and it is what the `MachO32`
// arm below does — but the two cases are NOT the same. A 32-bit Mach-O's
// `cputype` is genuinely in a different vocabulary (it lacks CPU_ARCH_ABI64),
// so answering would compare unlike things; a big-endian ELF's `e_machine` is
// the SAME gABI number, merely spelled the other way round, so it is knowable
// and the comparison is exact. Delegating instead would leave an escape that
// EVERY big-endian library triggers — silently disarming the guard for a whole
// class of input on the day a big-endian ELF format document ships, with
// nothing to notice. The decoded answer stays right on that day and on this
// one. (An `EI_DATA` that is neither of the two defined values is a header
// this table cannot decode at all, and that IS a `nullopt`.)
//
// `nullopt` means "these bytes name no single architecture", which is a real
// answer and not a failure. FOUR sources of it, each deliberate:
//   * `Ar` — a CONTAINER declares no architecture of its own; its MEMBERS do.
//     ⚠ SO THE CONTAINER IS NOT SIMPLY WAVED THROUGH: `ffi::checkLibrary-
//     MatchesTargetFormat` opens the archive and runs this same table over
//     EVERY member, because a `--resolve-library` archive is partitioned away
//     from the dynamic path before the per-CU build and would otherwise reach
//     no boundary check at all (✔MEASURED: an x86_64 `.a` fed to an aarch64
//     build produced rc=0, zero diagnostics and an aarch64 artefact). What is
//     delegated is only the CONTAINER-LEVEL guess this function would have to
//     invent from the global magic. Identical reasoning to
//     `objectFormatKindOfGuess`'s `ar` arm (ffi/ingest.cpp).
//   * `MachOFat` — a universal binary holds one cputype PER SLICE and no single
//     one for the file. FF1 refuses it with the `lipo -thin` remediation
//     (D-FF1-MACHO-FAT); answering here would replace that message with a
//     worse one.
//   * `MachO32` — its `cputype` sits at the same offset but LACKS the
//     CPU_ARCH_ABI64 bit, so comparing it against a 64-bit target's cputype
//     reports "wrong architecture" for a file whose actual defect is that it is
//     32-bit — and D-FF1-MACHO-32 already has the precise message for that.
//     Answering `nullopt` is what lets the accurate refusal reach the operator.
//   * `Unknown` — nothing to locate.
//
// ⚠ A SHORT LEAD IS `nullopt`, NEVER A GUESS: a file too short to hold the
// field cannot be classified here, and `readImports` reports the truncation
// with its own wording moments later.
struct ArchitectureField {
    std::uint64_t    offset    = 0;   // byte offset IN THE FILE
    std::uint8_t     width     = 0;   // 2 (u16) or 4 (u32)
    bool             bigEndian = false;  // the FILE's order, not the host's
    std::string_view fieldName;       // the format's OWN name for the field
};

// ELF `e_ident` — the two bytes this table reads, and the two values `EI_DATA`
// is allowed to take. Spelled here rather than borrowed from `elf_backend`:
// FF1 reads a file it was handed, the linker backend reads a file it is about
// to consume, and the gABI constants are the shared vocabulary both quote.
inline constexpr std::size_t  kElfEiDataOffset = 5u;
inline constexpr std::uint8_t kElfDataLsb      = 1u;   // ELFDATA2LSB
inline constexpr std::uint8_t kElfDataMsb      = 2u;   // ELFDATA2MSB

// The lead-byte window `architectureFieldOf` needs to LOCATE every format's
// field: 20 for the ELF `e_machine` itself, 8 for the Mach-O `cputype`, and
// 0x40 for the PE `e_lfanew` that points at the COFF header. 64 is the smallest
// window that covers all three, and it is the same prefix width
// `isRelocatableObjectFile` already reads for the same class of question.
inline constexpr std::size_t kArchitectureLeadBytes = 64;

[[nodiscard]] inline std::optional<ArchitectureField>
architectureFieldOf(FormatGuess g, std::span<std::uint8_t const> lead) noexcept {
    switch (g) {
        case FormatGuess::Elf: {
            if (lead.size() < 20u) return std::nullopt;
            std::uint8_t const eiData = lead[kElfEiDataOffset];
            // Only two encodings are DEFINED; anything else is not a header
            // this table can decode, and guessing one of the two would be the
            // fabrication the block above exists to prevent.
            if (eiData != kElfDataLsb && eiData != kElfDataMsb) {
                return std::nullopt;
            }
            return ArchitectureField{18u, 2u, eiData == kElfDataMsb,
                                     "e_machine"};
        }
        case FormatGuess::MachO64:
            if (lead.size() < 8u) return std::nullopt;
            return ArchitectureField{4u, 4u, false, "cputype"};
        case FormatGuess::Pe: {
            // `e_lfanew` is a u32 FILE OFFSET, so the COFF header can legally
            // sit anywhere; the caller seeks. The sum is computed in 64 BITS
            // deliberately — `e_lfanew` is a u32, so a 32-bit sum at the top
            // of its range wraps to 3 and would locate a small, plausible,
            // WRONG offset. (No overflow test guards the widened sum: the
            // largest `readU32` can answer is 0xFFFFFFFF, so `+ 4` cannot
            // approach the u64 range. A test for it would be a dead branch
            // with a live comment — the `peekLibraryIdentity` seek is where
            // an out-of-range offset is actually rejected, against
            // `streamoff`.)
            if (lead.size() < 0x40u) return std::nullopt;
            std::uint64_t const lfanew = readU32(lead, 0x3Cu);
            constexpr std::uint64_t kCoffMachineOffset = 4u;
            return ArchitectureField{lfanew + kCoffMachineOffset, 2u, false,
                                     "Machine"};
        }
        case FormatGuess::Ar:
        case FormatGuess::MachOFat:
        case FormatGuess::MachO32:
        case FormatGuess::Unknown:
            return std::nullopt;
    }
    return std::nullopt;
}

// The DECODE that goes with the location above, kept beside it so the width
// and the byte order have ONE reader rather than one per call site. `value`
// must be exactly `field.width` bytes — the caller (a stream read, or a
// subspan of a member already in memory) is the tier that knows how to obtain
// them and has already bounds-checked the source.
[[nodiscard]] inline std::uint32_t
decodeArchitectureCode(std::span<std::uint8_t const> value,
                       ArchitectureField const&      field) noexcept {
    if (field.width == 2u) {
        return field.bigEndian ? std::uint32_t{readU16BE(value, 0)}
                               : std::uint32_t{readU16(value, 0)};
    }
    return field.bigEndian ? readU32BE(value, 0) : readU32(value, 0);
}

} // namespace dss::ffi
