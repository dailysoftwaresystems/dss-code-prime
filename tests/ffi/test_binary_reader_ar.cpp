// Plan 11 FF1 roadmap B3 tests -- the `ar` static-archive reader
// (`dss::ffi::readArArchive` + the `readImportsFromBytes` dispatch
// projection). Anchor D-FF1-AR-READER.
//
// Pins:
//   * Round-trip a hand-built GNU/System V archive (magic + "/" armap +
//     2 object members) -> exact member list + armap symbol->member map.
//     Red-on-disable: dropping the Pass-3 armap parse empties
//     `symbols` and fails the size/mapping asserts here.
//   * GNU long-name resolution: a "/N" name field -> the "//" table.
//   * Dispatch projection: `readImportsFromBytes` on ar magic routes to
//     the ar reader + projects the armap to ImportSurface rows
//     ("<archive>(<member>)" libraryPath, NoType kind, External linkage).
//   * No-armap archive: members parse, surface is empty (success).
//   * Empty-armap-name skip+warn (F_BinaryReaderPartialCorruption).
//   * Failure modes each fail loud CorruptedBinary: bad magic,
//     member-header-past-EOF, non-numeric size, member-data-past-EOF,
//     armap-offset matches no member, "/N" long-name offset past the
//     "//" table, truncated armap name blob, BSD/__SYMDEF + SYM64
//     variant rejection.
//
// Strategy: synthesize minimal archives directly in C++ (byte-exact to
// the GNU `ar` 2.42 layout audited for W1/W2) + read them back. No real
// `.a` files shipped in-test (CI hermeticity); the WSL round-trip
// witnesses cover real archives.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "ffi/binary_reader.hpp"
#include "ffi/binary_readers/ar_reader.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::ffi;

