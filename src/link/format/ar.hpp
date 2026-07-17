#pragma once

#include "core/export.hpp"
#include "core/types/diagnostic_reporter.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// GNU / System V `ar` static-archive WRITER -- c163, the D-LK-STATIC-ARCHIVE-
// WRITER anchor (roadmap "write STATIC libs for all platforms"; the writer
// half of D-FF1-AR-WRITER-STATIC-LINK).
//
// The EXACT INVERSE of the c161 `ar` READER (src/ffi/binary_readers/
// ar_reader.cpp): the reader parses an `!<arch>` container + its SysV/GNU
// armap into a member list + symbol index; this writer takes N relocatable
// object members (each: file name + opaque `.o` bytes + its exported-symbol
// list) and PRODUCES those bytes. The writer's output round-trips through the
// c161 reader by construction -- that pairing is the strongest self-contained
// oracle (writer <-> shipped reader). Byte-audited against GNU `ar` 2.42.
//
// FORMAT-BLIND. The `ar` container is identical whether its members are ELF
// ET_REL, Mach-O MH_OBJECT, or COFF `.obj` objects -- the framing never
// inspects a member's bytes or its object format. The armap (symbol index) is
// built purely from the members' exported-symbol NAME lists, which the object
// writers already know (`AssembledModule.symbols`, filtered by
// `isExternallyVisible`). So there is NO source/target/object-format branch in
// this writer -- it is the archive-container tier, one level above the
// per-format ELF/PE/Mach-O walkers.
//
// Layout produced (GNU/System V, Linux + macOS both use this form):
//   [0..7]   global magic "!<arch>\n"
//   "/"      the armap (SysV symbol index), ALWAYS emitted first:
//              u32 count (BIG-endian)
//              u32 member-header-offset x count (BIG-endian) -- each exported
//                  symbol -> the file offset of its DEFINING member's ar_hdr
//              count NUL-terminated symbol names
//   "//"     the GNU long-name string table, emitted IFF any member name is
//            longer than 15 bytes (will not fit "name/" in the 16-byte field):
//              "<name>/\n" per long-named member
//   members  each: a 60-byte ar_hdr (name / mtime / uid / gid / mode / size /
//            "`\n" terminator) + the member's `.o` bytes. A member name
//            <= 15 bytes is stored inline as "name/"; a longer name becomes
//            "/N" (N = the byte offset into the "//" table).
//
// EVERY member (the "/" armap, the "//" table, and the object members alike)
// uses the SAME rule: the ar_hdr `size` field is the EXACT payload length, and
// a single '\n' pad byte follows an odd payload so the next ar_hdr lands on an
// even file offset (the universal 2-byte `ar` member alignment; the pad is
// external -- NOT counted in `size`). This is the c161 reader's `off += size &
// 1` advance, and GNU `ar`/`nm`/`ld` accept it (W2). (GNU itself pads the "/"
// armap + "//" table INTERNALLY instead -- a trailing NUL / '\n' counted in
// `size` -- but both land the next member identically; a reader cannot tell
// them apart, and the object-member bytes differ from GNU's anyway, so
// byte-identity with GNU is neither a goal nor achievable.)
//
// Header field values match GNU deterministic `ar` (`ar D`, the modern distro
// default): mtime = uid = gid = 0; a regular member's mode = 644 (octal
// rw-r--r--); the "/" + "//" special members' mode = 0. These fields are
// cosmetic to any conforming reader (the c161 reader consults only name +
// size), but matching GNU keeps the writer a faithful drop-in producer.
//
// Fail-loud discipline (mirrors the reader's): a member name that is empty or
// carries a '/' or '\n' (would corrupt the framing) fails
// `K_ArchiveMemberNameInvalid`; an empty exported-symbol name does too (an
// armap entry with no name is a caller bug). A payload larger than the 10-digit
// ASCII size field, or a member-header offset past the SysV armap's 32-bit
// range (a >4 GiB archive -- the GNU `/SYM64/` case, out of scope), fails
// `K_ArchiveFieldOverflow`. On any such failure the returned byte vector is
// empty and the reporter carries the diagnostic; callers check
// `reporter.errorCount()`.
//
// Windows COFF `.lib` is NOT produced here: a COFF import/static library adds a
// SECOND linker member (Microsoft's little-endian sorted symbol->member index)
// on top of this first SysV "/" member. That is the named follow-up
// D-FF1-AR-COFF-WRITER; this writer ships the SysV/GNU `.a` that Linux + macOS
// consume.

namespace dss::link::format {

// One relocatable object member to bundle into the archive.
struct DSS_EXPORT ArMemberInput {
    // The member's file name recorded in the ar_hdr (e.g. "lib.o"). A name
    // <= 15 bytes is stored inline; a longer name goes to the GNU "//" table.
    // Must be non-empty and free of '/' and '\n' (framing-reserved bytes).
    std::string name;
    // The member's opaque object bytes (an ELF ET_REL / Mach-O MH_OBJECT /
    // COFF .obj -- the writer never inspects them).
    std::vector<std::uint8_t> objectBytes;
    // The member's DEFINED, externally-visible symbol names -- exactly the set
    // a foreign linker resolves an unresolved extern against (the armap union).
    // The compile pipeline derives this from `AssembledModule.symbols` filtered
    // by `isExternallyVisible`. Order is preserved into the armap (per-member,
    // in the order given). May be empty (a member contributing no armap rows).
    std::vector<std::string> exportedSymbols;
};

// The archive index flavor.
//   * `SysV` -- GNU / System V `.a` (Linux + macOS): the single "/" armap
//     (big-endian). The GNU-compatible default (byte-audited against GNU `ar`).
//   * `Coff` -- Windows `.lib` (D-FF1-AR-COFF-WRITER, c169): the SAME "/"
//     first linker member (big-endian) PLUS a SECOND "/" linker member --
//     Microsoft's LITTLE-endian SORTED symbol->member index (`link.exe`
//     PREFERS it) -- emitted between the first armap and the "//" table. A
//     `.lib` is a strict SUPERSET of the SysV `.a`; the extra member never
//     changes an object member's bytes, only the on-disk offsets.
enum class ArArchiveFlavor : std::uint8_t {
    SysV = 0,
    Coff = 1,
};

// Bundle `members` into an `ar` archive (`!<arch>` + "/" first-linker-member
// armap + optional COFF "/" second-linker-member index + optional "//"
// long-name table + the member objects). Returns the archive bytes on success;
// an EMPTY vector with a diagnostic on `reporter` on any fail-loud belt (see
// the file docblock). An empty `members` span yields a valid archive with just
// the magic + an empty (count-0) armap. `flavor` selects the SysV `.a` (the
// GNU-compatible default) or the Windows COFF `.lib` (adds the second linker
// member) -- the framing + member bytes are byte-identical, only the index
// members (and thus the member offsets) differ.
[[nodiscard]] DSS_EXPORT std::vector<std::uint8_t>
writeArArchive(std::span<ArMemberInput const> members,
               DiagnosticReporter&            reporter,
               ArArchiveFlavor                flavor = ArArchiveFlavor::SysV);

} // namespace dss::link::format
