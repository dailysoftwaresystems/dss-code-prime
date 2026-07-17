#include "ffi/binary_readers/ar_reader.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_readers/reader_common.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>

namespace dss::ffi {

namespace {

// -- `ar` static-archive reader (FF1, roadmap B3) ----------------
// Plan 11 FF1: parse a GNU / System V `ar` archive into its member
// list + archive symbol index (armap). Layout (audited against GNU
// binutils `ar` 2.42 output + the SysV ar(5) format):
//
//   [0..7]  global magic "!<arch>\n"
//   then, repeated (each member 2-byte aligned):
//     ar_hdr (60 bytes, all ASCII except the trailer):
//       [ 0..15] name  (16, space-padded; short name ends in '/')
//       [16..27] mtime (12, ASCII decimal)   -- unused
//       [28..33] uid   ( 6, ASCII decimal)   -- unused
//       [34..39] gid   ( 6, ASCII decimal)   -- unused
//       [40..47] mode  ( 8, ASCII octal)     -- unused
//       [48..57] size  (10, ASCII decimal)   -- the payload length
//       [58..59] magic "`\n" (0x60 0x0A)     -- header terminator
//     then `size` payload bytes, then a '\n' pad byte iff size is odd.
//
// Two special members carry archive metadata (never linkable objects,
// excluded from the reported member list -- they do not show in `ar t`):
//   name "/"  -> the armap (System V / GNU symbol index). Its payload:
//                  [0..3]      count           (u32 BIG-endian)
//                  [4..4+4c-1] member offsets  (u32 BIG-endian x count)
//                  [rest]      count NUL-terminated symbol names
//                symbol[i] is defined by the member whose ar_hdr begins
//                at offset[i]. (Big-endian is the ONE non-ASCII quirk.)
//   name "//" -> the GNU long-name string table. A member whose name
//                field is "/N" (N decimal) takes its real name from the
//                "//" table starting at byte N, terminated by "/\n".
//
// Fail-loud discipline mirrors the c159 PE / c160 Mach-O readers: a
// structural break (bad magic, header/data past EOF, non-numeric size,
// armap count/offset out of range, a "/N" offset past the "//" table,
// an armap offset matching no member) fails loud `CorruptedBinary`; a
// merely degenerate per-entry case (an empty armap symbol name) is
// skipped + counted, summarized via F_BinaryReaderPartialCorruption.

constexpr std::size_t kArMagicSize = 8;
constexpr std::size_t kArHdrSize   = 60;
constexpr std::size_t kArNameOff   = 0;
constexpr std::size_t kArNameLen   = 16;
constexpr std::size_t kArSizeOff   = 48;
constexpr std::size_t kArSizeLen   = 10;
constexpr std::size_t kArTrailerOff = 58;  // "`\n"

// Parse an ASCII-decimal, space-padded ar_hdr field (e.g. `size`).
// Accepts optional leading spaces, one run of decimal digits, then only
// trailing spaces / NULs. Returns nullopt on a non-numeric field or an
// overflow -- the caller fails loud. A field of pure spaces (no digit)
// is nullopt (a `size` must state a number).
[[nodiscard]] std::optional<std::uint64_t>
parseAsciiDecimalField(std::span<std::uint8_t const> bytes,
                       std::size_t start, std::size_t len) {
    std::uint64_t v = 0;
    bool sawDigit = false;
    std::size_t i = 0;
    while (i < len && bytes[start + i] == ' ') ++i;   // leading pad
    for (; i < len; ++i) {
        std::uint8_t const c = bytes[start + i];
        if (c >= '0' && c <= '9') {
            std::uint64_t const d = static_cast<std::uint64_t>(c - '0');
            if (v > (UINT64_MAX - d) / 10u) return std::nullopt;  // overflow
            v = v * 10u + d;
            sawDigit = true;
        } else if (c == ' ' || c == 0u) {
            break;                                     // trailing pad
        } else {
            return std::nullopt;                       // non-numeric
        }
    }
    for (; i < len; ++i) {                             // rest must be pad
        std::uint8_t const c = bytes[start + i];
        if (c != ' ' && c != 0u) return std::nullopt;
    }
    if (!sawDigit) return std::nullopt;
    return v;
}

// Strip trailing spaces + NULs from a 16-byte ar_hdr name field.
[[nodiscard]] std::string rstripNameField(std::span<std::uint8_t const> bytes,
                                           std::size_t off) {
    std::size_t end = kArNameLen;
    while (end > 0
        && (bytes[off + end - 1] == ' ' || bytes[off + end - 1] == 0u)) {
        --end;
    }
    return std::string{reinterpret_cast<char const*>(&bytes[off]), end};
}

[[nodiscard]] bool allDigits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char const c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

// Tokenize the GNU "//" long-name string table ONCE into an
// offset -> name map. Each entry is "<name>/\n" (the trailing '/' is the
// GNU terminator; '\n' ends the entry); the map key is the entry's START
// offset -- exactly what a member's "/N" name field references. Building
// this once (a single linear pass) replaces a per-member rescan: a
// crafted "//" table with no terminator + many "/N" members drove that
// rescan to O(members x table-bytes) -- a time-hang DoS on the untrusted
// format-blind path (memory-safe, but unbounded work). An unterminated
// final NON-empty entry (no closing '\n' before the table end) fails
// loud CorruptedBinary -- a real GNU/SysV table always '\n'-terminates
// every entry, so this never rejects a benign archive. Empty entries (a
// trailing padding '\n', or "\n\n") are skipped, not mapped.
[[nodiscard]] std::expected<std::unordered_map<std::uint64_t, std::string>,
                            BinaryReadError>
parseLongNameTable(std::span<std::uint8_t const> tbl,
                   DiagnosticReporter& reporter) {
    std::unordered_map<std::uint64_t, std::string> map;
    std::size_t start = 0;
    while (start < tbl.size()) {
        std::size_t nl = start;
        while (nl < tbl.size() && tbl[nl] != '\n') ++nl;
        if (nl >= tbl.size()) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: '//' long-name table has an unterminated final "
                "entry at offset " + std::to_string(start)
                + " (no '\\n' before the table end)", reporter));
        }
        // Entry = [start, nl); strip one trailing '/' (GNU terminator).
        std::size_t entryEnd = nl;
        if (entryEnd > start && tbl[entryEnd - 1] == '/') --entryEnd;
        if (entryEnd > start) {
            map.emplace(static_cast<std::uint64_t>(start),
                std::string{reinterpret_cast<char const*>(&tbl[start]),
                            static_cast<std::size_t>(entryEnd - start)});
        }
        start = nl + 1;
    }
    return map;
}