namespace {

// Big-endian u32 emit/patch (the armap is the one BE structure; the
// shared test byte_emit helpers are LE-only).
void appU32BE(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >>  8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>( v        & 0xFF));
}
void putU32BE(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    b[off + 0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
    b[off + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    b[off + 2] = static_cast<std::uint8_t>((v >>  8) & 0xFF);
    b[off + 3] = static_cast<std::uint8_t>( v        & 0xFF);
}

// Append one field (ASCII, space-padded to `width`).
void appField(std::vector<std::uint8_t>& b, std::string s, std::size_t width) {
    s.resize(width, ' ');
    for (char const c : s) b.push_back(static_cast<std::uint8_t>(c));
}

// Append a 60-byte ar_hdr: name(16) mtime(12) uid(6) gid(6) mode(8)
// size(10) + "`\n". Only name + size matter to the reader.
void appArHdr(std::vector<std::uint8_t>& b, std::string nameField,
              std::uint64_t size) {
    appField(b, std::move(nameField), 16);
    appField(b, "0", 12);   // mtime
    appField(b, "0", 6);    // uid
    appField(b, "0", 6);    // gid
    appField(b, "0", 8);    // mode
    appField(b, std::to_string(size), 10);
    b.push_back(0x60);
    b.push_back(0x0A);
}

// Emit a full member (header + payload + a '\n' pad if odd). Returns the
// member's header offset.
std::size_t emitMember(std::vector<std::uint8_t>& b, std::string nameField,
                       std::span<std::uint8_t const> data) {
    std::size_t const hdrOff = b.size();
    appArHdr(b, std::move(nameField), data.size());
    b.insert(b.end(), data.begin(), data.end());
    if (data.size() & 1u) b.push_back(0x0A);
    return hdrOff;
}

struct MemberSpec {
    std::string               nameField;  // the raw 16-byte name field (e.g. "a.o/" or "/0")
    std::vector<std::uint8_t> data;
};
struct SymSpec {
    std::string name;
    std::size_t memberIndex;   // index into MemberSpec list
};

struct BuiltAr {
    std::vector<std::uint8_t> bytes;
    std::size_t               armapOffsetsPos = 0;   // start of the armap BE-u32 offset array
    std::vector<std::size_t>  memberHeaderOffsets;   // header offset of each real member
    std::vector<std::size_t>  memberNameFieldPos;    // name-field offset of each real member
    std::size_t               longnamesDataPos = 0;
};

// Build a GNU/System V archive: magic, then (if symbols) a "/" armap,
// then (if longnames) a "//" table, then the members. Back-patches the
// armap offsets to the real member header offsets.
BuiltAr buildAr(std::vector<MemberSpec> const& members,
                std::vector<SymSpec> const&    symbols,
                std::vector<std::uint8_t> const& longnames = {}) {
    BuiltAr out;
    auto& b = out.bytes;
    char const magic[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
    for (char const c : magic) b.push_back(static_cast<std::uint8_t>(c));

    if (!symbols.empty()) {
        std::vector<std::uint8_t> ar;
        appU32BE(ar, static_cast<std::uint32_t>(symbols.size()));
        std::size_t const offArrayRel = ar.size();          // == 4
        for (std::size_t i = 0; i < symbols.size(); ++i) appU32BE(ar, 0);
        for (SymSpec const& s : symbols) {
            for (char const c : s.name) ar.push_back(static_cast<std::uint8_t>(c));
            ar.push_back(0);
        }
        std::size_t const hdrOff = emitMember(b, "/", ar);
        out.armapOffsetsPos = hdrOff + 60 + offArrayRel;
    }
    if (!longnames.empty()) {
        std::size_t const hdrOff = emitMember(b, "//", longnames);
        out.longnamesDataPos = hdrOff + 60;
    }
    for (MemberSpec const& m : members) {
        std::size_t const hdrOff = emitMember(b, m.nameField, m.data);
        out.memberHeaderOffsets.push_back(hdrOff);
        out.memberNameFieldPos.push_back(hdrOff);   // name field is at header start
    }
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        putU32BE(b, out.armapOffsetsPos + i * 4,
                 static_cast<std::uint32_t>(
                     out.memberHeaderOffsets[symbols[i].memberIndex]));
    }
    return out;
}

// The canonical W1 archive: members a.o (defines foo, bar_data) + b.o
// (defines baz). Payloads are opaque dummy bytes -- the armap test does
// not parse member contents.
BuiltAr buildW1() {
    std::vector<std::uint8_t> aData(20, 0xAA);
    std::vector<std::uint8_t> bData(14, 0xBB);
    return buildAr(
        {{"a.o/", aData}, {"b.o/", bData}},
        {{"foo", 0}, {"bar_data", 0}, {"baz", 1}});
}

} // namespace

// -- Round-trip: member list + armap symbol->member map -----------

TEST(BinaryReaderAr, RoundTripMembersAndArmap) {
    auto built = buildW1();
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "libtest.a", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;

    // Member list == [a.o, b.o] with correct sizes (specials excluded).
    ASSERT_EQ(r->members.size(), 2u);
    EXPECT_EQ(r->members[0].name, "a.o");
    EXPECT_EQ(r->members[0].size, 20u);
    EXPECT_EQ(r->members[1].name, "b.o");
    EXPECT_EQ(r->members[1].size, 14u);

    // Armap: foo/bar_data -> a.o (index 0); baz -> b.o (index 1).
    // Red-on-disable: dropping the Pass-3 armap parse empties this.
    ASSERT_EQ(r->symbols.size(), 3u);
    EXPECT_EQ(r->symbols[0].name, "foo");
    EXPECT_EQ(r->symbols[0].memberIndex, 0u);
    EXPECT_EQ(r->symbols[1].name, "bar_data");
    EXPECT_EQ(r->symbols[1].memberIndex, 0u);
    EXPECT_EQ(r->symbols[2].name, "baz");
    EXPECT_EQ(r->symbols[2].memberIndex, 1u);
    // The recorded member offset must match the member's header offset.
    EXPECT_EQ(r->symbols[2].memberOffset, r->members[1].headerOffset);
}

// -- Dispatch projection: readImportsFromBytes -> armap surface -----

TEST(BinaryReaderAr, DispatchProjectsArmapToImportSurface) {
    auto built = buildW1();
    DiagnosticReporter rep;
    auto r = readImportsFromBytes(built.bytes, "libtest.a", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->size(), 3u);

    EXPECT_EQ((*r)[0].mangledName, "foo");
    EXPECT_EQ((*r)[0].libraryPath, "libtest.a(a.o)");
    EXPECT_EQ((*r)[0].kind, SymbolKind::NoType);       // armap carries no fn/data kind
    EXPECT_EQ((*r)[0].linkage, SymbolLinkage::External);
    EXPECT_EQ((*r)[0].visibility, SymbolVisibility::Default);
    EXPECT_EQ((*r)[1].mangledName, "bar_data");
    EXPECT_EQ((*r)[1].libraryPath, "libtest.a(a.o)");
    EXPECT_EQ((*r)[2].mangledName, "baz");
    EXPECT_EQ((*r)[2].libraryPath, "libtest.a(b.o)");
}

// -- GNU long-name resolution ("/N" -> "//" table) -----------------

TEST(BinaryReaderAr, GnuLongNameResolvesFromTable) {
    std::string const longName = "this_is_a_very_long_member_name.o";
    std::vector<std::uint8_t> table;              // "<name>/\n" (GNU terminator)
    for (char const c : longName) table.push_back(static_cast<std::uint8_t>(c));
    table.push_back('/');
    table.push_back('\n');

    std::vector<std::uint8_t> data(8, 0xCC);
    auto built = buildAr({{"/0", data}}, {{"foo", 0}}, table);
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "liblong.a", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->members.size(), 1u);
    EXPECT_EQ(r->members[0].name, longName);
    ASSERT_EQ(r->symbols.size(), 1u);
    EXPECT_EQ(r->symbols[0].name, "foo");
    EXPECT_EQ(r->symbols[0].memberIndex, 0u);
}

