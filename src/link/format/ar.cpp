#include "link/format/ar.hpp"

#include "core/types/parse_diagnostic.hpp"
#include "link/format/byte_emit.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// GNU / System V `ar` static-archive writer -- c163, D-LK-STATIC-ARCHIVE-
// WRITER. See ar.hpp for the full contract + byte layout. The inverse of the
// c161 reader (src/ffi/binary_readers/ar_reader.cpp); byte-audited against GNU
// `ar` 2.42.

namespace dss::link::format {

namespace {

using detail::appendU32BE;
using detail::emit;

constexpr std::size_t kArHdrSize = 60;
constexpr char kArMagic[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};

// The largest value the 10-byte ASCII-decimal `size` field can hold (10 nines).
constexpr std::uint64_t kMaxArFieldSize = 9999999999ull;   // 10^10 - 1
// The largest member-header offset the SysV armap's 32-bit BE offsets can name.
constexpr std::uint64_t kMaxArmapOffset = 0xFFFFFFFFull;

// A member name <= this many bytes stores inline as "name/" (name + the GNU
// short-name terminator '/', fitting the 16-byte ar_hdr name field); a longer
// name goes to the "//" long-name table as a "/N" back-reference.
constexpr std::size_t kMaxInlineNameLen = 15;

// Append an ASCII field left-justified, space-padded to `width`. The caller
// guarantees `s.size() <= width` (every field is pre-sized: a name is <= 16 by
// construction, mtime/uid/gid/mode are tiny constants, size is validated <=
// 10 digits before this call).
void appendField(std::vector<std::uint8_t>& out, std::string_view s,
                 std::size_t width) {
    for (char const c : s) out.push_back(static_cast<std::uint8_t>(c));
    for (std::size_t i = s.size(); i < width; ++i)
        out.push_back(static_cast<std::uint8_t>(' '));
}

// Append a 60-byte ar_hdr: name(16) mtime(12) uid(6) gid(6) mode(8) size(10)
// + the "`\n" terminator magic. Deterministic mtime/uid/gid = 0 (GNU `ar D`);
// `mode` is the octal mode string ("644" for a regular member, "0" for the
// "/"/"//" specials -- GNU convention). `payloadSize` is the ar_hdr `size`
// field: the member's payload length (the c161 reader reads exactly this).
void appendArHdr(std::vector<std::uint8_t>& out, std::string_view nameField,
                 std::string_view mode, std::uint64_t payloadSize) {
    appendField(out, nameField, 16);
    appendField(out, "0", 12);                       // mtime (deterministic)
    appendField(out, "0", 6);                        // uid
    appendField(out, "0", 6);                        // gid
    appendField(out, mode, 8);                       // mode (octal)
    appendField(out, std::to_string(payloadSize), 10);   // size (decimal)
    out.push_back(0x60u);                            // '`'
    out.push_back(0x0Au);                            // '\n'
}

// Emit a full member: its 60-byte ar_hdr + payload + a '\n' pad byte iff the
// payload length is odd (2-byte member alignment -- the next ar_hdr lands on an
// even file offset). The ar_hdr `size` field is the EXACT payload length; the
// pad byte is EXTERNAL (not counted in size) -- exactly what the c161 reader's
// `off += size & 1` advance expects.
void emitMember(std::vector<std::uint8_t>& out, std::string_view nameField,
                std::string_view mode, std::span<std::uint8_t const> payload) {
    appendArHdr(out, nameField, mode, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    if (payload.size() & 1u) out.push_back(0x0Au);   // '\n' 2-byte-align pad
}

// The file offset the NEXT member header lands at, given a header at `off`
// whose payload is `payloadLen` bytes: past the 60-byte header + the payload +
// the 0/1-byte even-alignment pad.
[[nodiscard]] std::uint64_t
nextHeaderOffset(std::uint64_t off, std::uint64_t payloadLen) noexcept {
    return off + kArHdrSize + payloadLen + (payloadLen & 1u);
}

// A member name byte that would corrupt the container framing: '/' is the GNU
// short-name terminator + the reserved "/"/"//"/"/N" special spellings; '\n'
// terminates a "//" long-name-table entry; '\0' would be stripped as trailing
// pad by the reader's name rstrip. A real object file name ("lib.o") carries
// none of these.
[[nodiscard]] bool isFramingReservedByte(char c) noexcept {
    return c == '/' || c == '\n' || c == '\0';
}

} // namespace

std::vector<std::uint8_t>
writeArArchive(std::span<ArMemberInput const> members,
               DiagnosticReporter&            reporter) {
    // -- Validate member names + size fields; classify short vs long names. --
    // A long-named member (name > 15 bytes) records its real name in the "//"
    // table and its ar_hdr name field becomes "/N" (N = byte offset into "//").
    std::vector<std::string> nameFields;        // the 16-byte name spelling per member
    nameFields.reserve(members.size());
    std::vector<std::uint8_t> longNameTable;    // the "//" payload ("<name>/\n" x)

    for (ArMemberInput const& m : members) {
        if (m.name.empty()) {
            emit(reporter, DiagnosticCode::K_ArchiveMemberNameInvalid,
                 "ar writer: a member has an empty file name -- an ar member "
                 "must carry a non-empty name (D-LK-STATIC-ARCHIVE-WRITER)");
            return {};
        }
        for (char const c : m.name) {
            if (isFramingReservedByte(c)) {
                emit(reporter, DiagnosticCode::K_ArchiveMemberNameInvalid,
                     "ar writer: member name '" + m.name + "' contains a byte "
                     "reserved by the archive framing ('/', newline, or NUL) -- "
                     "the container would mis-resolve it "
                     "(D-LK-STATIC-ARCHIVE-WRITER)");
                return {};
            }
        }
        if (m.objectBytes.size() > kMaxArFieldSize) {
            emit(reporter, DiagnosticCode::K_ArchiveFieldOverflow,
                 "ar writer: member '" + m.name + "' payload ("
                 + std::to_string(m.objectBytes.size())
                 + " bytes) exceeds the 10-digit ASCII ar_hdr size field "
                   "(D-LK-STATIC-ARCHIVE-WRITER)");
            return {};
        }

        if (m.name.size() <= kMaxInlineNameLen) {
            nameFields.push_back(m.name + "/");     // inline "name/"
        } else {
            // Long name: record "/N" then append "<name>/\n" to the "//" table.
            std::uint64_t const tableOffset = longNameTable.size();
            if (tableOffset > kMaxArmapOffset) {
                emit(reporter, DiagnosticCode::K_ArchiveFieldOverflow,
                     "ar writer: the '//' long-name table exceeds the 32-bit "
                     "offset a '/N' name reference can name "
                     "(D-LK-STATIC-ARCHIVE-WRITER)");
                return {};
            }
            nameFields.push_back("/" + std::to_string(tableOffset));
            for (char const c : m.name)
                longNameTable.push_back(static_cast<std::uint8_t>(c));
            longNameTable.push_back(static_cast<std::uint8_t>('/'));   // GNU terminator
            longNameTable.push_back(static_cast<std::uint8_t>('\n'));  // entry end
        }
    }
    if (longNameTable.size() > kMaxArFieldSize) {
        emit(reporter, DiagnosticCode::K_ArchiveFieldOverflow,
             "ar writer: the '//' long-name table exceeds the 10-digit ASCII "
             "ar_hdr size field (D-LK-STATIC-ARCHIVE-WRITER)");
        return {};
    }

    // -- Build the armap payload's NAME blob + record each symbol's member. --
    // The armap ("/" member) is a BE u32 count + BE u32 member-header offsets +
    // a NUL-terminated name blob. The offsets are back-filled once the layout
    // is known (below); here we gather the count + names + owning member index.
    std::vector<std::size_t> symbolMember;   // owning member index per armap symbol
    std::vector<std::uint8_t> armapNameBlob;
    for (std::size_t mi = 0; mi < members.size(); ++mi) {
        for (std::string const& sym : members[mi].exportedSymbols) {
            if (sym.empty()) {
                emit(reporter, DiagnosticCode::K_ArchiveMemberNameInvalid,
                     "ar writer: member '" + members[mi].name + "' exports an "
                     "empty symbol name -- an armap entry must be named "
                     "(D-LK-STATIC-ARCHIVE-WRITER)");
                return {};
            }
            for (char const c : sym) {
                if (c == '\0') {
                    emit(reporter, DiagnosticCode::K_ArchiveMemberNameInvalid,
                         "ar writer: an exported symbol name of member '"
                         + members[mi].name + "' contains a NUL, which would "
                         "truncate the armap name blob "
                         "(D-LK-STATIC-ARCHIVE-WRITER)");
                    return {};
                }
            }
            symbolMember.push_back(mi);
            for (char const c : sym)
                armapNameBlob.push_back(static_cast<std::uint8_t>(c));
            armapNameBlob.push_back(0u);   // NUL terminator
        }
    }
    std::size_t const symbolCount = symbolMember.size();
    if (symbolCount > kMaxArmapOffset) {
        emit(reporter, DiagnosticCode::K_ArchiveFieldOverflow,
             "ar writer: armap symbol count exceeds the 32-bit count field "
             "(D-LK-STATIC-ARCHIVE-WRITER)");
        return {};
    }
    // Armap payload = count(4) + offsets(4*n) + name blob.
    std::uint64_t const armapSize =
        4ull + 4ull * static_cast<std::uint64_t>(symbolCount)
        + static_cast<std::uint64_t>(armapNameBlob.size());
    if (armapSize > kMaxArFieldSize) {
        emit(reporter, DiagnosticCode::K_ArchiveFieldOverflow,
             "ar writer: the armap payload exceeds the 10-digit ASCII ar_hdr "
             "size field (D-LK-STATIC-ARCHIVE-WRITER)");
        return {};
    }

    // -- Compute each member's header offset (the layout pass). --
    // Order: magic (8) -> "/" armap -> "//" long-name table (iff any) -> the
    // member objects. Each member offset is deterministic from the sizes ahead
    // of it, so the armap offsets can be filled before emission.
    bool const emitLongTable = !longNameTable.empty();
    std::uint64_t off = sizeof(kArMagic);
    off = nextHeaderOffset(off, armapSize);            // past the "/" armap
    if (emitLongTable)
        off = nextHeaderOffset(off, longNameTable.size());   // past the "//" table
    std::vector<std::uint64_t> memberHeaderOffset;
    memberHeaderOffset.reserve(members.size());
    for (ArMemberInput const& m : members) {
        if (off > kMaxArmapOffset) {
            emit(reporter, DiagnosticCode::K_ArchiveFieldOverflow,
                 "ar writer: member '" + m.name + "' header offset "
                 + std::to_string(off) + " exceeds the SysV armap's 32-bit "
                 "offset range (a >4 GiB archive needs the GNU '/SYM64/' "
                 "64-bit armap, out of scope -- see D-FF1-AR-BSD-VARIANT)");
            return {};
        }
        memberHeaderOffset.push_back(off);
        off = nextHeaderOffset(off, m.objectBytes.size());
    }

    // -- Emit. --
    std::vector<std::uint8_t> out;
    for (char const c : kArMagic) out.push_back(static_cast<std::uint8_t>(c));

    // "/" armap: header (mode 0) + BE count + BE per-symbol member offsets +
    // the NUL-terminated name blob. Always emitted (the index is the point).
    {
        std::vector<std::uint8_t> armap;
        armap.reserve(static_cast<std::size_t>(armapSize));
        appendU32BE(armap, static_cast<std::uint32_t>(symbolCount));
        for (std::size_t s = 0; s < symbolCount; ++s) {
            appendU32BE(armap, static_cast<std::uint32_t>(
                                   memberHeaderOffset[symbolMember[s]]));
        }
        armap.insert(armap.end(), armapNameBlob.begin(), armapNameBlob.end());
        emitMember(out, "/", "0", armap);
    }

    // "//" GNU long-name table (mode 0), iff any member name needed it.
    if (emitLongTable) emitMember(out, "//", "0", longNameTable);

    // The member objects (mode 644 -- a regular rw-r--r-- member).
    for (std::size_t mi = 0; mi < members.size(); ++mi) {
        emitMember(out, nameFields[mi], "644", members[mi].objectBytes);
    }

    return out;
}

} // namespace dss::link::format