// Resolve a member's real name from its 16-byte ar_hdr name field,
// expanding a GNU "/N" reference via the PRE-TOKENIZED "//" long-name
// map (offset -> name, built once by parseLongNameTable). The caller has
// already peeled off the "/" armap + "//" table specials, so `trimmed`
// here is always a real member: either "/N" (long) or "name/" (GNU
// short, trailing '/' terminator) / "name" (bare SysV short).
[[nodiscard]] std::expected<std::string, BinaryReadError>
resolveMemberName(std::string trimmed,
                  std::optional<std::unordered_map<std::uint64_t,
                                                    std::string>> const& longNames,
                  DiagnosticReporter& reporter) {
    if (trimmed.size() >= 2 && trimmed[0] == '/'
        && allDigits(std::string_view{trimmed}.substr(1))) {
        // GNU long-name reference "/N" -> the "//" table entry at offset N.
        std::uint64_t n = 0;
        for (std::size_t k = 1; k < trimmed.size(); ++k) {
            std::uint64_t const d = static_cast<std::uint64_t>(trimmed[k] - '0');
            if (n > (UINT64_MAX - d) / 10u) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "ar reader: long-name reference '" + trimmed
                    + "' overflows", reporter));
            }
            n = n * 10u + d;
        }
        if (!longNames) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: member name '" + trimmed + "' references the "
                "'//' long-name table, but the archive has no '//' member",
                reporter));
        }
        // O(1) lookup into the tokenized table. A "/N" that does NOT land
        // on an entry START -- a mid-entry byte offset, or one past the
        // last entry -- fails loud (the tokenize-once correctness gain:
        // the old per-member rescan silently returned a shifted/truncated
        // substring for such an N, e.g. "/5" into the middle of a name).
        auto const it = longNames->find(n);
        if (it == longNames->end()) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: long-name offset " + std::to_string(n)
                + " does not begin a '//' table entry (mid-entry byte or "
                "unknown offset)", reporter));
        }
        return it->second;
    }
    // Short name: strip the single GNU trailing-'/' terminator if present.
    if (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    return trimmed;
}

// A member header located during the pass-1 walk (name resolved later,
// once the "//" table is known).
struct RawMember {
    std::string   nameTrimmed;
    std::uint64_t headerOffset;
    std::uint64_t dataOffset;
    std::uint64_t size;
};

} // namespace