// Multiple long-named members resolve from ONE tokenized "//" table --
// the real-world path (libc.a W3 has many "/N" members). Two entries
// "aaa.o/\n" (start 0) + "bbbbbb.o/\n" (start 7); members "/0" + "/7".
TEST(BinaryReaderAr, MultipleLongNamesResolveFromOneTable) {
    std::vector<std::uint8_t> table;
    for (char const c : std::string("aaa.o/\n"))    table.push_back(static_cast<std::uint8_t>(c));   // [0..6]
    for (char const c : std::string("bbbbbb.o/\n")) table.push_back(static_cast<std::uint8_t>(c));   // [7..]
    std::vector<std::uint8_t> d1(4, 0x01), d2(4, 0x02);
    auto built = buildAr({{"/0", d1}, {"/7", d2}}, {{"a_sym", 0}, {"b_sym", 1}}, table);
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "libmulti.a", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->members.size(), 2u);
    EXPECT_EQ(r->members[0].name, "aaa.o");
    EXPECT_EQ(r->members[1].name, "bbbbbb.o");
    ASSERT_EQ(r->symbols.size(), 2u);
    EXPECT_EQ(r->symbols[1].memberIndex, 1u);   // b_sym -> bbbbbb.o
}

// A "/N" offset that lands MID-ENTRY (not at an entry start) fails loud
// -- the tokenize-once correctness gain. Table "foobar.o/\n" is ONE
// entry at offset 0; "/3" points at the 'b' inside it. The OLD per-member
// rescan silently returned the shifted substring "bar.o"; the new O(1)
// map lookup misses -> CorruptedBinary. RED-ON-DISABLE the tokenize path.
TEST(BinaryReaderAr, MidEntryLongNameOffsetFailsLoud) {
    std::vector<std::uint8_t> table;
    for (char const c : std::string("foobar.o/\n")) table.push_back(static_cast<std::uint8_t>(c));
    std::vector<std::uint8_t> data(4, 0x55);
    auto built = buildAr({{"/3", data}}, /*symbols=*/{}, table);
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "midentry.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("does not begin a '//' table entry"),
              std::string::npos);
}