std::expected<ArArchive, BinaryReadError>
readArArchive(std::span<std::uint8_t const> bytes,
              std::string_view              archivePathLabel,
              DiagnosticReporter&           reporter) {
    // -- Global magic --
    if (bytes.size() < kArMagicSize) {
        return std::unexpected(emitAndReturn(
            BinaryReadErrorKind::CorruptedBinary,
            "ar reader: file shorter than the 8-byte '!<arch>' magic",
            reporter));
    }
    for (std::size_t i = 0; i < kArMagicSize; ++i) {
        if (bytes[i] != kArMagic[i]) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: bad archive magic (expected '!<arch>\\n')",
                reporter));
        }
    }

    // -- Pass 1: walk member headers --
    std::vector<RawMember> raw;
    std::optional<std::span<std::uint8_t const>> longnames;
    std::optional<std::span<std::uint8_t const>> armap;

    std::uint64_t off = kArMagicSize;
    while (off < bytes.size()) {
        if (rangeExceedsBuffer(off, kArHdrSize, bytes.size())) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: member header at offset " + std::to_string(off)
                + " runs past EOF (need 60 bytes)", reporter));
        }
        if (bytes[off + kArTrailerOff] != 0x60u
            || bytes[off + kArTrailerOff + 1] != 0x0Au) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: member header at offset " + std::to_string(off)
                + " is missing the '`\\n' terminator magic", reporter));
        }
        auto const sizeOpt =
            parseAsciiDecimalField(bytes, off + kArSizeOff, kArSizeLen);
        if (!sizeOpt) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: member header at offset " + std::to_string(off)
                + " has a non-numeric size field", reporter));
        }
        std::uint64_t const memberSize = *sizeOpt;
        std::uint64_t const dataOffset = off + kArHdrSize;
        if (rangeExceedsBuffer(dataOffset, memberSize, bytes.size())) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: member data at offset " + std::to_string(dataOffset)
                + " (size " + std::to_string(memberSize)
                + ") runs past EOF", reporter));
        }

        std::string const trimmed = rstripNameField(bytes, off + kArNameOff);

        // BSD / SYM64 variants: detect + fail loud cleanly (never silently
        // misparse). Anchor D-FF1-AR-BSD-VARIANT.
        if (trimmed.rfind("__.SYMDEF", 0) == 0) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: BSD-variant archive symbol table ('__.SYMDEF') "
                "is not yet supported -- GNU/System V archives are "
                "(anchor D-FF1-AR-BSD-VARIANT)", reporter));
        }
        if (trimmed.rfind("#1/", 0) == 0) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: BSD-variant inline-length member name ('#1/N') "
                "is not yet supported -- GNU/System V archives are "
                "(anchor D-FF1-AR-BSD-VARIANT)", reporter));
        }
        if (trimmed == "/SYM64/") {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: 64-bit GNU archive symbol table ('/SYM64/', for "
                ">4 GiB archives) is not yet supported -- 32-bit '/' armaps "
                "are (anchor D-FF1-AR-BSD-VARIANT)", reporter));
        }

        if (trimmed == "/") {
            // The FIRST "/" is the System V / COFF-1st-linker-member armap
            // (big-endian), which this reader parses. A Windows COFF `.lib`
            // carries a SECOND "/" member -- Microsoft's little-endian sorted
            // index (D-FF1-AR-COFF-WRITER, c169) -- which link.exe prefers but
            // this reader does NOT parse (it would mis-read the LE payload as
            // BE). FIRST-"/"-WINS: keep the big-endian armap, treat the 2nd "/"
            // as an ignored index member (not a linkable object).
            if (!armap) {
                armap = bytes.subspan(static_cast<std::size_t>(dataOffset),
                                      static_cast<std::size_t>(memberSize));
            }
        } else if (trimmed == "//") {
            longnames = bytes.subspan(static_cast<std::size_t>(dataOffset),
                                      static_cast<std::size_t>(memberSize));
        } else {
            raw.push_back(RawMember{trimmed, off, dataOffset, memberSize});
        }

        // Advance to the next header, 2-byte aligned (a '\n' pad byte
        // follows an odd-sized payload). At true EOF the pad is absent;
        // an over-shoot by 1 simply ends the loop.
        off = dataOffset + memberSize + (memberSize & 1u);
    }

    // -- Tokenize the "//" long-name table ONCE (offset -> name) --
    // A single linear pass; each subsequent "/N" member name is then an
    // O(1) map lookup (not a per-member table rescan -- the DoS fix).
    std::optional<std::unordered_map<std::uint64_t, std::string>> longNameMap;
    if (longnames) {
        auto m = parseLongNameTable(*longnames, reporter);
        if (!m) return std::unexpected(m.error());
        longNameMap = std::move(*m);
    }

    // -- Pass 2: resolve member names + build the member list --
    ArArchive out;
    out.archivePath = std::string{archivePathLabel};
    out.members.reserve(raw.size());
    std::unordered_map<std::uint64_t, std::size_t> hdrOffToIndex;
    hdrOffToIndex.reserve(raw.size());
    for (RawMember const& r : raw) {
        auto nameE = resolveMemberName(r.nameTrimmed, longNameMap, reporter);
        if (!nameE) return std::unexpected(nameE.error());
        hdrOffToIndex.emplace(r.headerOffset, out.members.size());
        ArMember m;
        m.name         = std::move(*nameE);
        m.headerOffset = r.headerOffset;
        m.dataOffset   = r.dataOffset;
        m.size         = r.size;
        out.members.push_back(std::move(m));
    }

    // -- Pass 3: parse the armap (System V / GNU symbol index) --
    if (armap) {
        std::span<std::uint8_t const> const d = *armap;
        if (d.size() < 4u) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: archive symbol table (armap) is smaller than its "
                "4-byte count field", reporter));
        }
        std::uint32_t const count = readU32BE(d, 0);
        std::uint64_t const offsetsBytes = static_cast<std::uint64_t>(count) * 4u;
        if (rangeExceedsBuffer(4u, offsetsBytes, d.size())) {
            return std::unexpected(emitAndReturn(
                BinaryReadErrorKind::CorruptedBinary,
                "ar reader: armap offset array (" + std::to_string(count)
                + " x 4 bytes) runs past the armap end", reporter));
        }
        std::uint64_t pos = 4u + offsetsBytes;   // name-blob start
        out.symbols.reserve(count);
        std::uint32_t emptyNameSkips = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint64_t const memberOff =
                readU32BE(d, static_cast<std::size_t>(4u + static_cast<std::uint64_t>(i) * 4u));
            if (pos >= d.size()) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "ar reader: armap declares " + std::to_string(count)
                    + " symbols but the name blob ends after "
                    + std::to_string(i) + " (truncated armap)", reporter));
            }
            std::uint64_t s = pos;
            while (s < d.size() && d[static_cast<std::size_t>(s)] != 0u) ++s;
            if (s >= d.size()) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "ar reader: armap symbol name at blob offset "
                    + std::to_string(pos)
                    + " is not NUL-terminated (truncated armap)", reporter));
            }
            std::string name{
                reinterpret_cast<char const*>(&d[static_cast<std::size_t>(pos)]),
                static_cast<std::size_t>(s - pos)};
            pos = s + 1u;

            // An armap offset that matches no member header is a structural
            // inconsistency -- fail loud (the index would mis-route a
            // lazy pull). Checked BEFORE the empty-name skip so a corrupt
            // offset is never masked by a degenerate name.
            auto const it = hdrOffToIndex.find(memberOff);
            if (it == hdrOffToIndex.end()) {
                return std::unexpected(emitAndReturn(
                    BinaryReadErrorKind::CorruptedBinary,
                    "ar reader: armap symbol '" + name
                    + "' points at member-header offset "
                    + std::to_string(memberOff)
                    + ", which matches no archive member", reporter));
            }

            if (name.empty()) {
                // Degenerate (not structural): a lone NUL in the blob. Skip
                // + count, mirroring the c159 PE reader's empty-name skip.
                ++emptyNameSkips;
                continue;
            }

            ArSymbol sym;
            sym.name         = std::move(name);
            sym.memberOffset = memberOff;
            sym.memberIndex  = it->second;
            out.symbols.push_back(std::move(sym));
        }

        if (emptyNameSkips > 0) {
            dss::report(reporter,
                DiagnosticCode::F_BinaryReaderPartialCorruption,
                DiagnosticSeverity::Warning,
                "ar reader: '" + std::string{archivePathLabel}
                + "': skipped " + std::to_string(emptyNameSkips)
                + " armap entries with empty symbol names. Surfaced "
                + std::to_string(out.symbols.size()) + " valid symbols.");
        }
    }

    return out;
}

std::expected<std::vector<ImportSurface>, BinaryReadError>
readAr(std::span<std::uint8_t const> bytes,
       std::string_view              archivePathLabel,
       DiagnosticReporter&           reporter) {
    auto archiveE = readArArchive(bytes, archivePathLabel, reporter);
    if (!archiveE) return std::unexpected(archiveE.error());
    ArArchive const& archive = *archiveE;

    // Project the armap to the linker-facing export surface: one row per
    // indexed symbol, naming its defining member via the standard
    // "<archive>(<member>)" nm/linker notation. The armap is the UNION of
    // the members' exported symbols -- exactly the set a linker resolves
    // an unresolved extern against (plan 11 sec 4 Q5 lazy pull).
    std::vector<ImportSurface> out;
    out.reserve(archive.symbols.size());
    for (ArSymbol const& sym : archive.symbols) {
        ImportSurface row;
        row.mangledName = sym.name;
        row.libraryPath = std::string{archivePathLabel} + "("
                        + archive.members[sym.memberIndex].name + ")";
        row.kind        = SymbolKind::NoType;      // armap carries no fn/data kind
        row.visibility  = SymbolVisibility::Default;
        row.linkage     = SymbolLinkage::External; // an armap lists defined externals
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace dss::ffi