// Quadratic-DoS guard: a crafted "//" table with NO terminator + many
// "/0" members. The OLD code rescanned the whole table per member
// (O(members x table-bytes) time hang); the tokenize-once pass detects
// the unterminated table in ONE linear scan and fails loud FAST, before
// any member is resolved. RED-ON-DISABLE: the old rescan would instead
// resolve each "/0" to the whole-table string and SUCCEED (no fail-loud).
TEST(BinaryReaderAr, UnterminatedLongNameTableFailsLoudNotQuadratic) {
    std::vector<std::uint8_t> table(512, 0x41);   // 512 'A's, NO '\n'
    std::vector<MemberSpec> members;
    for (int i = 0; i < 300; ++i)
        members.push_back({"/0", std::vector<std::uint8_t>(2, 0x66)});
    auto built = buildAr(members, /*symbols=*/{}, table);
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "dos.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("unterminated final entry"),
              std::string::npos);
}

// -- Archive with no armap: members parse, surface empty ----------

TEST(BinaryReaderAr, NoArmapYieldsEmptySurfaceButMembersParse) {
    std::vector<std::uint8_t> data(6, 0xDD);
    auto built = buildAr({{"only.o/", data}}, /*symbols=*/{});
    DiagnosticReporter rep;

    auto rich = readArArchive(built.bytes, "libnoidx.a", rep);
    ASSERT_TRUE(rich.has_value()) << rich.error().detail;
    ASSERT_EQ(rich->members.size(), 1u);
    EXPECT_EQ(rich->members[0].name, "only.o");
    EXPECT_TRUE(rich->symbols.empty());

    auto surf = readImportsFromBytes(built.bytes, "libnoidx.a", rep);
    ASSERT_TRUE(surf.has_value()) << surf.error().detail;
    EXPECT_TRUE(surf->empty());
}

// -- Empty-armap-name skip+warn (partial-corruption discipline) ---

TEST(BinaryReaderAr, EmptyArmapNameSkippedWithWarning) {
    // Two symbols; the second has an empty name (a lone NUL). It is
    // skipped + counted, the first surfaces.
    std::vector<std::uint8_t> data(4, 0xEE);
    auto built = buildAr({{"m.o/", data}}, {{"good", 0}, {"", 0}});
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "libskip.a", rep);
    ASSERT_TRUE(r.has_value()) << r.error().detail;
    ASSERT_EQ(r->symbols.size(), 1u);
    EXPECT_EQ(r->symbols[0].name, "good");

    bool sawPartial = false;
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::F_BinaryReaderPartialCorruption) {
            sawPartial = true;
            EXPECT_EQ(d.severity, DiagnosticSeverity::Warning);
            EXPECT_NE(d.actual.find("skipped 1"), std::string::npos)
                << "actual: " << d.actual;
        }
    }
    EXPECT_TRUE(sawPartial)
        << "empty armap name must fire F_BinaryReaderPartialCorruption";
}

// -- Failure modes (each fails loud CorruptedBinary) --------------

TEST(BinaryReaderAr, BadMagicRejected) {
    auto built = buildW1();
    built.bytes[1] = 'X';   // corrupt the '<' of "!<arch>\n"
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "bad.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("bad archive magic"), std::string::npos);
}

TEST(BinaryReaderAr, ShortBufferRejected) {
    std::vector<std::uint8_t> tiny{'!', '<', 'a', 'r'};   // 4 bytes
    DiagnosticReporter rep;
    auto r = readArArchive(tiny, "tiny.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
}

TEST(BinaryReaderAr, MemberHeaderPastEofRejected) {
    auto built = buildW1();
    // Truncate mid the LAST member's 60-byte header.
    std::size_t const lastHdr = built.memberHeaderOffsets.back();
    built.bytes.resize(lastHdr + 30);
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "trunc.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("runs past EOF"), std::string::npos);
}

TEST(BinaryReaderAr, NonNumericSizeFieldRejected) {
    auto built = buildW1();
    // Poison member[0]'s size field (name(16)+mtime(12)+uid(6)+gid(6)+
    // mode(8) = 48 -> size at header+48).
    std::size_t const sizeOff = built.memberHeaderOffsets[0] + 48;
    built.bytes[sizeOff] = 'x';
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "badsize.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("non-numeric size"), std::string::npos);
}

TEST(BinaryReaderAr, MemberDataPastEofRejected) {
    auto built = buildW1();
    // Overwrite member[0]'s size field with a huge value.
    std::size_t const sizeOff = built.memberHeaderOffsets[0] + 48;
    std::string const huge = "999999999";     // 9 digits, fits the 10-byte field
    for (std::size_t i = 0; i < huge.size(); ++i)
        built.bytes[sizeOff + i] = static_cast<std::uint8_t>(huge[i]);
    built.bytes[sizeOff + huge.size()] = ' ';
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "bigsize.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("runs past EOF"), std::string::npos);
}

TEST(BinaryReaderAr, ArmapOffsetMatchingNoMemberRejected) {
    auto built = buildW1();
    // Point the first armap offset at nowhere.
    putU32BE(built.bytes, built.armapOffsetsPos, 0xFFFFFFF0u);
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "badidx.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("matches no archive member"),
              std::string::npos);
}

TEST(BinaryReaderAr, LongNameOffsetPastTableRejected) {
    std::vector<std::uint8_t> table;
    std::string const nm = "long_name.o";
    for (char const c : nm) table.push_back(static_cast<std::uint8_t>(c));
    table.push_back('/'); table.push_back('\n');
    std::vector<std::uint8_t> data(4, 0x11);
    auto built = buildAr({{"/0", data}}, {{"sym", 0}}, table);
    // Rewrite the member name field "/0" -> "/999" (offset past the table).
    std::size_t const nameOff = built.memberNameFieldPos[0];
    std::string const bad = "/999";
    for (std::size_t i = 0; i < 16; ++i)
        built.bytes[nameOff + i] =
            static_cast<std::uint8_t>(i < bad.size() ? bad[i] : ' ');
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "badlong.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    // Offset 999 is not an entry start in the tokenized "//" map -> the
    // map-miss fail-loud path (same as a mid-entry offset).
    EXPECT_NE(r.error().detail.find("does not begin a '//' table entry"),
              std::string::npos);
}

TEST(BinaryReaderAr, TruncatedArmapNameBlobRejected) {
    // Hand-craft an armap whose count (2) exceeds the names present (1),
    // with everything else in-bounds -> the Pass-3 blob walk fails loud
    // (distinct from the Pass-1 member-data-past-EOF check).
    std::vector<std::uint8_t> b;
    char const magic[8] = {'!', '<', 'a', 'r', 'c', 'h', '>', '\n'};
    for (char const c : magic) b.push_back(static_cast<std::uint8_t>(c));
    // The real member's header offset is deterministic:
    //   8 (magic) + 60 (armap header) + 16 (armap data) = 84.
    std::uint32_t const memberHdrOff = 84;
    std::vector<std::uint8_t> ar;
    appU32BE(ar, 2);              // count = 2 ...
    appU32BE(ar, memberHdrOff);   // offset[0]
    appU32BE(ar, memberHdrOff);   // offset[1]
    for (char const c : std::string("foo")) ar.push_back(static_cast<std::uint8_t>(c));
    ar.push_back(0);              // ... but only ONE name ("foo\0"); ar.size()==16
    emitMember(b, "/", ar);
    std::vector<std::uint8_t> data(4, 0x44);
    std::size_t const hdrOff = emitMember(b, "m.o/", data);
    ASSERT_EQ(hdrOff, static_cast<std::size_t>(memberHdrOff));  // layout sanity
    DiagnosticReporter rep;
    auto r = readArArchive(b, "trunc-armap.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("name blob ends"), std::string::npos);
}

TEST(BinaryReaderAr, BsdSymdefRejectedCleanly) {
    std::vector<std::uint8_t> data(4, 0x22);
    auto built = buildAr({{"__.SYMDEF", data}}, /*symbols=*/{});
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "bsd.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("BSD-variant"), std::string::npos);
    EXPECT_NE(r.error().detail.find("D-FF1-AR-BSD-VARIANT"), std::string::npos);
}

TEST(BinaryReaderAr, Sym64ArmapRejectedCleanly) {
    std::vector<std::uint8_t> data(4, 0x33);
    auto built = buildAr({{"/SYM64/", data}}, /*symbols=*/{});
    DiagnosticReporter rep;
    auto r = readArArchive(built.bytes, "sym64.a", rep);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, BinaryReadErrorKind::CorruptedBinary);
    EXPECT_NE(r.error().detail.find("SYM64"), std::string::npos);
}
